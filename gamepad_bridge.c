#include "gamepad_bridge.h"

#include "app_config.h"

#include <SDL2/SDL.h>
#include <libusb-1.0/libusb.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* See gamepad_bridge.h for context and protocol sources. */

/* Known USB identifiers of ConsoleTuner adapters (GPP/Cronus/Titan). The
 * user's Titan One is 2508:0003; the others are kept for compatibility in
 * case the device is ever replaced/added to. */
static const struct {
    uint16_t vid, pid;
    const char *name;
} KNOWN_DEVICES[] = {
    {0x2508, 0x0001, "GPP/Cronus/CronusMAX"},
    {0x2508, 0x0002, "Cronus"},
    {0x2508, 0x0003, "Titan One"},
    {0x2508, 0x0004, "CronusMAX v2"},
    {0x2008, 0x0001, "CronusMAX PLUS (v3)"},
};
#define N_KNOWN_DEVICES (int)(sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0]))

/* GCAPI protocol commands (HID report). */
#define GPPKG_INPUT_REPORT 0x01
#define GPPKG_OUTPUT_REPORT 0x04
#define GPPKG_ENTER_CAPTURE 0x07
#define GPPKG_LEAVE_CAPTURE 0x08

/* Two commands of unknown exact purpose, observed (via a real USB capture)
 * in GTuner Pro's own startup sequence, right around its own
 * GPPKG_LEAVE_CAPTURE, before it ever sends GPPKG_ENTER_CAPTURE: 0xf0
 * first, then LEAVE_CAPTURE, then 0xf1. Both are sent with an empty
 * payload, same as ENTER_CAPTURE/LEAVE_CAPTURE. We don't know what they
 * do, but GTuner's real sessions never show the phantom "select/back"
 * press we get at startup, so we replay its exact sequence here as an
 * experiment -- worst case they are no-ops for the firmware. */
#define GPPKG_UNKNOWN_F0 0xf0
#define GPPKG_UNKNOWN_F1 0xf1

/* Offset, within a received GPPKG_INPUT_REPORT, of the start of the
 * GCAPI_REPORT structure (controller, console, led[4], rumble[2],
 * battery_level, then the input[] array indexed like GAMEPAD_XB360_*) --
 * value taken as-is from GIMX's source code (pcprog.c). Known to produce
 * inconsistent buttons in output (not yet verified with a targeted USB
 * capture) -- left as-is for now, priority being the latency of the
 * virtual state, which does not depend on it. */
#define GCAPI_REPORT_OFFSET 7
#define GCAPI_REPORT_INPUT_OFFSET (GCAPI_REPORT_OFFSET + 9) /* +controller+console+led[4]+rumble[2]+battery */

#define REPORT_SIZE 64 /* actual size on the wire: header(4) + data(60) */
#define REPORT_DATA_SIZE (REPORT_SIZE - 4)

#define GCAPI_OUTPUT_TOTAL 36 /* total entries defined by the protocol, only 0..20 are used for Xbox 360 */

static libusb_context *g_ctx;
static libusb_device_handle *g_handle;
static int g_interface_number = -1;
static unsigned char g_out_ep = 0;
static unsigned char g_in_ep = 0;
static int g_connected = 0;
/* The USB link itself, as opposed to g_connected which means "the bridge
 * was initialised and its send thread is running". The adapter can be
 * unplugged and plugged back in without the rest of the program noticing:
 * the state written by gamepad_bridge_update() keeps being accepted, and
 * is applied as soon as the link is back. */
static volatile int g_link_up = 0;
/* Asked for by gamepad_bridge_reset(). Acted on by the send thread,
 * never by the caller: the send thread hammers the same handle
 * continuously, and issuing a reset underneath it from another thread
 * would be a use-after-free waiting to happen. */
static volatile int g_reset_requested = 0;
/* A HOME press asked for by a client, applied by the send thread. */
static volatile int g_home_press_requested = 0;
/* Milliseconds GUIDE is held. Short enough to be a tap, long enough that
 * the console's own polling cannot miss it. */
#define HOME_PRESS_MS 120
/* How long to wait between attempts to find the adapter again. Long
 * enough not to spin on libusb enumeration, short enough that plugging it
 * back in feels immediate. */
#define RECONNECT_RETRY_MS 1000
/* Only ever reached when the adapter does not answer -- a healthy
 * transfer returns in a couple of ms, so this does not touch the send
 * loop's timing. It does bound how long shutdown waits to join that
 * thread, which matters because the launcher SIGKILLs at 2 s. */
#define USB_TRANSFER_TIMEOUT_MS 250

/* How long the adapter stays CLOSED during a reset, before being opened
 * again.
 *
 * libusb_reset_device() re-enumerates in a few milliseconds, and that
 * turned out not to be enough: after waking the console, input latency
 * persisted until the adapter was physically unplugged for a couple of
 * seconds. So the reset now reproduces that -- released, left alone, then
 * reopened -- rather than just re-enumerating. Three seconds, since two
 * was the shortest unplug observed to work. */
#define RESET_HOLD_MS 3000
/* How often the send loop reports its own throughput. */
#define SEND_STATS_INTERVAL_MS 5000
/* Longest silence before an unchanged report goes out anyway, so the
 * adapter never concludes we stopped talking to it. Well under any
 * plausible timeout, and far above the input rate that matters. */
#define KEEPALIVE_MS 100
/* One retry is enough: the only expected failure is the reset having
 * re-enumerated the device out from under the handle. */
#define GAMEPAD_OPEN_ATTEMPTS 2
#define GAMEPAD_OPEN_RETRY_MS 300
/* Whatever usb_open_and_claim() last matched. Points at a string literal
 * or into KNOWN_DEVICES, both static, so it stays valid. */
static const char *g_device_name = "adapter";

/* Last known state of the REAL controller plugged into the Titan One's
 * controller port (read via GPPKG_INPUT_REPORT), to merge with our
 * virtual state instead of overwriting it -- the local user should keep
 * control at the same time as remote control. Only read/written by the
 * send thread (send_thread_main): no mutex needed. */
static int8_t g_real_state[GAMEPAD_BRIDGE_STATE_COUNT];

/* USB writes (libusb_interrupt_transfer) are blocking and can take
 * several milliseconds. gamepad_bridge_update() is called from the
 * GStreamer/WebRTC thread on every DataChannel message (~60Hz on the
 * browser side): blocking it on that would cause pending messages to
 * pile up if the USB is even slightly slower than the arrival rate, with
 * a delay that grows and never recovers (observed: up to ~1 minute after
 * a while). A dedicated thread therefore handles the USB on its own, in a
 * continuous loop (see send_thread_main); gamepad_bridge_update() just
 * stores the latest state (memcpy under a mutex, near-instant) and never
 * touches the USB. */
static SDL_mutex *g_state_mutex;
static int8_t g_latest_state[GAMEPAD_BRIDGE_STATE_COUNT];

/* One slot per connected client.
 *
 * There used to be a single state, and whoever sent last won. With two
 * people connected that is a fight rather than a hand-over: the browser
 * sends its state on every animation frame whether or not anything
 * changed, so a browser sitting on the page with no controller selected
 * wrote zeroes sixty times a second over whatever the other player was
 * holding. The result was not one player winning -- it was both
 * stuttering. */
#define GAMEPAD_MAX_SOURCES 8

/* A source silent for this long is dropped from the combination. Both
 * transports announce their own departure, so this only covers the case
 * where one is cut off mid-press: without it, a client whose network
 * disappears leaves a button held down on the console forever. */
#define GAMEPAD_SOURCE_TIMEOUT_MS 1500

typedef struct {
    int      in_use;
    unsigned key;
    Uint32   last_ms;
    int8_t   state[GAMEPAD_BRIDGE_STATE_COUNT];
} GamepadSource;

static GamepadSource g_sources[GAMEPAD_MAX_SOURCES];

/* Reports actually put on the wire, per second, over the last window.
 *
 * Published rather than only printed: it is the one observable that says
 * input is reaching the console, and the alternative was a test reading
 * it back out of a log file -- which meant the log had to keep being
 * written for the test to work. Written by the send thread, read by
 * whoever asks; a torn double here would cost a wrong number on a
 * diagnostic, not a wrong decision. */
static volatile double g_reports_per_sec = 0.0;

double gamepad_bridge_report_rate(void) {
    return g_connected ? g_reports_per_sec : 0.0;
}

/* Sticks are signed and everything else runs 0..100, so they cannot be
 * combined the same way: the largest deflection wins for an axis, while
 * a button is pressed if anyone is pressing it. */
static int is_axis(int i) {
    return i == GAMEPAD_XB360_RX || i == GAMEPAD_XB360_RY ||
           i == GAMEPAD_XB360_LX || i == GAMEPAD_XB360_LY;
}

/* Caller holds g_state_mutex. */
static void recombine(void) {
    Uint32 now = SDL_GetTicks();
    int8_t merged[GAMEPAD_BRIDGE_STATE_COUNT];
    memset(merged, 0, sizeof(merged));

    for (int s = 0; s < GAMEPAD_MAX_SOURCES; s++) {
        GamepadSource *src = &g_sources[s];
        if (!src->in_use) {
            continue;
        }
        if (now - src->last_ms > GAMEPAD_SOURCE_TIMEOUT_MS) {
            src->in_use = 0;
            continue;
        }
        for (int i = 0; i < GAMEPAD_BRIDGE_STATE_COUNT; i++) {
            int v = src->state[i];
            if (is_axis(i)) {
                if (abs(v) > abs(merged[i])) merged[i] = (int8_t)v;
            } else if (v > merged[i]) {
                merged[i] = (int8_t)v;
            }
        }
    }
    memcpy(g_latest_state, merged, sizeof(g_latest_state));
}
static SDL_Thread *g_send_thread;
static volatile int g_thread_running = 0;

/* Builds and sends a GCAPI HID report on the wire: [0]=type,
 * [1..2]=length (LE16), [3]=first(1), [4..63]=data (zero-padded) -- 64
 * bytes total.
 *
 * The "logical" format of the protocol (as described by GIMX) includes a
 * leading report-id byte set to 0x00 (65 bytes), but that is only an
 * internal C struct convention: GIMX always drops it on the wire whenever
 * it is 0 (see gusbhid_write_timeout, which skips the first byte if it
 * is == 0x00 before the actual USB send), confirmed by a USB capture of
 * GTuner's traffic (64-byte packets, never 65). Sending it anyway shifts
 * the whole packet by one byte and the firmware silently ignores it
 * (accepted by USB, but no effect on the console) -- bug fixed here. */
/* Split out from send_report() so the wire format itself can be unit
 * tested without a device attached (see tests/c/) -- this layout is
 * where the original "packet silently ignored by the firmware" bug
 * lived, so it's worth pinning down. `buf` must be REPORT_SIZE bytes. */
static void build_report(unsigned char *buf, uint8_t type, const uint8_t *data, uint16_t length) {
    memset(buf, 0, REPORT_SIZE);
    buf[0] = type;
    buf[1] = (unsigned char)(length & 0xff);
    buf[2] = (unsigned char)((length >> 8) & 0xff);
    buf[3] = 1; /* first: our packets always fit in a single report (<=REPORT_DATA_SIZE bytes) */
    if (data && length > 0) {
        uint16_t n = length > REPORT_DATA_SIZE ? REPORT_DATA_SIZE : length;
        memcpy(buf + 4, data, n);
    }
}

/* Errors that mean "the adapter is gone", as opposed to a transfer that
 * merely failed: these are the ones worth reconnecting from rather than
 * logging. */
static int usb_error_is_disconnect(int ret) {
    return ret == LIBUSB_ERROR_NO_DEVICE || ret == LIBUSB_ERROR_NOT_FOUND ||
           ret == LIBUSB_ERROR_IO || ret == LIBUSB_ERROR_PIPE;
}

/* Returns the libusb code (0 on success) rather than a flat -1, so the
 * send thread can tell an unplugged adapter from a transfer that just
 * went wrong. Unplugging is reported once by the caller, not on every
 * pass: this loop runs with no sleep at all, so logging a disconnect here
 * would write thousands of identical lines per second. */
static int send_report(uint8_t type, const uint8_t *data, uint16_t length) {
    if (!g_handle) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    unsigned char buf[REPORT_SIZE];
    build_report(buf, type, data, length);

    int transferred = 0;
    int ret = libusb_interrupt_transfer(g_handle, g_out_ep, buf, sizeof(buf), &transferred, USB_TRANSFER_TIMEOUT_MS);
    if (ret != 0 && !usb_error_is_disconnect(ret)) {
        fprintf(stderr, "gamepad_bridge: failed to send report (type=0x%02x): %s\n", type,
                libusb_error_name(ret));
    }
    return ret;
}

/* Walks the device's USB descriptors to find its HID interface and the
 * addresses of its interrupt IN/OUT endpoints. */
static int find_hid_endpoints(libusb_device *dev, int *interface_number, unsigned char *out_ep,
                               unsigned char *in_ep) {
    struct libusb_config_descriptor *cfg;
    if (libusb_get_config_descriptor(dev, 0, &cfg) != 0) {
        return -1;
    }

    int found = -1;
    for (int itf = 0; itf < cfg->bNumInterfaces && found < 0; itf++) {
        const struct libusb_interface *interface = &cfg->interface[itf];
        for (int alt = 0; alt < interface->num_altsetting && found < 0; alt++) {
            const struct libusb_interface_descriptor *idesc = &interface->altsetting[alt];
            if (idesc->bInterfaceClass != LIBUSB_CLASS_HID) {
                continue;
            }
            unsigned char found_out = 0, found_in = 0;
            for (int ep = 0; ep < idesc->bNumEndpoints; ep++) {
                const struct libusb_endpoint_descriptor *edesc = &idesc->endpoint[ep];
                if ((edesc->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                    continue;
                }
                if ((edesc->bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_OUT) {
                    found_out = edesc->bEndpointAddress;
                } else {
                    found_in = edesc->bEndpointAddress;
                }
            }
            if (found_out) {
                *interface_number = idesc->bInterfaceNumber;
                *out_ep = found_out;
                *in_ep = found_in; /* may stay 0 if absent -- reading will just be disabled */
                found = 0;
            }
        }
    }

    libusb_free_config_descriptor(cfg);
    return found;
}

/* Reads the latest available GPPKG_INPUT_REPORT (state of the real
 * controller plugged into the controller port) and updates g_real_state.
 * Very short timeout: this is a complement to the virtual stream, no
 * question of slowing down sending if nothing is available right away. */
static void read_real_controller_state(void) {
    if (!g_in_ep) {
        return;
    }
    unsigned char buf[REPORT_SIZE] = {0};
    int transferred = 0;
    int ret = libusb_interrupt_transfer(g_handle, g_in_ep, buf, sizeof(buf), &transferred, 3);
    /* Whenever we don't get a fresh, well-formed report, fall back to "no
     * button held" rather than leaving g_real_state at whatever it was --
     * silently keeping a stale value here is what let a single bad/garbage
     * read latch a button as permanently pressed until the next good read
     * (this is currently moot since the merge that consumes g_real_state
     * is disabled above, but keep this correct for when it's re-enabled). */
    if (ret != 0 || transferred < GCAPI_REPORT_INPUT_OFFSET + GAMEPAD_BRIDGE_STATE_COUNT) {
        memset(g_real_state, 0, sizeof(g_real_state));
        return;
    }
    if (buf[0] != GPPKG_INPUT_REPORT) {
        memset(g_real_state, 0, sizeof(g_real_state));
        return;
    }
    memcpy(g_real_state, buf + GCAPI_REPORT_INPUT_OFFSET, GAMEPAD_BRIDGE_STATE_COUNT);
}

/* Releases the interface and handle without touching the libusb context
 * or the send thread: used both when the adapter vanishes and when it is
 * deliberately reset. */
/* Defined below, next to gamepad_bridge_init() which shares it. */
static int usb_open_and_claim(int quiet);

/* Silences usb_open_and_claim()'s diagnostics while it is being retried
 * once a second waiting for the adapter to reappear. Without this, an
 * adapter left unplugged would write "no ConsoleTuner adapter found"
 * to the log every second, forever -- and "gamepad support disabled" is
 * the wrong thing to say about a retry anyway. */
static void open_log(int quiet, const char *fmt, ...) {
    if (quiet) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Sleeps in slices so shutdown is never made to wait for it. A plain
 * SDL_Delay here would hold the send thread for its whole duration, and
 * gamepad_bridge_shutdown() joins that thread before sending
 * LEAVE_CAPTURE -- which is exactly how the adapter used to get killed
 * mid-handover. */
static void interruptible_delay(int total_ms) {
    const int slice = 50;
    for (int waited = 0; waited < total_ms && g_thread_running; waited += slice) {
        SDL_Delay(slice);
    }
}

static void usb_close_device(void) {
    if (!g_handle) {
        return;
    }
    libusb_release_interface(g_handle, g_interface_number);
    libusb_close(g_handle);
    g_handle = NULL;
}

static inline int8_t clamp_i8(int v) {
    if (v > 100) return 100;
    if (v < -100) return -100;
    return (int8_t)v;
}

static int send_thread_main(void *arg) {
    (void)arg;
    /* -1 slots are impossible in a real report, so the first pass always
     * counts as a change and sends. */
    uint8_t last_output[GCAPI_OUTPUT_TOTAL];
    memset(last_output, 0xff, sizeof(last_output));
    Uint32 last_sent_ms = 0;
    const int dedup = (int)config_get_int("GAMEPAD_DEDUP", 1, 0, 1);

    /* Non-zero while a requested HOME press is still being held. */
    Uint32 home_until_ms = 0;

    unsigned long stat_reports = 0;
    double stat_transfer_ms = 0;
    double stat_worst_ms = 0;
    Uint32 stat_since = SDL_GetTicks();
    /* Continuous loop, with no artificial wait whatsoever: each pass
     * re-reads the most recent state, sends it, then immediately re-reads
     * the real controller for the next pass. The only time that elapses
     * between two passes is that of the USB transfers themselves (a few
     * ms, unavoidable) -- specifically NO sleep/condvar with a fixed
     * timeout: a fixed-interval keepalive would make EVERYTHING (including
     * the real controller's passthrough, which now goes through this same
     * loop) follow that imposed pace instead of following the hardware at
     * its own speed. That is exactly what made the real passthrough itself
     * slow as soon as capture mode is active. */
    while (g_thread_running) {
        /* Both branches below are the UNPLUGGED path. When the link is
         * up, execution falls straight through to exactly the same
         * transfers, in the same order, with no added sleep or syscall --
         * the timing that was measured as good is untouched. */
        if (g_reset_requested) {
            g_reset_requested = 0;
            fprintf(stderr, "gamepad_bridge: resetting %s (held closed for %d ms)\n",
                    g_device_name, RESET_HOLD_MS);
            if (g_handle) {
                /* Re-enumerate, then let go of it entirely: the console
                 * only re-handshakes with an adapter that actually went
                 * away for a moment, which is why a physical unplug
                 * worked where an instant reset did not. */
                libusb_reset_device(g_handle);
                usb_close_device();
            }
            g_link_up = 0;
            /* Sleeping here rather than in the reconnect path below: this
             * branch is only reached on an explicit reset, so it cannot
             * add delay to an ordinary unplug. */
            interruptible_delay(RESET_HOLD_MS);
            continue;
        }

        if (!g_link_up) {
            /* Nothing to talk to. Retry at a human pace: this is the only
             * place in this loop allowed to sleep, and it costs nothing
             * because there is no latency to protect while unplugged. */
            if (usb_open_and_claim(1)) {
                fprintf(stderr, "gamepad_bridge: %s reconnected, capture mode active\n", g_device_name);
            } else {
                SDL_Delay(RECONNECT_RETRY_MS);
            }
            continue;
        }

        int8_t snapshot[GAMEPAD_BRIDGE_STATE_COUNT];
        SDL_LockMutex(g_state_mutex);
        memcpy(snapshot, g_latest_state, sizeof(snapshot));
        SDL_UnlockMutex(g_state_mutex);

        /* Real-controller merge temporarily disabled: g_real_state is
         * populated by read_real_controller_state() using an unverified
         * byte offset (GCAPI_REPORT_INPUT_OFFSET, see above), and on top
         * of that a garbage byte read once is never cleared afterwards
         * (read_real_controller_state() silently keeps the previous value
         * whenever a report read fails/times out or the report layout
         * doesn't match). A single bad read at the wrong offset can
         * therefore make one button look permanently held down forever
         * after -- this is exactly what was making "Select" look stuck
         * pressed. Until the real offset is verified (see README goals),
         * only the virtual/browser state drives the output. We still call
         * read_real_controller_state() below to keep this loop's timing
         * identical (same number/order of USB transfers) to the version
         * whose latency was confirmed "perfect" -- do not remove that
         * call, only re-enable using its result once the offset is fixed. */
        uint8_t output[GCAPI_OUTPUT_TOTAL] = {0};
        for (int i = 0; i < GAMEPAD_BRIDGE_STATE_COUNT && i < GCAPI_OUTPUT_TOTAL; i++) {
            output[i] = (uint8_t)clamp_i8((int)snapshot[i]);
        }

        /* A HOME press asked for by a client. Overlaid on whatever else
         * is held, and held for its own duration -- pressing it is a
         * request from elsewhere, not something the client keeps
         * repeating, so the timing lives here. */
        if (g_home_press_requested) {
            g_home_press_requested = 0;
            home_until_ms = SDL_GetTicks() + HOME_PRESS_MS;
            fprintf(stderr, "gamepad_bridge: HOME press requested\n");
        }
        if (home_until_ms && SDL_GetTicks() < home_until_ms) {
            output[GAMEPAD_XB360_GUIDE] = 100;
        } else {
            home_until_ms = 0;
        }

        /* Skip a report that says exactly what the last one said.
         *
         * This loop runs with no pacing and was measured at 300
         * reports/s, nearly all of them identical -- nothing is moving
         * most of the time. If the adapter accepts them faster than the
         * console drains them, the surplus queues up INSIDE the adapter
         * and input latency grows the longer you play, which is the
         * "cumulative latency" symptom: no reset of the USB link can fix
         * a backlog that lives in the device.
         *
         * Real input is unaffected: a changed state still goes out on
         * the very next pass with nothing added to it. Only the silence
         * between presses gets quieter. A keepalive still goes out
         * regularly so the adapter never sees us as gone.
         *
         * Set GAMEPAD_DEDUP=0 in the .env to send every pass again, the
         * way it did before. */
        int changed = memcmp(output, last_output, sizeof(output)) != 0;
        Uint32 now_ticks = SDL_GetTicks();
        if (dedup && !changed && (now_ticks - last_sent_ms) < KEEPALIVE_MS) {
            read_real_controller_state();
            continue;
        }
        memcpy(last_output, output, sizeof(output));
        last_sent_ms = now_ticks;
        Uint64 t_before = SDL_GetPerformanceCounter();
        int ret = send_report(GPPKG_OUTPUT_REPORT, output, GCAPI_OUTPUT_TOTAL);
        double transfer_ms = (SDL_GetPerformanceCounter() - t_before) * 1000.0 /
                             (double)SDL_GetPerformanceFrequency();
        /* Rate and transfer cost of this loop, reported every few
         * seconds. It sends with no pacing at all, so if the adapter
         * accepts reports faster than the console consumes them, they
         * queue up INSIDE the adapter and input latency grows the longer
         * you play -- which no amount of resetting the USB link would
         * fix. This is the number that says whether that is happening. */
        stat_reports++;
        stat_transfer_ms += transfer_ms;
        if (transfer_ms > stat_worst_ms) stat_worst_ms = transfer_ms;
        Uint32 now_ms = SDL_GetTicks();
        if (now_ms - stat_since >= SEND_STATS_INTERVAL_MS) {
            double secs = (now_ms - stat_since) / 1000.0;
            /* Nothing here writes anything on its own.
             *
             * These are the input-latency numbers and they are worth
             * having while that is being looked at -- but a line every
             * few seconds for as long as the program runs is a file
             * growing forever to report that nothing happened. Behind
             * VERBOSE, like every other periodic line. */
            g_reports_per_sec = stat_reports / secs;
            if (app_verbose()) {
                fprintf(stderr, "gamepad_bridge: %.0f reports/s, transfer avg %.2f ms / worst %.2f ms\n",
                        stat_reports / secs, stat_reports ? stat_transfer_ms / stat_reports : 0.0,
                        stat_worst_ms);
            }
            stat_reports = 0;
            stat_transfer_ms = 0;
            stat_worst_ms = 0;
            stat_since = now_ms;
        }
        if (usb_error_is_disconnect(ret)) {
            /* Reported once, here, rather than from send_report(): this
             * loop has no sleep in it, so logging per failed transfer
             * would produce thousands of identical lines a second. */
            fprintf(stderr, "gamepad_bridge: %s disconnected (%s), waiting for it to come back\n",
                    g_device_name, libusb_error_name(ret));
            usb_close_device();
            g_link_up = 0;
            continue;
        }
        read_real_controller_state();
    }
    return 0;
}

/* Finds the adapter, opens it, claims its HID interface and puts it into
 * capture mode. Split out of gamepad_bridge_init() so reconnecting after
 * an unplug runs exactly the same sequence -- including GTuner's startup
 * handshake, which the adapter needs every time it enumerates, not just
 * once at program start.
 *
 * Leaves the libusb context alone on failure: its lifetime belongs to
 * gamepad_bridge_init(), and a failed reconnect attempt must be free to
 * simply be retried a second later. */
static int usb_open_and_claim(int quiet) {
    libusb_device **list;
    ssize_t count = libusb_get_device_list(g_ctx, &list);
    if (count < 0) {
        open_log(quiet, "gamepad_bridge: libusb_get_device_list failed\n");
        return 0;
    }

    /* An explicit USB id in the .env overrides the built-in list, so a
     * device this build has never heard of (a newer adapter, a clone)
     * can be used without touching the source. Both must be set for the
     * override to apply; otherwise the known-devices table is used. */
    long cfg_vid = config_get_int("GAMEPAD_USB_VID", -1, 0, 0xffff);
    long cfg_pid = config_get_int("GAMEPAD_USB_PID", -1, 0, 0xffff);
    int use_config_id = (cfg_vid >= 0 && cfg_pid >= 0);

    libusb_device *target = NULL;
    const char *target_name = NULL;
    for (ssize_t i = 0; i < count && !target; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) {
            continue;
        }
        if (use_config_id) {
            if (desc.idVendor == (uint16_t)cfg_vid && desc.idProduct == (uint16_t)cfg_pid) {
                target = list[i];
                target_name = "configured adapter";
            }
            continue;
        }
        for (int k = 0; k < N_KNOWN_DEVICES; k++) {
            if (desc.idVendor == KNOWN_DEVICES[k].vid && desc.idProduct == KNOWN_DEVICES[k].pid) {
                target = list[i];
                target_name = KNOWN_DEVICES[k].name;
                break;
            }
        }
    }

    if (!target) {
        if (use_config_id) {
            open_log(quiet,
                    "gamepad_bridge: no USB device %04lx:%04lx (GAMEPAD_USB_VID/PID in the .env) found, "
                    "gamepad support disabled\n",
                    cfg_vid, cfg_pid);
        } else {
            open_log(quiet, "gamepad_bridge: no ConsoleTuner adapter (Titan One...) found, "
                            "gamepad support disabled\n");
        }
        libusb_free_device_list(list, 1);
        return 0;
    }

    if (find_hid_endpoints(target, &g_interface_number, &g_out_ep, &g_in_ep) != 0) {
        open_log(quiet, "gamepad_bridge: HID interface/endpoint not found on %s\n", target_name);
        libusb_free_device_list(list, 1);
        return 0;
    }

    if (libusb_open(target, &g_handle) != 0) {
        open_log(quiet, "gamepad_bridge: could not open %s (udev permissions?)\n", target_name);
        libusb_free_device_list(list, 1);
        return 0;
    }
    libusb_free_device_list(list, 1);

    /* Automatically detach/reattach the hid-generic kernel driver around
     * our transfers -- without this, claim_interface fails since the
     * kernel already has control of the HID interface. */
    libusb_set_auto_detach_kernel_driver(g_handle, 1);

    /* Re-enumerate before doing anything else. A previous run that was
     * killed before it could leave capture mode leaves the adapter in a
     * state its startup handshake does NOT clear -- which is exactly why
     * unplugging and replugging it by hand fixed the input latency. This
     * is that replug, so a relaunch always starts from the same place
     * whether the last run exited cleanly or was killed.
     *
     * A reset can invalidate the handle if the device comes back
     * re-enumerated; the caller retries in that case. */
    int reset_ret = libusb_reset_device(g_handle);
    if (reset_ret == LIBUSB_ERROR_NOT_FOUND) {
        libusb_close(g_handle);
        g_handle = NULL;
        return 0;
    }

    if (libusb_claim_interface(g_handle, g_interface_number) != 0) {
        open_log(quiet, "gamepad_bridge: could not claim the HID interface of %s\n", target_name);
        libusb_close(g_handle);
        g_handle = NULL;
        return 0;
    }

    /* Replay GTuner Pro's exact real-world startup sequence (captured via
     * USB sniffing) instead of jumping straight to ENTER_CAPTURE: 0xf0,
     * then LEAVE_CAPTURE (harmless if we weren't in capture mode), then
     * 0xf1, then a short settle delay before ENTER_CAPTURE. See
     * GPPKG_UNKNOWN_F0/F1 above for why. */
    send_report(GPPKG_UNKNOWN_F0, NULL, 0);
    send_report(GPPKG_LEAVE_CAPTURE, NULL, 0);
    send_report(GPPKG_UNKNOWN_F1, NULL, 0);
    SDL_Delay(50);

    if (send_report(GPPKG_ENTER_CAPTURE, NULL, 0) != 0) {
        open_log(quiet, "gamepad_bridge: failed to enter capture mode on %s\n", target_name);
        libusb_release_interface(g_handle, g_interface_number);
        libusb_close(g_handle);
        g_handle = NULL;
        return 0;
    }

    /* A phantom press on "select/back" has been observed systematically
     * at startup, even before a browser connects -- so on entering
     * capture mode itself, not in our handling of browser messages.
     * Hypothesis: a brief transient firmware state during its own
     * ENTER_CAPTURE negotiation. We force several zeroed reports
     * immediately to try to override it; if the problem persists despite
     * this, it is likely a firmware limitation we cannot fix on the
     * software side. */
    {
        uint8_t zero_output[GCAPI_OUTPUT_TOTAL] = {0};
        for (int i = 0; i < 5; i++) {
            send_report(GPPKG_OUTPUT_REPORT, zero_output, GCAPI_OUTPUT_TOTAL);
        }
    }

    g_device_name = target_name;
    g_link_up = 1;
    return 1;
}

int gamepad_bridge_init(void) {
    if (g_connected) {
        return 1;
    }

    if (libusb_init(&g_ctx) != 0) {
        fprintf(stderr, "gamepad_bridge: libusb_init failed\n");
        return 0;
    }

    /* Retried because the reset above can hand back an invalidated
     * handle when the adapter comes back re-enumerated. Quiet after the
     * first go, so an absent adapter still reports itself exactly once. */
    int opened = 0;
    for (int attempt = 0; attempt < GAMEPAD_OPEN_ATTEMPTS && !opened; attempt++) {
        if (attempt > 0) {
            SDL_Delay(GAMEPAD_OPEN_RETRY_MS);
        }
        opened = usb_open_and_claim(attempt > 0);
    }
    if (!opened) {
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return 0;
    }

    g_state_mutex = SDL_CreateMutex();
    if (!g_state_mutex) {
        fprintf(stderr, "gamepad_bridge: mutex creation failed\n");
        libusb_release_interface(g_handle, g_interface_number);
        libusb_close(g_handle);
        g_handle = NULL;
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return 0;
    }
    memset(g_latest_state, 0, sizeof(g_latest_state));
    memset(g_sources, 0, sizeof(g_sources));
    memset(g_real_state, 0, sizeof(g_real_state));

    g_thread_running = 1;
    g_send_thread = SDL_CreateThread(send_thread_main, "gamepad-bridge", NULL);
    if (!g_send_thread) {
        fprintf(stderr, "gamepad_bridge: SDL_CreateThread failed\n");
        g_thread_running = 0;
        SDL_DestroyMutex(g_state_mutex);
        g_state_mutex = NULL;
        libusb_release_interface(g_handle, g_interface_number);
        libusb_close(g_handle);
        g_handle = NULL;
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return 0;
    }

    fprintf(stderr, "gamepad_bridge: connected to %s, capture mode active\n", g_device_name);
    g_connected = 1;
    return 1;
}

void gamepad_bridge_update(unsigned source, const int8_t state[GAMEPAD_BRIDGE_STATE_COUNT]) {
    if (!g_connected) {
        return;
    }
    /* Just storing the value under a mutex: see the comment near
     * g_state_mutex above. */
    SDL_LockMutex(g_state_mutex);

    int slot = -1, free_slot = -1;
    for (int i = 0; i < GAMEPAD_MAX_SOURCES; i++) {
        if (g_sources[i].in_use && g_sources[i].key == source) {
            slot = i;
            break;
        }
        if (!g_sources[i].in_use && free_slot < 0) {
            free_slot = i;
        }
    }
    if (slot < 0) slot = free_slot;

    if (slot >= 0) {
        g_sources[slot].in_use = 1;
        g_sources[slot].key = source;
        g_sources[slot].last_ms = SDL_GetTicks();
        memcpy(g_sources[slot].state, state, sizeof(g_sources[slot].state));
        recombine();
    }
    SDL_UnlockMutex(g_state_mutex);
}

void gamepad_bridge_forget(unsigned source) {
    if (!g_connected) {
        return;
    }
    SDL_LockMutex(g_state_mutex);
    for (int i = 0; i < GAMEPAD_MAX_SOURCES; i++) {
        if (g_sources[i].in_use && g_sources[i].key == source) {
            g_sources[i].in_use = 0;
        }
    }
    recombine();
    SDL_UnlockMutex(g_state_mutex);
}

/* Asks for the adapter to be re-enumerated. Asynchronous on purpose: the
 * send thread owns the USB handle and is using it continuously, so the
 * reset is performed there rather than underneath it.
 *
 * Used after waking the console: the adapter has to re-handshake with a
 * console that was asleep when it last enumerated, and re-plugging it is
 * what makes that happen. */
void gamepad_bridge_press_home(void) {
    if (!g_connected) {
        return;
    }
    g_home_press_requested = 1;
}

void gamepad_bridge_reset(void) {
    if (!g_connected) {
        return;
    }
    g_reset_requested = 1;
}

void gamepad_bridge_shutdown(void) {
    if (!g_connected) {
        return;
    }

    g_thread_running = 0;
    SDL_WaitThread(g_send_thread, NULL);
    g_send_thread = NULL;
    SDL_DestroyMutex(g_state_mutex);
    g_state_mutex = NULL;

    /* Both no-ops if the adapter was unplugged and never came back. */
    int left = send_report(GPPKG_LEAVE_CAPTURE, NULL, 0);
    fprintf(stderr, "gamepad_bridge: left capture mode (%s)\n",
            left == 0 ? "ok" : libusb_error_name(left));
    usb_close_device();
    g_link_up = 0;
    libusb_exit(g_ctx);
    g_ctx = NULL;
    g_connected = 0;
}
