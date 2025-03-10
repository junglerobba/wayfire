extern "C"
{
#include <wlr/types/wlr_idle.h>
}

#include "singleton-plugin.hpp"
#include "render-manager.hpp"
#include "output.hpp"
#include "core.hpp"
#include "config.hpp"
#include "output-layout.hpp"
#include "workspace-manager.hpp"
#include "../cube/cube-control-signal.hpp"

#include <cmath>
#include <animation.hpp>

#define ZOOM_BASE 1.0

enum screensaver_state
{
    SCREENSAVER_DISABLED,
    SCREENSAVER_RUNNING,
    SCREENSAVER_STOPPING
};

class wayfire_idle
{
    wf::effect_hook_t render_hook;
    double rotation = 0.0;
    wf_duration duration;
    wf_transition rot_transition, zoom_transition;
    screensaver_state state = SCREENSAVER_DISABLED;
    std::map<wf::output_t*, bool> screensaver_hook_set;
    bool outputs_inhibited = false;
    bool idle_enabled = true;
    uint32_t last_time;
    wlr_idle_timeout *timeout_screensaver = NULL;
    wlr_idle_timeout *timeout_dpms = NULL;
    wf::wl_listener_wrapper on_idle_screensaver, on_resume_screensaver;
    wf::wl_listener_wrapper on_idle_dpms, on_resume_dpms;

    wf_option dpms_timeout, screensaver_timeout;
    wf_option cube_zoom_speed, cube_rotate_speed, cube_zoom_end;
    wf_option_callback dpms_timeout_updated = [=] () {
        create_dpms_timeout(dpms_timeout->as_int());
    };
    wf_option_callback screensaver_timeout_updated = [=] () {
        create_screensaver_timeout(screensaver_timeout->as_int());
    };

    public:
    wayfire_idle()
    {
        dpms_timeout = wf::get_core().config->get_section("idle")
            ->get_option("dpms_timeout", "-1");
        screensaver_timeout = wf::get_core().config->get_section("idle")
            ->get_option("screensaver_timeout", "-1");
        cube_zoom_speed = wf::get_core().config->get_section("idle")
            ->get_option("cube_zoom_speed", "1000");
        cube_rotate_speed = wf::get_core().config->get_section("idle")
            ->get_option("cube_rotate_speed", "1.0");
        cube_zoom_end = wf::get_core().config->get_section("idle")
            ->get_option("cube_max_zoom", "1.5");

        render_hook = [=] () { screensaver_frame(); };

        dpms_timeout->add_updated_handler(&dpms_timeout_updated);
        dpms_timeout_updated();
        screensaver_timeout->add_updated_handler(&screensaver_timeout_updated);
        screensaver_timeout_updated();
        duration = wf_duration(cube_zoom_speed);
    }

    void destroy_dpms_timeout()
    {
        if (timeout_dpms)
        {
            on_idle_dpms.disconnect();
            on_resume_dpms.disconnect();
            wlr_idle_timeout_destroy(timeout_dpms);
        }

        timeout_dpms = NULL;
    }

    void destroy_screensaver_timeout()
    {
        if (state == SCREENSAVER_RUNNING)
            stop_screensaver();

        if (timeout_screensaver)
        {
            on_idle_screensaver.disconnect();
            on_resume_screensaver.disconnect();
            wlr_idle_timeout_destroy(timeout_screensaver);
        }

        timeout_screensaver = NULL;
    }

    void create_dpms_timeout(int timeout_sec)
    {
        destroy_dpms_timeout();
        if (timeout_sec <= 0)
            return;

        timeout_dpms = wlr_idle_timeout_create(wf::get_core().protocols.idle,
            wf::get_core().get_current_seat(), 1000 * timeout_sec);

        on_idle_dpms.set_callback([&] (void*) {
            set_state(wf::OUTPUT_IMAGE_SOURCE_SELF, wf::OUTPUT_IMAGE_SOURCE_DPMS);
        });
        on_idle_dpms.connect(&timeout_dpms->events.idle);

        on_resume_dpms.set_callback([&] (void*) {
            set_state(wf::OUTPUT_IMAGE_SOURCE_DPMS, wf::OUTPUT_IMAGE_SOURCE_SELF);
        });
        on_resume_dpms.connect(&timeout_dpms->events.resume);
    }

    void create_screensaver_timeout(int timeout_sec)
    {
        destroy_screensaver_timeout();
        if (timeout_sec <= 0)
            return;

        timeout_screensaver = wlr_idle_timeout_create(wf::get_core().protocols.idle,
            wf::get_core().get_current_seat(), 1000 * timeout_sec);
        on_idle_screensaver.set_callback([&] (void*) {
            start_screensaver();
        });
        on_idle_screensaver.connect(&timeout_screensaver->events.idle);

        on_resume_screensaver.set_callback([&] (void*) {
            stop_screensaver();
        });
        on_resume_screensaver.connect(&timeout_screensaver->events.resume);
    }

    void inhibit_outputs()
    {
        if (state == SCREENSAVER_DISABLED || outputs_inhibited)
            return;

        for (auto& output : wf::get_core().output_layout->get_outputs())
        {
            if (screensaver_hook_set[output])
            {
                output->render->rem_effect(&render_hook);
                screensaver_hook_set[output] = false;
            }
            output->render->add_inhibit(true);
            output->render->damage_whole();
        }
        screensaver_hook_set.clear();
        state = SCREENSAVER_DISABLED;
        outputs_inhibited = true;
    }

    void screensaver_terminate()
    {
        cube_control_signal data;
        data.angle = 0.0;
        data.zoom = ZOOM_BASE;
        data.last_frame = true;
        for (auto& output : wf::get_core().output_layout->get_outputs())
        {
            output->emit_signal("cube-control", &data);
            if (screensaver_hook_set[output])
            {
                output->render->rem_effect(&render_hook);
                screensaver_hook_set[output] = false;
            }
            if (state == SCREENSAVER_DISABLED && outputs_inhibited)
            {
                output->render->add_inhibit(false);
                output->render->damage_whole();
                outputs_inhibited = false;
            }
        }
        state = SCREENSAVER_DISABLED;
    }

    void screensaver_frame()
    {
        cube_control_signal data;
        bool all_outputs_active = true;
        uint32_t current = get_current_time();
        uint32_t elapsed = current - last_time;

        last_time = current;

        if (state == SCREENSAVER_STOPPING && !duration.running())
        {
            screensaver_terminate();
            return;
        }

        if (state == SCREENSAVER_STOPPING)
        {
            rotation = duration.progress(rot_transition);
        }
        else
        {
            rotation += (cube_rotate_speed->as_double() / 5000.0) * elapsed;
        }

        if (rotation > M_PI * 2)
        {
            rotation -= M_PI * 2;
        }

        data.angle = rotation;
        data.zoom = duration.progress(zoom_transition);
        data.last_frame = false;

        for (auto& output : wf::get_core().output_layout->get_outputs())
        {
            output->emit_signal("cube-control", &data);
            if (!data.carried_out)
            {
                all_outputs_active = false;
                break;
            }
        }

        if (!all_outputs_active)
        {
            inhibit_outputs();
            state = SCREENSAVER_DISABLED;
            return;
        }

        if (state == SCREENSAVER_STOPPING)
        {
            wlr_idle_notify_activity(wf::get_core().protocols.idle,
                wf::get_core().get_current_seat());
        }
    }

    void start_screensaver()
    {
        cube_control_signal data;
        data.angle = 0.0;
        data.zoom = ZOOM_BASE;
        data.last_frame = false;
        bool all_outputs_active = true;
        bool hook_set = false;

        for (auto& output : wf::get_core().output_layout->get_outputs())
        {
            for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
            {
                if (view->fullscreen)
                    return;
            }
            output->emit_signal("cube-control", &data);
            if (data.carried_out)
            {
                if (!screensaver_hook_set[output] && !hook_set)
                {
                    output->render->add_effect(&render_hook, wf::OUTPUT_EFFECT_PRE);
                    hook_set = screensaver_hook_set[output] = true;
                }
            }
            else
            {
                all_outputs_active = false;
            }
        }

        state = SCREENSAVER_RUNNING;

        if (!all_outputs_active)
        {
            inhibit_outputs();
            state = SCREENSAVER_DISABLED;
            return;
        }

        rotation = 0.0;
        zoom_transition = {ZOOM_BASE, cube_zoom_end->as_double()};
        duration.start();
        last_time = get_current_time();
    }

    void stop_screensaver()
    {
        if (state == SCREENSAVER_DISABLED)
        {
            if (outputs_inhibited)
            {
                for (auto& output : wf::get_core().output_layout->get_outputs())
                {
                    output->render->add_inhibit(false);
                    output->render->damage_whole();
                }
                outputs_inhibited = false;
            }
            return;
        }

        state = SCREENSAVER_STOPPING;
        double end = rotation > M_PI ? M_PI * 2 : 0.0;
        rot_transition = {rotation, end};
        zoom_transition = {duration.progress(zoom_transition), ZOOM_BASE};
        duration.start();
    }

    ~wayfire_idle()
    {
        destroy_dpms_timeout();
        destroy_screensaver_timeout();

        dpms_timeout->rem_updated_handler(&dpms_timeout_updated);
        screensaver_timeout->rem_updated_handler(&screensaver_timeout_updated);

        /* Make sure idle is enabled */
        if (!idle_enabled)
            toggle_idle();
    }

    /* Change all outputs with state from to state to */
    void set_state(wf::output_image_source_t from, wf::output_image_source_t to)
    {
        auto config = wf::get_core().output_layout->get_current_configuration();

        for (auto& entry : config)
        {
            if (entry.second.source == from)
                entry.second.source = to;
        }

        wf::get_core().output_layout->apply_configuration(config);
    }

    void toggle_idle()
    {
        idle_enabled ^= 1;
        wlr_idle_set_enabled(wf::get_core().protocols.idle, NULL, idle_enabled);
    }
};

class wayfire_idle_singleton : public wf::singleton_plugin_t<wayfire_idle>
{
    activator_callback toggle;
    void init(wayfire_config *config) override
    {
        singleton_plugin_t::init(config);

        grab_interface->name = "idle";
        grab_interface->capabilities = 0;

        auto binding = config->get_section("idle")
            ->get_option("toggle", "<super> <shift> KEY_I");
        toggle = [=] (wf_activator_source, uint32_t) {
            if (!output->can_activate_plugin(grab_interface))
                return false;

            get_instance().toggle_idle();

            return true;
        };

        output->add_activator(binding, &toggle);
    }

    void fini() override
    {
        output->rem_binding(&toggle);
        singleton_plugin_t::fini();
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_idle_singleton);
