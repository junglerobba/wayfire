#ifndef WF_UTIL_HPP
#define WF_UTIL_HPP

#include <algorithm>
#include <functional>
#include <pixman.h>
#include <nonstd/noncopyable.hpp>

#include "geometry.hpp"

/* ---------------------- pixman utility functions -------------------------- */
struct wf_region
{
    wf_region();
    /* Makes a copy of the given region */
    wf_region(pixman_region32_t *damage);
    wf_region(const wlr_box& box);
    ~wf_region();

    wf_region(const wf_region& other);
    wf_region(wf_region&& other);

    wf_region& operator = (const wf_region& other);
    wf_region& operator = (wf_region&& other);

    bool empty() const;
    void clear();

    void expand_edges(int amount);
    pixman_box32_t get_extents() const;
    bool contains_point(const wf_point& point) const;
    bool contains_pointf(const wf_pointf& point) const;

    /* Translate the region */
    wf_region operator + (const wf_point& vector) const;
    wf_region& operator += (const wf_point& vector);

    wf_region operator * (float scale) const;
    wf_region& operator *= (float scale);

    /* Region intersection */
    wf_region operator & (const wlr_box& box) const;
    wf_region operator & (const wf_region& other) const;
    wf_region& operator &= (const wlr_box& box);
    wf_region& operator &= (const wf_region& other);

    /* Region union */
    wf_region operator | (const wlr_box& other) const;
    wf_region operator | (const wf_region& other) const;
    wf_region& operator |= (const wlr_box& other);
    wf_region& operator |= (const wf_region& other);

    /* Subtract the box/region from the current region */
    wf_region operator ^ (const wlr_box& box) const;
    wf_region operator ^ (const wf_region& other) const;
    wf_region& operator ^= (const wlr_box& box);
    wf_region& operator ^= (const wf_region& other);

    pixman_region32_t *to_pixman();

    const pixman_box32_t* begin() const;
    const pixman_box32_t* end() const;

    private:
    pixman_region32_t _region;
    /* Returns a const-casted pixman_region32_t*, useful in const operators
     * where we use this->_region as only source for calculations, but pixman
     * won't let us pass a const pixman_region32_t* */
    pixman_region32_t* unconst() const;
};

wlr_box wlr_box_from_pixman_box(const pixman_box32_t& box);
pixman_box32_t pixman_box_from_wlr_box(const wlr_box& box);

/* ------------------------- misc helper functions ------------------------- */
int64_t timespec_to_msec(const timespec& ts);

/* Returns current time in msec, using CLOCK_MONOTONIC as a base */
uint32_t get_current_time();

/* Ensure that value is in the interval [min, max] */
template<class T>
T clamp(T value, T min, T max)
{
    return std::min(std::max(value, min), max);
}

/**
 * Return the closest geometry to window which is completely inside the output.
 * The returned geometry might be smaller, but never bigger than window.
 */
wf_geometry clamp(wf_geometry window, wf_geometry output);

namespace wf
{
    /**
     * A wrapper around wl_listener compatible with C++11 std::functions
     */
    struct wl_listener_wrapper : public noncopyable_t
    {
        using callback_t = std::function<void(void*)>;
        wl_listener_wrapper();
        ~wl_listener_wrapper();

        /** Set the callback to be used when the signal is fired. Can be called
         * multiple times to update it */
        void set_callback(callback_t call);
        /** Connect this callback to a signal. Calling this on an already
         * connected listener will have no effect.
         * @return true if connection was successful */
        bool connect(wl_signal *signal);
        /** Disconnect from the wl_signal. No-op if not connected */
        void disconnect();
        /** @return true if connected to a wl_signal */
        bool is_connected() const;
        /** Call the stored callback. No-op if no callback was specified */
        void emit(void *data);

        struct wrapper
        {
            wl_listener listener;
            wl_listener_wrapper *self;
        };
        private:
        callback_t call;
        wrapper _wrap;
    };

    /**
     * A wrapper for adding idle callbacks to the event loop
     */
    class wl_idle_call : public noncopyable_t
    {
        public:
        using callback_t = std::function<void()>;
        /* Initialize an empty idle call. set_event_loop() and set_callback()
         * should be called before calls to run_once(), otherwise it won't
         * have any effect */
        wl_idle_call();
        /** Will disconnect if connected */
        ~wl_idle_call();

        /** Set the event loop. This will disconnect the wl_idle_call if it
         * is connected. If no event loop is set (or if NULL loop), the default
         * loop from core is used */
        void set_event_loop(wl_event_loop *loop);

        /** Set the callback. This will disconnect the wl_idle_call if it is
         * connected */
        void set_callback(callback_t call);

        /** Run the passed callback the next time the loop goes idle. No effect
         * if already waiting for idleness, or if the callback hasn't been set. */
        void run_once();

        /* Same as calling set_callbck + run_once */
        void run_once(callback_t call);

        /** Stop waiting for idle, no-op if not connected */
        void disconnect();
        /** @return true if the event source is active */
        bool is_connected();

        /** execute the callback now. do not use manually! */
        void execute();

        private:
        callback_t call;
        wl_event_loop *loop = NULL;
        wl_event_source *source = NULL;
    };

    /**
     * A wrapper for wl_event_loop_add_timer / wl_event_loop_timer_update
     */
    class wl_timer
    {
        public:
        using callback_t = std::function<void()>;

        /** Disconnects the timer if connected */
        ~wl_timer();

        /** Execute call after a timeout of timeout_ms */
        void set_timeout(uint32_t timeout_ms, callback_t call);

        /** If a timeout has been registered, but not fired yet, remove the
         * timeout. Otherwise no-op */
        void disconnect();
        /** @return true if the event source is active */
        bool is_connected();

        /* Run the stored call now, regardless of the timeout. No-op if not
         * connected */
        void execute();

        private:
        callback_t call;
        wl_event_source *source = NULL;
    };
}

#endif /* end of include guard: WF_UTIL_HPP */
