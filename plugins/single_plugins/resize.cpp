#include <cmath>
#include <plugin.hpp>
#include <debug.hpp>
#include <output.hpp>
#include <view.hpp>
#include <core.hpp>
#include <workspace-manager.hpp>
#include <linux/input.h>
#include <signal-definitions.hpp>

extern "C"
{
#include <wlr/types/wlr_xcursor_manager.h>
}

#include "../wobbly/wobbly-signal.hpp"

class wayfire_resize : public wf::plugin_interface_t
{
    wf::signal_callback_t resize_request, view_destroyed;

    button_callback activate_binding;
    touch_callback touch_activate_binding;

    wayfire_view view;

    bool was_client_request, is_using_touch;
    wf_point grab_start;
    wf_geometry grabbed_geometry;

    uint32_t edges;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = "resize";
        grab_interface->capabilities =
            wf::CAPABILITY_GRAB_INPUT | wf::CAPABILITY_MANAGE_DESKTOP;

        auto button = config->get_section("resize")
            ->get_option("activate", "<super> BTN_RIGHT");

        activate_binding = [=] (uint32_t, int, int)
        {
            auto view = wf::get_core().get_cursor_focus_view();
            if (view)
            {
                is_using_touch = false;
                was_client_request = false;
                return initiate(view);
            }
            return false;
        };

        touch_activate_binding = [=] (int32_t sx, int32_t sy)
        {
            auto view = wf::get_core().get_touch_focus_view();
            if (view)
            {
                is_using_touch = true;
                was_client_request = false;
                return initiate(view);
            }
            return false;
        };

        output->add_button(button, &activate_binding);
        output->add_touch(new_static_option("<super> <shift>"), &touch_activate_binding);

        grab_interface->callbacks.pointer.button = [=] (uint32_t b, uint32_t state)
        {
            if (state == WLR_BUTTON_RELEASED && was_client_request && b == BTN_LEFT)
                return input_pressed(state);

            if (b != button->as_cached_button().button)
                return;

            input_pressed(state);
        };

        grab_interface->callbacks.pointer.motion = [=] (int, int)
        {
            input_motion();
        };

        grab_interface->callbacks.touch.up = [=] (int32_t id)
        {
            if (id == 0)
                input_pressed(WLR_BUTTON_RELEASED);
        };

        grab_interface->callbacks.touch.motion = [=] (int32_t id, int32_t, int32_t)
        {
            if (id == 0)
                input_motion();
        };

        grab_interface->callbacks.cancel = [=] ()
        {
            input_pressed(WLR_BUTTON_RELEASED);
        };

        using namespace std::placeholders;
        resize_request = std::bind(std::mem_fn(&wayfire_resize::resize_requested),
                this, _1);
        output->connect_signal("resize-request", &resize_request);

        view_destroyed = [=] (wf::signal_data_t* data)
        {
            if (get_signaled_view(data) == view)
            {
                view = nullptr;
                input_pressed(WLR_BUTTON_RELEASED);
            }
        };

        output->connect_signal("detach-view", &view_destroyed);
        output->connect_signal("view-disappeared", &view_destroyed);
    }

    void resize_requested(wf::signal_data_t *data)
    {
        auto request = static_cast<resize_request_signal*> (data);
        auto view = get_signaled_view(data);

        if (!view)
            return;

        auto touch = wf::get_core().get_touch_position(0);
        if (!std::isnan(touch.x) && !std::isnan(touch.y)) {
            is_using_touch = true;
        } else {
            is_using_touch = false;
        }

        was_client_request = true;
        initiate(view, request->edges);
    }

    /* Returns the currently used input coordinates in global compositor space */
    wf_point get_global_input_coords()
    {
        wf_pointf input;
        if (is_using_touch) {
            input = wf::get_core().get_touch_position(0);
        } else {
            input = wf::get_core().get_cursor_position();
        }

        return {(int)input.x, (int)input.y};
    }

    /* Returns the currently used input coordinates in output-local space */
    wf_point get_input_coords()
    {
        auto og = output->get_layout_geometry();
        return get_global_input_coords() - wf_point{og.x, og.y};
    }

    /* Calculate resize edges, grab starts at (sx, sy), view's geometry is vg */
    uint32_t calculate_edges(wf_geometry vg, int sx, int sy)
    {
        int view_x = sx - vg.x;
        int view_y = sy - vg.y;

        uint32_t edges = 0;
        if (view_x < vg.width / 2) {
            edges |= WLR_EDGE_LEFT;
        } else {
            edges |= WLR_EDGE_RIGHT;
        }

        if (view_y < vg.height / 2) {
            edges |= WLR_EDGE_TOP;
        } else {
            edges |= WLR_EDGE_BOTTOM;
        }

        return edges;
    }

    bool initiate(wayfire_view view, uint32_t forced_edges = 0)
    {
        if (!view || view->role == wf::VIEW_ROLE_SHELL_VIEW || !view->is_mapped())
            return false;

        auto current_ws_impl =
            output->workspace->get_workspace_implementation();
        if (!current_ws_impl->view_resizable(view))
            return false;

        if (!output->activate_plugin(grab_interface))
            return false;
        if (!grab_interface->grab()) {
            output->deactivate_plugin(grab_interface);
            return false;
        }

        grab_start = get_input_coords();
        grabbed_geometry = view->get_wm_geometry();

        this->edges = forced_edges ?: calculate_edges(grabbed_geometry,
            grab_start.x, grab_start.y);

        if ((edges & WLR_EDGE_LEFT) || (edges & WLR_EDGE_TOP))
            view->set_moving(true);

        view->set_resizing(true, edges);

        if (view->fullscreen)
            view->set_fullscreen(false);
        if (view->tiled_edges)
            view->set_tiled(0);

        if (edges == 0) /* simply deactivate */
            input_pressed(WL_POINTER_BUTTON_STATE_RELEASED);

        this->view = view;

        auto og = view->get_output_geometry();
        int anchor_x = og.x;
        int anchor_y = og.y;

        if (edges & WLR_EDGE_LEFT)
            anchor_x += og.width;
        if (edges & WLR_EDGE_TOP)
            anchor_y += og.height;

        start_wobbly(view, anchor_x, anchor_y);
        wf::get_core().set_cursor(wlr_xcursor_get_resize_name((wlr_edges)edges));

        return true;
    }

    void input_pressed(uint32_t state)
    {
        if (state != WLR_BUTTON_RELEASED)
            return;

        grab_interface->ungrab();
        output->deactivate_plugin(grab_interface);

        if (view)
        {
            if ((edges & WLR_EDGE_LEFT) ||
                (edges & WLR_EDGE_TOP))
                view->set_moving(false);
            view->set_resizing(false);
            end_wobbly(view);
        }
    }

    void input_motion()
    {
        auto input = get_input_coords();
        int dx = input.x - grab_start.x;
        int dy = input.y - grab_start.y;
        int width = grabbed_geometry.width;
        int height = grabbed_geometry.height;

        if (edges & WLR_EDGE_LEFT)
            width -= dx;
        else if (edges & WLR_EDGE_RIGHT)
            width += dx;

        if (edges & WLR_EDGE_TOP)
            height -= dy;
        else if (edges & WLR_EDGE_BOTTOM)
            height += dy;

        height = std::max(height, 1);
        width  = std::max(width,  1);
        view->resize(width, height);
    }

    void fini()
    {
        if (grab_interface->is_grabbed())
            input_pressed(WLR_BUTTON_RELEASED);

        output->rem_binding(&activate_binding);
        output->rem_binding(&touch_activate_binding);

        output->disconnect_signal("resize-request", &resize_request);
        output->disconnect_signal("detach-view", &view_destroyed);
        output->disconnect_signal("view-disappeared", &view_destroyed);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_resize);
