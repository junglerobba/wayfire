#include <cassert>
#include <algorithm>
#include "surface-map-state.hpp"

extern "C"
{
#include <wlr/types/wlr_seat.h>
}

#include "signal-definitions.hpp"
#include "../core-impl.hpp"
#include "../../output/output-impl.hpp"
#include "touch.hpp"
#include "keyboard.hpp"
#include "cursor.hpp"
#include "input-manager.hpp"
#include "output-layout.hpp"
#include "workspace-manager.hpp"
#include "debug.hpp"

#include "switch.hpp"
#include "tablet.hpp"
#include "pointing-device.hpp"

bool input_manager::is_touch_enabled()
{
    return touch_count > 0;
}

void input_manager::update_capabilities()
{
    uint32_t cap = 0;
    if (pointer_count)
        cap |= WL_SEAT_CAPABILITY_POINTER;
    if (keyboards.size())
        cap |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (touch_count)
        cap |= WL_SEAT_CAPABILITY_TOUCH;

    wlr_seat_set_capabilities(seat, cap);
}

static std::unique_ptr<wf_input_device_internal> create_wf_device_for_device(
    wlr_input_device *device)
{
    switch (device->type)
    {
        case WLR_INPUT_DEVICE_SWITCH:
            return std::make_unique<wf::switch_device_t> (device);
        case WLR_INPUT_DEVICE_POINTER:
            return std::make_unique<wf::pointing_device_t> (device);
        case WLR_INPUT_DEVICE_TABLET_TOOL:
            return std::make_unique<wf::tablet_t> (
                wf::get_core_impl().input->cursor->cursor, device);
        default:
            return std::make_unique<wf_input_device_internal> (device);
    }
}

void input_manager::handle_new_input(wlr_input_device *dev)
{
    log_info("handle new input: %s, default mapping: %s",
        dev->name, dev->output_name);
    input_devices.push_back(create_wf_device_for_device(dev));

    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
    {
        keyboards.push_back(
            std::make_unique<wf_keyboard> (dev, wf::get_core().config));
    }

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        cursor->attach_device(dev);
        pointer_count++;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
    {
        touch_count++;
        if (!our_touch)
            our_touch = std::unique_ptr<wf_touch> (new wf_touch(cursor->cursor));

        our_touch->add_device(dev);
    }

    auto section = wf::get_core().config->get_section(nonull(dev->name));
    auto mapped_output = section->get_option("output",
        nonull(dev->output_name))->as_string();

    auto wo = wf::get_core().output_layout->find_output(mapped_output);
    if (wo)
        wlr_cursor_map_input_to_output(cursor->cursor, dev, wo->handle);

    update_capabilities();

    wf::input_device_signal data;
    data.device = nonstd::make_observer(input_devices.back().get());
    wf::get_core().emit_signal("input-device-added", &data);
}

void input_manager::handle_input_destroyed(wlr_input_device *dev)
{
    log_info("remove input: %s", dev->name);

    auto it = std::remove_if(input_devices.begin(), input_devices.end(),
        [=] (const std::unique_ptr<wf_input_device_internal>& idev) {
            return idev->get_wlr_handle() == dev;
        });

    // devices should be unique
    wf::input_device_signal data;
    data.device = nonstd::make_observer(it->get());
    wf::get_core().emit_signal("input-device-removed", &data);

    input_devices.erase(it, input_devices.end());

    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
    {
        auto it = std::remove_if(keyboards.begin(), keyboards.end(),
            [=] (const std::unique_ptr<wf_keyboard>& kbd) { return kbd->device == dev; });

        keyboards.erase(it, keyboards.end());
    }

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        cursor->detach_device(dev);
        pointer_count--;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
        touch_count--;

    update_capabilities();
}

input_manager::input_manager()
{
    wf::pointing_device_t::config.load(wf::get_core().config);

    input_device_created.set_callback([&] (void *data) {
        auto dev = static_cast<wlr_input_device*> (data);
        assert(dev);
        wf::get_core_impl().input->handle_new_input(dev);
    });
    input_device_created.connect(&wf::get_core().backend->events.new_input);

    create_seat();
    surface_map_state_changed = [=] (wf::signal_data_t *data)
    {
        auto ev = static_cast<_surface_map_state_changed_signal*> (data);
        if (our_touch)
        {
            if (ev && our_touch->grabbed_surface == ev->surface && !ev->surface->is_mapped())
                our_touch->end_touch_down_grab();

            auto touch_points = our_touch->gesture_recognizer.current;
            for (auto f : touch_points)
            {
                our_touch->gesture_recognizer.update_touch(get_current_time(),
                    f.first, f.second.current, false);
            }
        }
    };
    wf::get_core().connect_signal("_surface_mapped", &surface_map_state_changed);
    wf::get_core().connect_signal("_surface_unmapped", &surface_map_state_changed);

    config_updated = [=] (wf::signal_data_t *)
    {
        for (auto& dev : input_devices)
            dev->update_options();
        for (auto& kbd : keyboards)
            kbd->reload_input_options();
    };

    wf::get_core().connect_signal("reload-config", &config_updated);

    output_added = [=] (wf::signal_data_t *data)
    {
        auto wo = (wf::output_impl_t*)get_signaled_output(data);
        if (exclusive_client != nullptr)
            wo->inhibit_plugins();
    };
    wf::get_core().output_layout->connect_signal("output-added", &output_added);
}

input_manager::~input_manager()
{
    wf::get_core().disconnect_signal("reload-config", &config_updated);
    wf::get_core().disconnect_signal("_surface_mapped",
        &surface_map_state_changed);
    wf::get_core().disconnect_signal("_surface_unmapped",
        &surface_map_state_changed);
    wf::get_core().output_layout->disconnect_signal(
        "output-added", &output_added);
}

uint32_t input_manager::get_modifiers()
{
    uint32_t mods = 0;
    auto keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard)
        mods = wlr_keyboard_get_modifiers(keyboard);

    return mods;
}

bool input_manager::grab_input(wf::plugin_grab_interface_t* iface)
{
    if (!iface || !iface->is_grabbed())
        return false;

    assert(!active_grab); // cannot have two active input grabs!

    if (our_touch)
        our_touch->input_grabbed();

    active_grab = iface;

    auto kbd = wlr_seat_get_keyboard(seat);
    auto mods = kbd ? kbd->modifiers : wlr_keyboard_modifiers {0, 0, 0, 0};
    mods.depressed = 0;
    wlr_seat_keyboard_send_modifiers(seat, &mods);

    set_keyboard_focus(nullptr, seat);
    lpointer->set_enable_focus(false);
    wf::get_core().set_cursor("default");
    return true;
}

void input_manager::ungrab_input()
{
    active_grab = nullptr;
    wf::get_core().set_active_view(
        wf::get_core().get_active_output()->get_active_view());

    /* We must update cursor focus, however, if we update "too soon", the current
     * pointer event (button press/release, maybe something else) will be sent to
     * the client, which shouldn't happen (at the time of the event, there was
     * still an active input grab) */
    idle_update_cursor.run_once([&] () {
        lpointer->set_enable_focus(true);
    });
}

bool input_manager::input_grabbed()
{
    return active_grab;
}

bool input_manager::can_focus_surface(wf::surface_interface_t *surface)
{
    if (exclusive_client && surface->get_client() != exclusive_client)
    {
        /* We have exclusive focus surface, for ex. a lockscreen.
         * The only kind of things we can focus are OSKs and similar */
        auto view = (wf::view_interface_t*) surface->get_main_surface();
        if (view && view->get_output()) {
            auto layer =
                view->get_output()->workspace->get_view_layer(view->self());
            return  layer == wf::LAYER_DESKTOP_WIDGET;
        }
        return false;
    }

    return true;
}

wf::surface_interface_t* input_manager::input_surface_at(wf_pointf global,
    wf_pointf& local)
{
    auto output = wf::get_core().output_layout->get_output_coords_at(global, global);
    /* If the output at these coordinates was just destroyed or some other edge case */
    if (!output)
        return nullptr;

    auto og = output->get_layout_geometry();
    global.x -= og.x;
    global.y -= og.y;

    for (auto& view : output->workspace->get_views_in_layer(wf::VISIBLE_LAYERS))
    {
        if (can_focus_surface(view.get()))
        {
            auto surface = view->map_input_coordinates(global, local);
            if (surface)
                return surface;
        }
    }

    return nullptr;
}

void input_manager::set_exclusive_focus(wl_client *client)
{
    exclusive_client = client;
    for (auto& wo : wf::get_core().output_layout->get_outputs())
    {
        auto impl = (wf::output_impl_t*) wo;
        if (client) {
            impl->inhibit_plugins();
        } else {
            impl->uninhibit_plugins();
        }
    }

    /* We no longer have an exclusively focused client, so we should restore
     * focus to the topmost view */
    if (!client)
        wf::get_core().get_active_output()->refocus(nullptr);
}

/* add/remove bindings */

wf_binding* input_manager::new_binding(wf_binding_type type, wf_option value,
    wf::output_t *output, void *callback)
{
    auto binding = std::make_unique<wf_binding>();

    assert(value && output && callback);

    binding->type = type;
    binding->value = value;
    binding->output = output;
    binding->call.raw = callback;

    auto raw = binding.get();
    bindings[type].push_back(std::move(binding));

    return raw;
}

void input_manager::rem_binding(binding_criteria criteria)
{
    for(auto& category : bindings)
    {
        auto& container = category.second;
        auto it = container.begin();
        while (it != container.end())
        {
            if (criteria((*it).get())) {
                it = container.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void input_manager::rem_binding(wf_binding *binding)
{
    rem_binding([=] (wf_binding *ptr) { return binding == ptr; });
}

void input_manager::rem_binding(void *callback)
{
    rem_binding([=] (wf_binding* ptr) {return ptr->call.raw == callback; });
}

void input_manager::free_output_bindings(wf::output_t *output)
{
    rem_binding([=] (wf_binding* binding) {
        return binding->output == output;
    });
}

bool input_manager::check_button_bindings(uint32_t button)
{
    std::vector<std::function<bool()>> callbacks;

    auto oc = wf::get_core().get_active_output()->get_cursor_position();
    auto mod_state = get_modifiers();

    for (auto& binding : bindings[WF_BINDING_BUTTON])
    {
        if (binding->output == wf::get_core().get_active_output() &&
            binding->value->as_cached_button().matches({mod_state, button}))
        {
            /* We must be careful because the callback might be erased,
             * so force copy the callback into the lambda */
            auto callback = binding->call.button;
            callbacks.push_back([=] () {
                return (*callback) (button, oc.x, oc.y);
            });
        }
    }

    for (auto& binding : bindings[WF_BINDING_ACTIVATOR])
    {
        if (binding->output == wf::get_core().get_active_output() &&
            binding->value->matches_button({mod_state, button}))
        {
            /* We must be careful because the callback might be erased,
             * so force copy the callback into the lambda */
            auto callback = binding->call.activator;
            callbacks.push_back([=] () {
                return (*callback) (ACTIVATOR_SOURCE_BUTTONBINDING, button);
            });
        }
    }

    bool binding_handled = false;
    for (auto call : callbacks)
        binding_handled |= call();

    return !callbacks.empty() && binding_handled;
}

bool input_manager::check_axis_bindings(wlr_event_pointer_axis *ev)
{
    std::vector<axis_callback*> callbacks;
    auto mod_state = get_modifiers();

    for (auto& binding : bindings[WF_BINDING_AXIS])
    {
        if (binding->output == wf::get_core().get_active_output() &&
            binding->value->as_cached_key().matches({mod_state, 0}))
            callbacks.push_back(binding->call.axis);
    }

    for (auto call : callbacks)
        (*call) (ev);

    return !callbacks.empty();
}

wf::SurfaceMapStateListener::SurfaceMapStateListener()
{
    on_surface_map_state_change = [=] (void *data)
    {
        if (this->callback)
        {
            auto ev = static_cast<_surface_map_state_changed_signal*> (data);
            this->callback(ev ? ev->surface : nullptr);
        }
    };

    wf::get_core().connect_signal("_surface_mapped",
        &on_surface_map_state_change);
    wf::get_core().connect_signal("_surface_unmapped",
        &on_surface_map_state_change);
}

wf::SurfaceMapStateListener::~SurfaceMapStateListener()
{
    wf::get_core().disconnect_signal("_surface_mapped",
        &on_surface_map_state_change);
    wf::get_core().disconnect_signal("_surface_unmapped",
        &on_surface_map_state_change);
}

void wf::SurfaceMapStateListener::set_callback(Callback call)
{
    this->callback = call;
}

