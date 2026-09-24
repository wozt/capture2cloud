#define _GNU_SOURCE

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <execinfo.h>

#include "gamepad_bridge.h"
#include "wiiu_pad.h"
#include "app_config.h"
#include "audio_capture.h"
#include "app_settings.h"
#include "c2s_protocol.h"
#include "gtk_shell.h"
#include "local_pad.h"
#include "video_capture.h"
#include "gst_webrtc.h"
#include "web_stream.h"
#include "switch_stream.h"

#include <libavutil/pixfmt.h>

#define APP_NAME "Capture2Cloud"
/* Fallback only: the real value belongs in the .env (AUDIO_SOURCE), so
 * a different capture card can be used without rebuilding. This default
 * is the card this project was developed against. */
#define DEFAULT_AUDIO_SOURCE "alsa_input.usb-MACROSILICON_USB3_Video_20210623-02.pro-input-0"
#define AUDIO_GATE_THRESHOLD 0
#define AUDIO_GATE_RELEASE_CHUNKS 20
#define AUDIO_HIGHPASS_ALPHA 0.995f
#define AUDIO_NOTCH_COUNT 9
#define AUDIO_NOTCH_Q 20.0f
/* While the capture card is missing: how often to look for it, and how
 * long each pass sleeps. The tick stays short so the window keeps
 * handling events (including "quit") while waiting. */
/* A gap this long between two captured frames is not normal jitter at
* 60 fps (16.7 ms) -- three missed frames is already a visible hitch, and worth a line in the log
 * saying how long it actually was. Quiet in normal operation. */
#define FRAME_GAP_WARN_MS 50
#define VIDEO_REOPEN_RETRY_MS 1000
#define VIDEO_WAIT_TICK_MS 100
#define WEB_STREAM_DEFAULT_PORT 5080
#define WEB_STREAM_AUDIO_RATE 48000
#define WEB_STREAM_AUDIO_CHANNELS 2
#define WEB_STREAM_AUDIO_PACKET_FRAMES 480

/* Whatever the video/audio modules and the UI all need to agree on.
 * The V4L2 buffers, JPEG decoding and PulseAudio filtering that used to
 * live here now belong to video_capture.c / audio_capture.c. */
struct app {
    unsigned int width;
    unsigned int height;
    volatile sig_atomic_t running;
};



static struct app g_app = {
    .width = 1920,
    .height = 1080,
    .running = 1,
};

/* No window, no GTK control bar: capture, encode and serve, nothing
 * drawn locally. For running over SSH, from a systemd unit, or on a
 * machine with no desktop session at all -- the web page becomes the
 * only interface, which is why the stream is started unconditionally
 * below. */
static int g_headless = 0;

static VideoCapture *g_video = NULL;
static WebStream *g_web = NULL;
static GstWebrtcStream *g_gst = NULL;
static GtkShell *g_shell = NULL;
static AudioCapture *g_audio = NULL;

/* The video window, and the ask to bring it back. Owned by the main
 * loop; the tray only ever raises the flag. */
static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static volatile int g_show_window_requested = 0;

/* What the local interface can change, and what is actually in force.
 * The settings window sends its whole set; on_settings() compares. */
static AppSettings g_settings = APP_SETTINGS_DEFAULTS;
static SwitchStream *g_switch = NULL;
/*
 * The Wii U GamePad client, when one is running. A separate program;
 * see wiiu_pad.h for why it is not a thread in here.
 */
static WiiuPad *g_wiiu_pad = NULL;

typedef enum {
    WIIU_PAD_REQUEST_NONE = 0,
    WIIU_PAD_REQUEST_ENABLE,
    WIIU_PAD_REQUEST_DISABLE,
    WIIU_PAD_REQUEST_RESTART,
    WIIU_PAD_REQUEST_STOP,
} WiiuPadRequest;

/*
 * Written by GTK callbacks, consumed by the main loop.
 *
 * The old path called wiiu_pad_stop() directly from GTK, which can wait
 * up to five seconds for libdrc to exit. During that wait the entire
 * settings application stopped navigating or repainting.
 */
static SDL_atomic_t g_wiiu_pad_request;
/*
 * Where this program was started from, which is where its `wiiu`
 * subdirectory is. Taken from argv[0] for the same reason the restart
 * above uses it: the launcher always invokes this with a full path.
 */
static char g_project_dir[PATH_MAX] = ".";
static int g_web_port = WEB_STREAM_DEFAULT_PORT; /* overridden from the .env at startup */


static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--headless] [video device]\n"
            "\n"
            "  --headless   no local window and no GTK control bar; the web\n"
            "               stream starts on its own and is the only interface.\n"
            "               Stop it with SIGINT/SIGTERM.\n"
            "  --help       this message\n"
            "\n"
            "The video device also comes from VIDEO_DEVICE in scripts/.env;\n"
            "an argument here overrides it for a one-off run.\n",
            argv0);
}

static void on_signal(int sig) {
    (void)sig;
    g_app.running = 0;
}

static void on_crash_signal(int sig) {
    void *frames[64];
    int n = backtrace(frames, 64);
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "\n[crash] signal %d, backtrace:\n", sig);
    write(2, msg, (size_t)len);
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}




/* Reports to stderr in every mode -- headless has no other way to say
 * what happened, and in windowed mode the log is where you look anyway
 * once the message box has been dismissed. The GTK calls are skipped
 * rather than passed a NULL shell. */
static void start_or_report_web_stream(void) {
    char err[256];
    if (web_stream_start(g_web, g_web_port, err, sizeof(err)) != 0) {
        fprintf(stderr, "web stream: failed to start on port %d: %s\n", g_web_port, err);
        g_settings.stream_enabled = 0;
        if (g_shell) {
            gtk_shell_show_error(g_shell, err);
            gtk_shell_update(g_shell, &g_settings);
        }
    } else {
        fprintf(stderr, "web stream: listening on port %d\n", g_web_port);
        g_settings.stream_enabled = 1;
        if (g_shell) {
            gtk_shell_update(g_shell, &g_settings);
        }
    }
}

/* Everything the settings window can change, applied in one place.
 *
 * The interface hands over the whole set rather than one field at a
 * time, so this compares against what is already in force and acts only
 * on what differs. One place to look for "what happens when I move
 * that", instead of a callback per control.
 *
 * Nothing here checks a password. The person at this keyboard is at the
 * machine the console is plugged into; a login would guard a door they
 * are standing behind. */
/* How many console clients are connected, for the page's viewer count.
 * Read through the current server rather than a captured pointer: it is
 * replaced whenever the port changes. */
/* A browser whose WebSocket handshake is done, handed to the transport
 * that already serves the console and the phone. Looked up rather than
 * captured: the native server is stopped and started when its port
 * changes, so a pointer taken once would go stale. */
static int adopt_web_socket(void *ctx, int fd, int may_control) {
    (void)ctx;
    if (!g_switch) {
        return -1;
    }
    return switch_stream_adopt_websocket(g_switch, fd, may_control);
}

static void count_native_clients(void *ctx, int *now, int *max) {
    (void)ctx;
    *now = switch_stream_client_count(g_switch);
    *max = g_switch ? switch_stream_max_clients() : 0;
}

/* Opens the video window, or brings it back if it is already there.
 *
 * The same path at startup and later, because "show capture" has to work
 * from three states that look different and are not: never opened
 * (headless), closed by its own button, and minimised. Called only from
 * the main loop -- SDL's window calls belong on the thread that
 * initialised video, and the tray runs on its own. */
static int open_capture_window(void) {
    /* Sound comes with the picture. Showing the capture brought the
     * window back and nothing else, because in a session that started
     * headless the speakers were never opened -- and the only chance to
     * open them had gone by half an hour earlier. They are asked for
     * here; the capture thread opens them, since it owns the stream.
     * Muting is left alone: it is a separate answer to a separate
     * question. */
    audio_capture_set_local_output(g_audio, 1);

    if (g_window) {
        SDL_ShowWindow(g_window);
        SDL_RestoreWindow(g_window);
        SDL_RaiseWindow(g_window);
        return 0;
    }

    /* Video was never initialised in headless mode, so it is initialised
     * now. Everything else -- the capture, the encoders, the servers --
     * has been running all along; this only adds somewhere to look. */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "show capture: no video (%s)\n", SDL_GetError());
        return -1;
    }
    /*
     * IMPORTANT:
     *
     * The local preview lives in the SAME loop as V4L2 capture.
     * A vsynced SDL_RenderPresent() therefore does not merely delay the
     * preview: it prevents us from returning to VIDIOC_DQBUF for the
     * next capture frame.
     *
     * The capture card already paces us at 60 Hz. The preview must be a
     * consumer of that cadence, never its clock.
     */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    g_window = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                (int)g_app.width, (int)g_app.height,
                                /* Decorated, like any other window: close, minimise
                                 * and maximise come from the window manager. It was
                                 * borderless so a GTK bar could pretend to be its
                                 * title bar, which meant reimplementing all three
                                 * badly. */
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    /* The same icon the tray and the page show. A BMP rather than the PNG
     * the rest use, because SDL loads BMP on its own and pulling in
     * SDL_image to draw one 64-pixel square would be a dependency for a
     * decoration. Failing to load it costs the window its icon and
     * nothing else, so it is not worth stopping for. */
    {
        char path[512];
        app_path(path, sizeof(path), "assets/icon-64.bmp");
        SDL_Surface *icon = SDL_LoadBMP(path);
        if (icon) {
            SDL_SetWindowIcon(g_window, icon);
            SDL_FreeSurface(icon);
        }
    }

    /*
     * No PRESENTVSYNC here.
     *
     * Remote streaming is the timing-critical output. If the desktop
     * compositor needs to delay the preview, it must not stall capture.
     */
    g_renderer = SDL_CreateRenderer(
        g_window,
        -1,
        SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        return -1;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(g_renderer, &info) == 0) {
        fprintf(stderr,
                "SDL: renderer '%s', preview vsync %s (capture is never paced by preview)\n",
                info.name,
                (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "ON" : "OFF");
    }
    return 0;
}

static void process_wiiu_pad_request(void)
{
    const int request = SDL_AtomicSet(
        &g_wiiu_pad_request,
        WIIU_PAD_REQUEST_NONE);

    if (request == WIIU_PAD_REQUEST_NONE) {
        return;
    }

    switch (request) {
        case WIIU_PAD_REQUEST_ENABLE:
            gst_webrtc_stream_set_drc_enabled(g_gst, 1);

            if (!g_switch) {
                fprintf(stderr,
                        "wiiu_pad: not started -- native transport is off\n");
                return;
            }

            if (g_wiiu_pad) {
                wiiu_pad_request_start(g_wiiu_pad);
            } else {
                g_wiiu_pad =
                    wiiu_pad_start(g_project_dir, C2S_DRC_PORT);
            }
            break;

        case WIIU_PAD_REQUEST_DISABLE:
            gst_webrtc_stream_set_drc_enabled(g_gst, 0);

            if (g_wiiu_pad) {
                wiiu_pad_request_stop(g_wiiu_pad);
            }
            break;

        case WIIU_PAD_REQUEST_RESTART:
            gst_webrtc_stream_set_drc_enabled(g_gst, 1);

            if (!g_switch) {
                fprintf(stderr,
                        "wiiu_pad: native transport is off, "
                        "so there is nothing to connect to\n");
                return;
            }

            if (g_wiiu_pad) {
                wiiu_pad_request_restart(g_wiiu_pad);
            } else {
                g_wiiu_pad =
                    wiiu_pad_start(g_project_dir, C2S_DRC_PORT);
            }
            break;

        case WIIU_PAD_REQUEST_STOP:
            gst_webrtc_stream_set_drc_enabled(g_gst, 0);

            if (g_wiiu_pad) {
                wiiu_pad_request_stop(g_wiiu_pad);
            }
            break;

        default:
            break;
    }
}

/*
 * One line per client family for the settings window, once a second.
 *
 * Each page shows its own: four encodes with four audiences, and a
 * single global "3 watching" could never say which of them a picture
 * belonged to. The numbers come from the chain itself rather than being
 * tracked here, so a line cannot disagree with what is being encoded.
 */
static void publish_client_status(void) {
    static const struct {
        int slot;
        GtkShellClient client;
    } MAP[] = {
        { SS_STREAM_WEB,  GTK_SHELL_CLIENT_BROWSER },
        { SS_STREAM_H264, GTK_SHELL_CLIENT_NATIVE },
        { SS_STREAM_DRC,  GTK_SHELL_CLIENT_WIIU_PAD },
        { SS_STREAM_WIIU, GTK_SHELL_CLIENT_WIIU_CONSOLE },
    };

    for (size_t i = 0; i < sizeof(MAP) / sizeof(*MAP); i++) {
        int w = 0, h = 0, fps = 0, kbps = 0;
        const int clients = gst_webrtc_stream_slot_info(g_gst, MAP[i].slot, &w, &h, &fps, &kbps);
        char line[160];
        if (clients <= 0) {
            /* Said plainly, because it is the good case: a chain with
             * nobody on it is not fed at all, and that is the rule that
             * makes five encodes affordable on one machine. */
            snprintf(line, sizeof(line), "nothing connected — not encoding");
        } else {
            snprintf(line, sizeof(line), "%d connected — %dx%d@%d, %d kbps", clients, w, h,
                     fps > 0 ? fps : 60, kbps);
        }
        gtk_shell_set_client_status(g_shell, MAP[i].client, line);
    }

    /*
     * The browsers' line counts the H.264 chain only, and that is not
     * the whole audience: a browser on WebRTC is on the VP8 chain
     * instead. Both are browsers, so both are said.
     */
    int vw = 0, vh = 0, vfps = 0, vkbps = 0;
    const int vp8 = gst_webrtc_stream_slot_info(g_gst, SS_STREAM_VP8, &vw, &vh, &vfps, &vkbps);
    if (vp8 > 0) {
        char line[160];
        int w = 0, h = 0, fps = 0, kbps = 0;
        const int h264 = gst_webrtc_stream_slot_info(g_gst, SS_STREAM_WEB, &w, &h, &fps, &kbps);
        snprintf(line, sizeof(line), "%d on h264 (%dx%d, %d kbps), %d on vp8 (%dx%d, %d kbps)",
                 h264, w, h, kbps, vp8, vw, vh, vkbps);
        gtk_shell_set_client_status(g_shell, GTK_SHELL_CLIENT_BROWSER, line);
    }
}

static void publish_server_status(void)
{
    if (!g_shell) {
        return;
    }

    char line[192];

    snprintf(
        line,
        sizeof(line),
        "running — capture %s — browser server %s — native server %s",
        g_video ? "online" : "waiting",
        (g_web && web_stream_is_running(g_web)) ? "listening" : "off",
        g_switch ? "listening" : "off");

    gtk_shell_set_status(g_shell, line);
}

static void on_settings(void *userdata, const AppSettings *want) {
    (void)userdata;
    AppSettings *have = &g_settings;

    if (want->web_port != have->web_port && want->web_port > 0) {
        have->web_port = want->web_port;
        g_web_port = want->web_port;
        if (web_stream_is_running(g_web)) {
            web_stream_stop(g_web);
            start_or_report_web_stream();
        }
    }
    if (want->stream_enabled != have->stream_enabled) {
        have->stream_enabled = want->stream_enabled;
        if (want->stream_enabled) {
            start_or_report_web_stream();
        } else {
            web_stream_stop(g_web);
        }
    }
    if (want->switch_enabled != have->switch_enabled ||
        (want->switch_port != have->switch_port && want->switch_port > 0)) {
        have->switch_enabled = want->switch_enabled;
        if (want->switch_port > 0) {
            have->switch_port = want->switch_port;
        }
        /* Torn down and started again: a listening socket cannot be
         * moved. Whoever was connected is dropped, which is the honest
         * outcome -- they were told to knock on a door that is no longer
         * there, and the console has to be pointed at the new one. */
        switch_stream_stop(g_switch);
        g_switch = have->switch_enabled
                       ? switch_stream_start(g_web, (uint16_t)have->switch_port)
                       : NULL;
        /* Told either way: a NULL output is what makes the encoder stop
         * being fed rather than encoding for a server that is gone. */
        gst_webrtc_stream_set_switch_output(g_gst, g_switch);
        /*
         * The pad client connects to that port, so a port that just
         * moved takes it with it. Stopped unconditionally and started
         * again below if it is still wanted.
         */
        if (g_wiiu_pad) {
            wiiu_pad_stop(g_wiiu_pad);
            g_wiiu_pad = NULL;
        }
    }
    if (want->wiiu_pad_bitrate_mbps != have->wiiu_pad_bitrate_mbps &&
        want->wiiu_pad_bitrate_mbps > 0) {
        have->wiiu_pad_bitrate_mbps = want->wiiu_pad_bitrate_mbps;
        gst_webrtc_stream_set_drc_bitrate(g_gst, have->wiiu_pad_bitrate_mbps * 1000);
        config_set_int("WIIU_PAD_BITRATE_MBPS", have->wiiu_pad_bitrate_mbps);
    }
    /*
     * The Wii U console client. Its own flag, its own chain, and nothing
     * to start: unlike the pad's, there is no separate program here --
     * the client runs on the console and knocks on the door itself.
     */
    if (want->wiiu_console_enabled != have->wiiu_console_enabled) {
        have->wiiu_console_enabled = want->wiiu_console_enabled;
        gst_webrtc_stream_set_wiiu_enabled(g_gst, have->wiiu_console_enabled);
        /* Remembered across restarts for the reason the pad's is: it is
         * a decision about this machine, not a preference belonging to
         * whoever last opened the window. */
        config_set_int("WIIU_CONSOLE_ENABLED", have->wiiu_console_enabled ? 1 : 0);
    }
    if (want->wiiu_console_height != have->wiiu_console_height ||
        want->wiiu_console_bitrate_mbps != have->wiiu_console_bitrate_mbps) {
        if (want->wiiu_console_height > 0) {
            have->wiiu_console_height = want->wiiu_console_height;
        }
        if (want->wiiu_console_bitrate_mbps > 0) {
            have->wiiu_console_bitrate_mbps = want->wiiu_console_bitrate_mbps;
        }
        gst_webrtc_stream_set_wiiu_profile(g_gst, have->wiiu_console_height,
                                           have->wiiu_console_bitrate_mbps * 1000);
        config_set_int("WIIU_CONSOLE_HEIGHT", have->wiiu_console_height);
        config_set_int("WIIU_CONSOLE_BITRATE_MBPS", have->wiiu_console_bitrate_mbps);
    }
    if (want->wiiu_pad_enabled != have->wiiu_pad_enabled ||
        (want->wiiu_pad_enabled && !g_wiiu_pad)) {
        have->wiiu_pad_enabled = want->wiiu_pad_enabled;

        /*
         * Persist immediately, but do not touch the child process from
         * the GTK callback. Starting/stopping it belongs to the main
         * loop, and the supervisor itself now stops asynchronously.
         */
        config_set_int(
            "WIIU_PAD_AUTOSTART",
            want->wiiu_pad_enabled ? 1 : 0);

        SDL_AtomicSet(
            &g_wiiu_pad_request,
            want->wiiu_pad_enabled
                ? WIIU_PAD_REQUEST_ENABLE
                : WIIU_PAD_REQUEST_DISABLE);
    }

    if (want->browser_height != have->browser_height) {
        have->browser_height = want->browser_height;
        const int w = want->browser_height * 16 / 9;
        gst_webrtc_stream_set_browser_resolution(g_gst, w & ~1, want->browser_height);
    }
    if (want->bitrate_mbps != have->bitrate_mbps) {
        have->bitrate_mbps = want->bitrate_mbps;
        gst_webrtc_stream_set_video_bitrate(g_gst, want->bitrate_mbps * 1000);
    }
    if (want->capture_mjpeg != have->capture_mjpeg) {
        have->capture_mjpeg = want->capture_mjpeg;
        video_capture_request_format(want->capture_mjpeg ? VIDEO_FORMAT_MJPEG
                                                         : VIDEO_FORMAT_YUYV);
        /* Shared by everyone watching, so the native clients are told
         * rather than left to find out. */
        gst_webrtc_stream_set_capture_mjpeg(g_gst, want->capture_mjpeg);
    }
    if (want->local_muted != have->local_muted || want->local_volume != have->local_volume) {
        have->local_muted = want->local_muted;
        have->local_volume = want->local_volume;
        /* Muting silences an open stream; it does not close the
         * speakers. Closing them on every tick of a checkbox would mean
         * tearing a device down and building it again to answer a
         * question about volume. */
        audio_capture_set_local_mute(g_audio, want->local_muted);
        audio_capture_set_local_volume(g_audio, want->local_volume);
    }
    /* Its own test, not folded in with mute and volume above: this one
     * does close the stream and build it again, because there is no
     * moving a stream from one output to another. */
    if (want->local_direct_sink != have->local_direct_sink) {
        have->local_direct_sink = want->local_direct_sink;
        audio_capture_set_local_direct(g_audio, want->local_direct_sink);
    }

    /* The rest are read where they are used -- the controller poll, the
     * drawing -- so storing them is applying them. */
    if (want->gamepad_enabled != have->gamepad_enabled) {
        have->gamepad_enabled = want->gamepad_enabled;

        config_set_int(
            "LOCAL_GAMEPAD_ENABLED",
            have->gamepad_enabled);
    }

    if (want->gamepad_index != have->gamepad_index) {
        have->gamepad_index = want->gamepad_index;

        char guid[64];

        if (have->gamepad_index >= 0 &&
            local_pad_guid_for_slot(
                have->gamepad_index,
                guid,
                sizeof(guid))) {
            config_set_str(
                "LOCAL_GAMEPAD_GUID",
                guid);
        } else {
            /*
             * config_set_str() deliberately refuses empty values, so use
             * an explicit token for "no preferred local controller".
             */
            config_set_str(
                "LOCAL_GAMEPAD_GUID",
                "none");
        }
    }

    /*
     * Output backend is a startup choice: each implementation owns
     * resources with very different lifetimes (USB interfaces, BLE
     * peripheral state, UDP sockets...). Switching those underneath
     * active input threads would be much harder to reason about than
     * stopping them in the shutdown order we already trust.
     *
     * Persist the selection, then use the ordinary in-process restart.
     */
    if (want->output_backend != have->output_backend) {
        const int index = want->output_backend;

        if (index < 0 ||
            index >= gamepad_bridge_backend_count() ||
            !gamepad_bridge_backend_available(index)) {
            if (g_shell) {
                gtk_shell_show_error(
                    g_shell,
                    "That controller output backend is not implemented "
                    "in this build yet.");
                gtk_shell_update(g_shell, have);
            }
        } else {
            const char *name = gamepad_bridge_backend_name_at(index);

            if (config_set_str("GAMEPAD_OUTPUT_BACKEND", name) != 0) {
                if (g_shell) {
                    gtk_shell_show_error(
                        g_shell,
                        "Could not save GAMEPAD_OUTPUT_BACKEND to scripts/.env.");
                    gtk_shell_update(g_shell, have);
                }
            } else {
                have->output_backend = index;
                fprintf(stderr,
                        "controller output: backend changed to %s; restarting\n",
                        name);
                app_request_restart();
            }
        }
    }

    /*
     * LOCAL SDL input shaping.
     *
     * local_pad_poll() reads g_settings every pass, so assigning these
     * values is the live application. Persist only the individual value
     * that moved; dragging one slider must not rewrite the .env dozens
     * of unnecessary times.
     */
    if (want->invert_ry != have->invert_ry) {
        have->invert_ry = want->invert_ry;
        config_set_int("INPUT_INVERT_RY", have->invert_ry);
    }

    if (want->lt_threshold != have->lt_threshold) {
        have->lt_threshold = want->lt_threshold;
        config_set_int("INPUT_LT_THRESHOLD", have->lt_threshold);
    }

    if (want->rt_threshold != have->rt_threshold) {
        have->rt_threshold = want->rt_threshold;
        config_set_int("INPUT_RT_THRESHOLD", have->rt_threshold);
    }

    static const char *const INPUT_DEADZONE_KEY[2] = {
        "INPUT_LEFT_STICK_DEADZONE",
        "INPUT_RIGHT_STICK_DEADZONE",
    };

    static const char *const INPUT_RANGE_KEY[2] = {
        "INPUT_LEFT_STICK_RANGE",
        "INPUT_RIGHT_STICK_RANGE",
    };

    static const char *const INPUT_DIAGONAL_KEY[2] = {
        "INPUT_LEFT_STICK_DIAGONAL",
        "INPUT_RIGHT_STICK_DIAGONAL",
    };

    for (int i = 0; i < 2; i++) {
        if (want->stick_deadzone[i] != have->stick_deadzone[i]) {
            have->stick_deadzone[i] = want->stick_deadzone[i];
            config_set_int(
                INPUT_DEADZONE_KEY[i],
                have->stick_deadzone[i]);
        }

        if (want->stick_range[i] != have->stick_range[i]) {
            have->stick_range[i] = want->stick_range[i];
            config_set_int(
                INPUT_RANGE_KEY[i],
                have->stick_range[i]);
        }

        if (want->stick_diagonal[i] != have->stick_diagonal[i]) {
            have->stick_diagonal[i] = want->stick_diagonal[i];
            config_set_int(
                INPUT_DIAGONAL_KEY[i],
                have->stick_diagonal[i]);
        }
    }

    /*
     * FINAL output shaping.
     *
     * Unlike local input shaping, this affects every source. The bridge
     * recombines and sends the currently-held state immediately when one
     * of these values changes.
     */
    int output_shaping_changed = 0;

    if (want->output_invert_ry != have->output_invert_ry) {
        have->output_invert_ry = want->output_invert_ry;
        config_set_int("OUTPUT_INVERT_RY", have->output_invert_ry);
        output_shaping_changed = 1;
    }

    if (want->output_lt_threshold != have->output_lt_threshold) {
        have->output_lt_threshold = want->output_lt_threshold;
        config_set_int(
            "OUTPUT_LT_THRESHOLD",
            have->output_lt_threshold);
        output_shaping_changed = 1;
    }

    if (want->output_rt_threshold != have->output_rt_threshold) {
        have->output_rt_threshold = want->output_rt_threshold;
        config_set_int(
            "OUTPUT_RT_THRESHOLD",
            have->output_rt_threshold);
        output_shaping_changed = 1;
    }

    static const char *const OUTPUT_DEADZONE_KEY[2] = {
        "OUTPUT_LEFT_STICK_DEADZONE",
        "OUTPUT_RIGHT_STICK_DEADZONE",
    };

    static const char *const OUTPUT_RANGE_KEY[2] = {
        "OUTPUT_LEFT_STICK_RANGE",
        "OUTPUT_RIGHT_STICK_RANGE",
    };

    static const char *const OUTPUT_DIAGONAL_KEY[2] = {
        "OUTPUT_LEFT_STICK_DIAGONAL",
        "OUTPUT_RIGHT_STICK_DIAGONAL",
    };

    for (int i = 0; i < 2; i++) {
        if (want->output_stick_deadzone[i] !=
            have->output_stick_deadzone[i]) {
            have->output_stick_deadzone[i] =
                want->output_stick_deadzone[i];

            config_set_int(
                OUTPUT_DEADZONE_KEY[i],
                have->output_stick_deadzone[i]);

            output_shaping_changed = 1;
        }

        if (want->output_stick_range[i] !=
            have->output_stick_range[i]) {
            have->output_stick_range[i] =
                want->output_stick_range[i];

            config_set_int(
                OUTPUT_RANGE_KEY[i],
                have->output_stick_range[i]);

            output_shaping_changed = 1;
        }

        if (want->output_stick_diagonal[i] !=
            have->output_stick_diagonal[i]) {
            have->output_stick_diagonal[i] =
                want->output_stick_diagonal[i];

            config_set_int(
                OUTPUT_DIAGONAL_KEY[i],
                have->output_stick_diagonal[i]);

            output_shaping_changed = 1;
        }
    }

    if (output_shaping_changed) {
        ControllerShaping shaping = {
            .invert_ry = have->output_invert_ry,
            .lt_threshold = have->output_lt_threshold,
            .rt_threshold = have->output_rt_threshold,
            .stick_deadzone = {
                have->output_stick_deadzone[0],
                have->output_stick_deadzone[1],
            },
            .stick_range = {
                have->output_stick_range[0],
                have->output_stick_range[1],
            },
            .stick_diagonal = {
                have->output_stick_diagonal[0],
                have->output_stick_diagonal[1],
            },
        };

        gamepad_bridge_set_output_shaping(&shaping);
    }

    have->brightness = want->brightness;
    have->contrast = want->contrast;
    have->vsync = want->vsync;

    /* This one is not stored and applied, it is asked of the adapter:
     * the value lives in the device, not here. Only sent when it moves,
     * because the bridge writes to non-volatile memory. */
    if (want->output_protocol != have->output_protocol && want->output_protocol >= 0) {
        have->output_protocol = want->output_protocol;
        gamepad_bridge_request_output_protocol(want->output_protocol);
    }
}

/*
 * Pairing, from the settings window.
 *
 * Backgrounded like the wake script: the exchange takes about a minute
 * of waiting for somebody to press sync on a pad, and the interface
 * must not sit on that. Its output goes to this program's log, which is
 * where the rest of the radio's noise already is.
 *
 * The PIN is eight digits and is checked here rather than trusted: it
 * arrives from a dialog this program wrote, but it is about to be
 * pasted into a shell command, and a value that reaches a shell should
 * be one nothing else can have touched.
 */
static void on_pair(void *userdata, const char *pin) {
    (void)userdata;
    if (!pin) {
        return;
    }
    size_t n = strlen(pin);
    if (n != 8) {
        fprintf(stderr, "wiiu: refusing a pairing PIN that is not eight digits\n");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            fprintf(stderr, "wiiu: refusing a pairing PIN that is not digits\n");
            return;
        }
    }
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "%s/wiiu_gamepad/tools/ap-pair.sh %s &", g_project_dir, pin);
    fprintf(stderr, "wiiu: pairing, PIN %s -- press sync on the pad\n", pin);
    if (system(cmd) != 0) {
        fprintf(stderr, "wiiu: the pairing script did not run\n");
    }
}

static void on_action(void *userdata, GtkShellAction action) {
    (void)userdata;
    switch (action) {
        case GTK_SHELL_ACTION_SHOW_CAPTURE:
            /* Only a request: the window belongs to the main loop. */
            g_show_window_requested = 1;
            break;
        case GTK_SHELL_ACTION_WAKE_CONSOLE:
            web_stream_wake_console(g_web);
            break;
        case GTK_SHELL_ACTION_RECOVER_OUTPUT:
            /*
             * The public action is "recover this backend"; what that
             * means belongs to the selected implementation.
             *
             * Titan currently maps it to USB re-enumeration. Future BLE
             * and JOCP backends can implement their own recovery without
             * pretending all three devices have a meaningful "reset".
             */
            gamepad_bridge_reset();
            break;
        case GTK_SHELL_ACTION_WIIU_START:
            SDL_AtomicSet(
                &g_wiiu_pad_request,
                WIIU_PAD_REQUEST_RESTART);
            break;

        case GTK_SHELL_ACTION_WIIU_STOP:
            SDL_AtomicSet(
                &g_wiiu_pad_request,
                WIIU_PAD_REQUEST_STOP);
            break;
        case GTK_SHELL_ACTION_WIIU_CONSOLE_KEYFRAME:
            /* Every chain gets one, not just the console's: the request
             * is "start the picture over" and the host has one gesture
             * for that. A chain nobody is on ignores it, because a
             * chain nobody is on is not running. */
            gst_webrtc_stream_request_keyframe(g_gst);
            fprintf(stderr, "wiiu: keyframe asked for by hand\n");
            break;
        case GTK_SHELL_ACTION_WIIU_AP_START:
        case GTK_SHELL_ACTION_WIIU_AP_STOP:
        case GTK_SHELL_ACTION_WIIU_DEAUTH: {
            /*
             * The radio, from the window. Backgrounded with a trailing
             * "&" like the wake script: bringing an access point up
             * takes seconds and the interface must not wait for it.
             *
             * hostapd binds an interface and so needs root; the script
             * asks through sudo and reports if it cannot. The deauth
             * does not -- the control socket belongs to the sudo group.
             */
            /* PATH_MAX for the directory, plus room for the rest: the
             * project path is whatever somebody cloned into. */
            char cmd[PATH_MAX + 64];
            const char *what = (action == GTK_SHELL_ACTION_WIIU_AP_START) ? "start"
                             : (action == GTK_SHELL_ACTION_WIIU_AP_STOP)  ? "stop"
                                                                          : "deauth";
            snprintf(cmd, sizeof(cmd), "%s/wiiu_gamepad/tools/ap.sh %s &", g_project_dir, what);
            fprintf(stderr, "wiiu: ap.sh %s\n", what);
            if (system(cmd) != 0) {
                fprintf(stderr, "wiiu: %s did not run\n", what);
            }
            break;
        }
        case GTK_SHELL_ACTION_RESTART:
            app_request_restart();
            break;
        case GTK_SHELL_ACTION_QUIT:
            g_app.running = 0;
            break;
    }
}

/* Closes the capture and arms the reopen path in the main loop. Reported
 * once, on the transition, rather than per failed frame. */
static void video_capture_lost(void) {
    if (!g_video) {
        return;
    }
    fprintf(stderr, "video_capture: device lost, waiting for it to come back\n");
    video_capture_close(g_video);
    g_video = NULL;
}

/* The capture's own description of a frame, in the terms swscale uses. */
static int av_format_of(VideoPixelFormat pixel) {
    switch (pixel) {
        case VIDEO_PIXEL_YUYV422: return AV_PIX_FMT_YUYV422;
        case VIDEO_PIXEL_YUV420P: return AV_PIX_FMT_YUV420P;
        case VIDEO_PIXEL_YUV422P: return AV_PIX_FMT_YUV422P;
        default:                  return AV_PIX_FMT_RGB24;
    }
}

/* Draws one capture frame in the local window, building the texture on
 * first sight and rebuilding it if the capture starts producing
 * something else.
 *
 * Nothing is converted on the way. YUYV goes up as the card's own bytes;
 * planar YUV goes up as its three planes. The only trick is 4:2:2, which
 * SDL has no texture for: an I420 texture reads half as many chroma rows
 * as luma ones, so handing it the chroma planes with doubled strides
 * makes it take every other row -- which is exactly the vertical
 * subsampling that turns 4:2:2 into 4:2:0, done by the upload rather
 * than by the CPU. */
static void show_frame(SDL_Renderer *renderer, SDL_Texture **texture, int *texture_pixel,
                       const VideoFrame *frame, int width, int height) {
    if (*texture_pixel != (int)frame->pixel || !*texture) {
        Uint32 fmt;
        switch (frame->pixel) {
            case VIDEO_PIXEL_YUYV422: fmt = SDL_PIXELFORMAT_YUY2; break;
            case VIDEO_PIXEL_RGB24:   fmt = SDL_PIXELFORMAT_RGB24; break;
            default:                  fmt = SDL_PIXELFORMAT_IYUV; break;
        }
        if (*texture) {
            SDL_DestroyTexture(*texture);
        }
        *texture = SDL_CreateTexture(renderer, fmt, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!*texture) {
            fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
            return;
        }
        *texture_pixel = (int)frame->pixel;
    }

    if (frame->pixel == VIDEO_PIXEL_YUV420P || frame->pixel == VIDEO_PIXEL_YUV422P) {
        const int chroma_step = (frame->pixel == VIDEO_PIXEL_YUV422P) ? 2 : 1;
        SDL_UpdateYUVTexture(*texture, NULL,
                             frame->plane[0], frame->stride[0],
                             frame->plane[1], frame->stride[1] * chroma_step,
                             frame->plane[2], frame->stride[2] * chroma_step);
    } else {
        SDL_UpdateTexture(*texture, NULL, frame->plane[0], frame->stride[0]);
    }
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, *texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static void remember_project_dir(const char *argv0) {
    char buf[PATH_MAX];
    if (argv0 && strchr(argv0, '/')) {
        snprintf(buf, sizeof(buf), "%s", argv0);
    } else {
        /* Not the usual path, but better than looking in the working
         * directory, which is wherever the shell happened to be. */
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0)
            return;
        buf[n] = '\0';
    }
    char *slash = strrchr(buf, '/');
    if (!slash)
        return;
    *slash = '\0';
    snprintf(g_project_dir, sizeof(g_project_dir), "%s", buf);
}

int main(int argc, char **argv) {
    remember_project_dir(argv[0]);
    /* Precedence: command line (handy for a one-off), then the .env,
     * then a last-resort default. Putting it in the .env is what lets a
     * different capture card be used without touching the code or the
     * launcher script. */
    char video_device_buf[PATH_MAX];
    const char *video_device = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            video_device = argv[i];
        }
    }
    if (!video_device) {
        video_device = config_get_str("VIDEO_DEVICE", video_device_buf, sizeof(video_device_buf), "/dev/video0");
    }

    /* Web server port/autostart come from the .env, the single place
     * this project is configured. (There used to be a second source,
     * ~/.config/capture2cloud.conf, which silently took precedence --
     * two files disagreeing about the same setting is exactly the kind
     * of surprise this project is trying to avoid, so it's gone.
     * Changes made from the GTK menu apply for the session; edit the
     * .env to make them stick.) */
    g_web_port = (int)config_get_int("WEB_PORT", WEB_STREAM_DEFAULT_PORT, 1, 65535);
    int web_autostart = (int)config_get_int("WEB_AUTOSTART", 0, 0, 1);
    if (g_headless && !web_autostart) {
        /* There is no menu to start it from, so honouring WEB_AUTOSTART=0
         * here would leave a process capturing into nothing with no way
         * to reach it. */
        fprintf(stderr, "headless: WEB_AUTOSTART=0 ignored -- the web stream is the only interface\n");
        web_autostart = 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGSEGV, on_crash_signal);
    signal(SIGABRT, on_crash_signal);
    signal(SIGBUS, on_crash_signal);
    signal(SIGFPE, on_crash_signal);

    char capture_format_buf[32];
    VideoFormat capture_format = video_format_from_name(
        config_get_str("CAPTURE_FORMAT", capture_format_buf, sizeof(capture_format_buf), "yuyv"));
    g_video = video_capture_open(video_device, capture_format, &g_app.width, &g_app.height);
    if (!g_video) {
        return 1;
    }

    /* Optional gamepad support: if no ConsoleTuner adapter (Titan One...)
     * is plugged in/accessible, we just carry on without it -- this is
     * not a required feature for the rest of the app. */
    gamepad_bridge_init();

    {
        const int configured = gamepad_bridge_backend_configured_index();
        g_settings.output_backend = configured >= 0 ? configured : 0;
    }

    /*
     * Local-input shaping and final-output shaping are two deliberately
     * different calibration stages.
     *
     * Both are ordinary live settings. The .env only provides their
     * startup values; changing a slider does NOT restart the process.
     */
    g_settings.invert_ry =
        (int)config_get_int("INPUT_INVERT_RY", 0, 0, 1);

    g_settings.lt_threshold =
        (int)config_get_int("INPUT_LT_THRESHOLD", 30, 0, 100);

    g_settings.rt_threshold =
        (int)config_get_int("INPUT_RT_THRESHOLD", 30, 0, 100);

    g_settings.stick_deadzone[0] =
        (int)config_get_int("INPUT_LEFT_STICK_DEADZONE", 5, 0, 40);
    g_settings.stick_range[0] =
        (int)config_get_int("INPUT_LEFT_STICK_RANGE", 100, 45, 100);
    g_settings.stick_diagonal[0] =
        (int)config_get_int("INPUT_LEFT_STICK_DIAGONAL", 100, 45, 100);

    g_settings.stick_deadzone[1] =
        (int)config_get_int("INPUT_RIGHT_STICK_DEADZONE", 5, 0, 40);
    g_settings.stick_range[1] =
        (int)config_get_int("INPUT_RIGHT_STICK_RANGE", 100, 45, 100);
    g_settings.stick_diagonal[1] =
        (int)config_get_int("INPUT_RIGHT_STICK_DIAGONAL", 100, 45, 100);

    g_settings.output_invert_ry =
        (int)config_get_int("OUTPUT_INVERT_RY", 0, 0, 1);

    g_settings.output_lt_threshold =
        (int)config_get_int("OUTPUT_LT_THRESHOLD", 0, 0, 100);

    g_settings.output_rt_threshold =
        (int)config_get_int("OUTPUT_RT_THRESHOLD", 0, 0, 100);

    g_settings.output_stick_deadzone[0] =
        (int)config_get_int("OUTPUT_LEFT_STICK_DEADZONE", 0, 0, 40);
    g_settings.output_stick_range[0] =
        (int)config_get_int("OUTPUT_LEFT_STICK_RANGE", 100, 45, 100);
    g_settings.output_stick_diagonal[0] =
        (int)config_get_int("OUTPUT_LEFT_STICK_DIAGONAL", 100, 45, 100);

    g_settings.output_stick_deadzone[1] =
        (int)config_get_int("OUTPUT_RIGHT_STICK_DEADZONE", 0, 0, 40);
    g_settings.output_stick_range[1] =
        (int)config_get_int("OUTPUT_RIGHT_STICK_RANGE", 100, 45, 100);
    g_settings.output_stick_diagonal[1] =
        (int)config_get_int("OUTPUT_RIGHT_STICK_DIAGONAL", 100, 45, 100);

    {
        ControllerShaping shaping = {
            .invert_ry = g_settings.output_invert_ry,
            .lt_threshold = g_settings.output_lt_threshold,
            .rt_threshold = g_settings.output_rt_threshold,
            .stick_deadzone = {
                g_settings.output_stick_deadzone[0],
                g_settings.output_stick_deadzone[1],
            },
            .stick_range = {
                g_settings.output_stick_range[0],
                g_settings.output_stick_range[1],
            },
            .stick_diagonal = {
                g_settings.output_stick_diagonal[0],
                g_settings.output_stick_diagonal[1],
            },
        };

        gamepad_bridge_set_output_shaping(&shaping);
    }

    /* What the adapter should pretend to be to the console. Asked for
     * once here; the bridge reads what the device actually holds and
     * only writes when it differs, so a matching value costs nothing.
     * An empty TITAN_OUTPUT_PROTOCOL leaves the adapter alone. */
    {
        char buf[32];
        const char *want = config_get_str("TITAN_OUTPUT_PROTOCOL", buf, sizeof(buf), "");
        int value = gamepad_protocol_from_name(want);
        if (*want && value < 0) {
            fprintf(stderr, "TITAN_OUTPUT_PROTOCOL: \"%s\" is not a known protocol, ignored\n", want);
        }
        g_settings.output_protocol = value;
        if (value >= 0) {
            gamepad_bridge_request_output_protocol(value);
        }
    }

    g_gst = gst_webrtc_stream_create((int)g_app.width, (int)g_app.height, WEB_STREAM_AUDIO_RATE,
                                      WEB_STREAM_AUDIO_CHANNELS);
    if (!g_gst) {
        fprintf(stderr, "gst_webrtc_stream_create: failed\n");
        video_capture_close(g_video);
        return 1;
    }

    g_web = web_stream_create(g_gst);
    if (!g_web) {
        fprintf(stderr, "web_stream_create: failed\n");
        gst_webrtc_stream_destroy(g_gst);
        video_capture_close(g_video);
        return 1;
    }

    /* Headless asks for no subsystem that needs a display: SDL is still
     * initialised because the audio capture thread uses its mutexes and
     * condition variables. */
    Uint32 sdl_flags = g_headless ? SDL_INIT_EVENTS : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    /* The native transport for non-browser clients (the Switch
     * homebrew). Listening costs nothing while nobody connects, and
     * failing to bind is not fatal -- the browser path is unaffected. */
    g_settings.switch_port =
        (int)config_get_int("SWITCH_PORT", C2S_DEFAULT_PORT, 1, 65535);
    g_settings.switch_enabled = (int)config_get_int("SWITCH_AUTOSTART", 1, 0, 1);
    /* Off unless asked for: it needs radio hardware most machines do
     * not have, and starting it without that only produces an error. */
    g_settings.wiiu_pad_enabled = (int)config_get_int("WIIU_PAD_AUTOSTART", 0, 0, 1);
    g_settings.wiiu_pad_bitrate_mbps =
        (int)config_get_int("WIIU_PAD_BITRATE_MBPS", 6, 2, 20);
    /* Off unless asked for, like the pad's, and for a milder reason: a
     * chain nobody is on costs nothing, but a machine that has never
     * seen a Wii U should not be listening on its behalf. */
    g_settings.wiiu_console_enabled = (int)config_get_int("WIIU_CONSOLE_ENABLED", 0, 0, 1);
    g_settings.wiiu_console_height = (int)config_get_int("WIIU_CONSOLE_HEIGHT", 720, 480, 1080);
    g_settings.wiiu_console_bitrate_mbps =
        (int)config_get_int("WIIU_CONSOLE_BITRATE_MBPS", 8, 2, 30);
    web_stream_set_native_counter(g_web, count_native_clients, NULL);
    web_stream_set_native_adopt(g_web, adopt_web_socket, NULL);
    g_switch = g_settings.switch_enabled
                   ? switch_stream_start(g_web, (uint16_t)g_settings.switch_port)
                   : NULL;
    gst_webrtc_stream_set_switch_output(g_gst, g_switch);
    /* The console client's chain, from what was remembered. Applied at
     * startup and not only on a change, or a host restarted with the
     * box ticked would serve nobody. */
    gst_webrtc_stream_set_wiiu_enabled(g_gst, g_settings.wiiu_console_enabled);
    gst_webrtc_stream_set_wiiu_profile(g_gst, g_settings.wiiu_console_height,
                                       g_settings.wiiu_console_bitrate_mbps * 1000);
    gst_webrtc_stream_set_drc_bitrate(g_gst, g_settings.wiiu_pad_bitrate_mbps * 1000);
    /*
     * Before the transport can be asked for that stream, and at startup
     * rather than only when the setting is changed.
     *
     * Applying it only on change meant a host that had the setting
     * saved came up with the encode switched off: the pad connected,
     * was refused the codec it exists for, and fell back to being sent
     * ordinary H.264 to decode and encode again -- the slow path, for a
     * feature that was turned on.
     *
     * After set_switch_output, so the answer reaches the transport that
     * has to offer or refuse it.
     */
    gst_webrtc_stream_set_drc_enabled(g_gst, g_settings.wiiu_pad_enabled);
    if (g_settings.wiiu_pad_enabled && g_switch) {
        g_wiiu_pad = wiiu_pad_start(g_project_dir, C2S_DRC_PORT);
        fprintf(stderr, "wiiu_pad: %s\n", wiiu_pad_status(g_wiiu_pad));
    }

    if (SDL_Init(sdl_flags) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        web_stream_destroy(g_web);
        gst_webrtc_stream_destroy(g_gst);
        video_capture_close(g_video);
        return 1;
    }

    SDL_Texture *texture = NULL;
    /* What `texture` was created for. The capture format can change while
     * running, and a texture built for one layout fed the other renders
     * as garbage -- a solid green window, in the case of RGB24 bytes
     * uploaded as YUY2. */
    /* What the current texture holds; -1 until the first frame says. */
    int texture_pixel = -1;

    /* The tray and the local controller in BOTH modes.
     *
     * They lived inside the windowed branch, which is the wrong place
     * for either: headless means no video window, not no desk. Someone
     * running it that way still wants somewhere to change the bitrate
     * and something to quit with, and a controller plugged into this
     * machine is just as plugged in. The GTK thread checks for a display
     * itself and stands down quietly when there is none, which is what
     * happens over ssh. */
    local_pad_init();

    /*
     * Remember the physical controller, not its transient SDL slot.
     * The slot is resolved again whenever SDL's device list changes.
     */
    g_settings.gamepad_enabled =
        g_headless
            ? 0
            : (int)config_get_int(
                  "LOCAL_GAMEPAD_ENABLED",
                  1,
                  0,
                  1);

    {
        char saved_guid[64];

        config_get_str(
            "LOCAL_GAMEPAD_GUID",
            saved_guid,
            sizeof(saved_guid),
            "none");

        g_settings.gamepad_index =
            local_pad_find_guid(saved_guid);
    }

    GtkShellCallbacks shell_callbacks = {
        .on_settings = on_settings,
        .on_action = on_action,
        .on_pair = on_pair,
        .userdata = NULL,
    };
    g_settings.web_port = g_web_port;
    g_settings.capture_mjpeg = (video_capture_format(g_video) == VIDEO_FORMAT_MJPEG);
    g_shell = gtk_shell_start(&g_settings, &shell_callbacks);

    if (g_shell) {
        publish_server_status();
    }

    if (!g_headless && open_capture_window() != 0) {
        SDL_Quit();
        gtk_shell_stop(g_shell);
        web_stream_destroy(g_web);
        gst_webrtc_stream_destroy(g_gst);
        video_capture_close(g_video);
        return 1;
    }
    if (!g_headless && web_autostart) {
        start_or_report_web_stream();
    }
    /* The texture is built on the first frame, once what the capture
     * actually produces is known. Asking the card was never quite the
     * same question: MJPEG arrives as planes whose subsampling comes
     * from the JPEG, not from the format that was requested. */

    if (g_headless && web_autostart) {
        /* Windowed mode starts it earlier, alongside the control bar. */
        start_or_report_web_stream();
    }

    char audio_source_buf[512];
    const char *audio_source =
        config_get_str("AUDIO_SOURCE", audio_source_buf, sizeof(audio_source_buf), DEFAULT_AUDIO_SOURCE);
    /* No speakers in headless mode: the stream is unaffected, since the
     * local output only ever fed a monitor for whoever is sitting
     * here. */
    AudioCapture *audio =
        audio_capture_start(audio_source, &g_app.running, g_web, g_gst, !g_headless);
    g_audio = audio;
    if (!audio) {
        fprintf(stderr, "audio capture unavailable, continuing without sound\n");
    }

    /* Drives the wait-for-the-card-to-come-back path below. */
    Uint32 video_last_open_try = 0;
    /* Detects hitches: tells a stall at the capture card apart from one
     * further down the encode/network path, which look identical to a
     * viewer. */
    Uint32 last_frame_ms = 0;
    Uint32 last_push_ms = 0;
    /* Edge-logged, so the log says when the encoder went idle rather
     * than repeating it sixty times a second. */
    int browser_fed = -1, native_fed = -1;

    while (g_app.running) {
        /* A controller here drives the console, on the same terms as a
         * browser or the console client: one more source into the merge,
         * so several hands combine rather than fight. Polled in both
         * modes -- headless turns it off through the settings, not by
         * never looking. */
        local_pad_poll(&g_settings);

        /* The list the settings window offers. Cheap, and SDL's joystick
         * calls belong on the thread that initialised it. */
        {
            static Uint32 last_scan = 0;
            const Uint32 now = SDL_GetTicks();
            if (now - last_scan > 2000) {
                last_scan = now;
                const char *names[8];
                const int n = local_pad_list(names, 8);

                /*
                 * A replug can renumber SDL joystick indices. Resolve the
                 * saved GUID every time the device list is refreshed so
                 * the preferred controller follows the hardware.
                 */
                char saved_guid[64];

                config_get_str(
                    "LOCAL_GAMEPAD_GUID",
                    saved_guid,
                    sizeof(saved_guid),
                    "none");

                const int preferred =
                    local_pad_find_guid(saved_guid);

                if (preferred != g_settings.gamepad_index) {
                    g_settings.gamepad_index =
                        preferred;

                    gtk_shell_update(
                        g_shell,
                        &g_settings);
                }

                gtk_shell_set_controllers(g_shell, names, n);
            }
        }

        /* The settings window follows what the clients did.
         *
         * The resolution, the bitrate and the capture format are shared
         * by everyone, and a browser or a console client can change any
         * of them -- at which point this window was still showing what
         * it last set, with no hint that it had been overruled. Read
         * back from the things themselves rather than from whatever
         * asked last, so a format the driver refused shows as refused.
         *
         * Once a second, and only when something actually differs:
         * load_controls() rebuilds every widget, and doing that while
         * somebody is dragging a slider would fight them. */
        {
            static Uint32 last_reconcile = 0;
            const Uint32 now = SDL_GetTicks();
            if (now - last_reconcile > 1000) {
                last_reconcile = now;
                int w = 0, h = 0;
                gst_webrtc_stream_get_browser_resolution(g_gst, &w, &h);
                const int bitrate = gst_webrtc_stream_get_video_bitrate(g_gst) / 1000;
                const int mjpeg = (video_capture_active_format() == VIDEO_FORMAT_MJPEG);
                if ((h > 0 && h != g_settings.browser_height)
                    || (bitrate > 0 && bitrate != g_settings.bitrate_mbps)
                    || mjpeg != g_settings.capture_mjpeg) {
                    if (h > 0) g_settings.browser_height = h;
                    if (bitrate > 0) g_settings.bitrate_mbps = bitrate;
                    g_settings.capture_mjpeg = mjpeg;
                    gtk_shell_update(g_shell, &g_settings);
                }
            }
        }

        if (app_restart_requested()) {
            /* Out through the ordinary shutdown, which knows the order
             * that matters -- the adapter first, then the sockets and
             * the device. Only once all of that is released does the
             * program replace its own image, at the bottom of main. */
            fprintf(stderr, "restart: shutting down to start again\n");
            g_app.running = 0;
            break;
        }

        if (g_show_window_requested) {
            g_show_window_requested = 0;
            open_capture_window();
        }

        /* Reaps it if it has gone, and takes in what it said. Cheap
         * enough to do every pass, and it is how a client that stopped
         * on its own stops claiming to be running. */
        wiiu_pad_poll(g_wiiu_pad);
        process_wiiu_pad_request();

        /*
         * NOT torn down when the client exits.
         *
         * Exiting is how a session ends now: the bridge waits for a
         * pad, serves it, and stops when the pad goes. Freeing the
         * supervisor here -- and switching the setting off with it --
         * meant that turning a pad off turned the whole feature off,
         * and the only way back was to tick the box again. The
         * supervisor starts the next session itself.
         */
        {
            static Uint32 pad_status_at = 0;
            const Uint32 now_pad = SDL_GetTicks();
            if (g_shell && now_pad - pad_status_at >= 1000) {
                pad_status_at = now_pad;
                gtk_shell_set_wiiu_status(g_shell,
                    g_wiiu_pad ? wiiu_pad_status(g_wiiu_pad) : "not running");
                publish_client_status();
                publish_server_status();
            }
        }

        if (g_window) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    /* The window's own close button. The program keeps
                     * running in the tray -- closing a monitor is not
                     * quitting a capture that other people are
                     * watching -- and the speakers here go with the
                     * picture, since a closed window that is still
                     * making noise is a program you cannot find.
                     *
                     * Minimising does not: that is for getting it out of
                     * the way while still listening. */
                    SDL_HideWindow(g_window);
                    audio_capture_set_local_output(g_audio, 0);
                } else if ((event.type == SDL_MOUSEBUTTONDOWN && event.button.clicks == 2) ||
                           (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F11)) {
                    const Uint32 flags = SDL_GetWindowFlags(g_window);
                    SDL_SetWindowFullscreen(
                        g_window, (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0
                                                                       : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
            }

        } /* g_window */

        /* A format switch asked for from the page. Handled here because
         * this loop owns the mapped buffers: closing the device from the
         * HTTP thread would pull them out from under a frame in flight.
         * Reuses the reopen path below by simply dropping the device. */
        VideoFormat wanted;
        if (video_capture_take_format_request(&wanted) && g_video &&
            video_capture_format(g_video) != wanted) {
            fprintf(stderr, "video_capture: switching to %s\n", video_format_name(wanted));
            capture_format = wanted;
            video_capture_close(g_video);
            g_video = NULL;
            video_last_open_try = 0; /* reopen immediately, not in a second */
        }

        /* The capture card can drop off the USB bus mid-session -- it
         * was observed doing exactly that, with VIDIOC_QBUF returning
         * "No such device". That used to end the program: the loop broke,
         * the web server went with it, and every viewer was left looking
         * at a frozen last frame with no explanation. Wait for it to come
         * back instead, keeping the stream and the gamepad bridge up.
         *
         * access() first, so a missing device costs one cheap syscall
         * rather than an open() that logs a failure every second. */
        if (!g_video) {
            SDL_Delay(VIDEO_WAIT_TICK_MS);
            Uint32 now = SDL_GetTicks();
            if (now - video_last_open_try < VIDEO_REOPEN_RETRY_MS) {
                continue;
            }
            video_last_open_try = now;
            if (access(video_device, F_OK) != 0) {
                continue;
            }
            unsigned int w = 0, h = 0;
            g_video = video_capture_open(video_device, capture_format, &w, &h);
            if (!g_video) {
                continue;
            }
            if (w != g_app.width || h != g_app.height) {
                /* The encoder, and every client that already negotiated
                 * with it, were built for the original size; a different
                 * one cannot be fed into the running pipeline. */
                fprintf(stderr,
                        "video_capture: came back as %ux%u but the stream was built for %ux%u -- "
                        "restart to pick up the new size\n",
                        w, h, g_app.width, g_app.height);
                video_capture_close(g_video);
                g_video = NULL;
                continue;
            }
            fprintf(stderr, "video_capture: device is back\n");

        }

        struct pollfd pfd;
        pfd.fd = video_capture_fd(g_video);
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            video_capture_lost();
            continue;
        }
        if (pr == 0) {
            continue;
        }

        VideoFrame frame;
        memset(&frame, 0, sizeof(frame));
        int got = video_capture_read(g_video, &frame);
        if (got < 0) {
            video_capture_lost();
            continue;
        }
        if (got == 1) {
            /* Two numbers, because a viewer cannot tell these apart but
             * they have opposite causes: `gap` is how long we waited for
             * this frame, `push` is how long the PREVIOUS one took to
             * hand to the encoder. A large gap with a small push means
             * the card stopped delivering; a large gap that matches the
             * previous push means we were busy downstream and never
             * asked. */
            Uint32 frame_ms = SDL_GetTicks();

            if (app_verbose() && last_frame_ms != 0 && frame_ms - last_frame_ms > FRAME_GAP_WARN_MS) {
                fprintf(stderr, "video_capture: %u ms gap before this frame (previous push took %u ms)\n",
                        (unsigned)(frame_ms - last_frame_ms), (unsigned)last_push_ms);
            }
            last_frame_ms = frame_ms;

            /* Encoder FIRST, local window second.
             *
             * SDL_RenderPresent() blocks until the display's next vsync
             * -- up to a full refresh period. Doing it before this push
             * meant every frame sat waiting on the local monitor before
             * it even reached the encoder, adding that delay to what the
             * remote viewer sees. The person playing over the network is
             * the one who cares about latency; the local window is a
             * monitor and can be one frame behind. */
            /* Nothing is pushed when nobody is watching.
             *
             * The encoder is downstream of the appsrc, so starving the
             * appsrc stops it dead -- no frames in, no work done. The
             * server used to encode 1080p continuously from the moment
             * the stream was enabled, whether or not a single client had
             * ever connected, which is most of this process's CPU spent
             * on output nobody receives.
             *
             * Restarting is free: appsrc timestamps on arrival, so a gap
             * is just a gap and the encoder emits a keyframe for the
             * client that turned up. */
            int watchers = web_stream_is_running(g_web)
                               ? gst_webrtc_stream_get_client_count(g_gst, NULL)
                               : 0;
            int native = switch_stream_client_count(g_switch);

            /* Reported separately, because they are separate encoders
             * and one being fed says nothing about the other. A single
             * line covering both meant "the native client gets nothing"
             * and "nobody is connected" looked identical in the log. */
            if ((watchers > 0) != browser_fed) {
                browser_fed = (watchers > 0);
                fprintf(stderr, "encoder: browser %s (%d watching)\n",
                        browser_fed ? "feeding" : "idle", watchers);
            }
            if ((native > 0) != native_fed) {
                native_fed = (native > 0);
                fprintf(stderr, "encoder: native %s (%d connected)\n",
                        native_fed ? "feeding" : "idle", native);
            }

            const int av_format = av_format_of(frame.pixel);
            if (watchers > 0) {
                gst_webrtc_stream_push_video(g_gst, frame.plane, frame.stride, av_format,
                                             (int)g_app.width, (int)g_app.height);
            }

            /* The 720p encode is gated separately: its appsrc is simply
             * not fed while no native client is connected, so that second
             * encoder sits at zero rather than producing a stream for
             * nobody. */
            if (native > 0) {
                gst_webrtc_stream_push_video_switch(g_gst, frame.plane, frame.stride, av_format,
                                                    (int)g_app.width, (int)g_app.height);
            }
            last_push_ms = SDL_GetTicks() - frame_ms;

            /* The console has started drawing after a wake: now is when
             * the adapter can usefully re-handshake with it. */
            if (video_capture_take_change_detected()) {
                fprintf(stderr, "wake: picture is back, re-enumerating the adapter\n");
                gamepad_bridge_reset();
            }

            if (g_renderer) {
                show_frame(g_renderer, &texture, &texture_pixel, &frame, (int)g_app.width,
                           (int)g_app.height);
            }
        }
    }

    g_app.running = 0;

    /* The adapter goes FIRST.
     *
     * A full shutdown takes about 2 s, and the launcher escalates to
     * SIGKILL at exactly 2 s -- so with this call at the end, the
     * LEAVE_CAPTURE report that hands the adapter back was routinely
     * never sent. It then stayed in capture mode across a relaunch,
     * adding input latency that only a physical unplug cleared. Nothing
     * else here is time-sensitive: the pipeline and the sockets are this
     * process's own, while the adapter is a device left in a state for
     * whatever runs next. */
    local_pad_shutdown();
    gamepad_bridge_shutdown();

    /* Before the transport it is connected to, so its last frames go
     * somewhere rather than into a socket that has just closed -- and
     * so the pad is given back rather than left associated to nothing. */
    wiiu_pad_stop(g_wiiu_pad);
    g_wiiu_pad = NULL;

    switch_stream_stop(g_switch);
    g_switch = NULL;

    audio_capture_stop(audio);
    g_audio = NULL;
    web_stream_destroy(g_web);
    gst_webrtc_stream_destroy(g_gst);

    if (texture) SDL_DestroyTexture(texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
    if (g_shell) {
        gtk_shell_stop(g_shell);
    }
    video_capture_close(g_video);

    if (app_restart_requested()) {
        /* Replacing the image rather than spawning and exiting: the pid
         * does not change, so the launcher's pid file stays true, and
         * stdout and stderr still point at the same log. Everything this
         * process held has just been closed above, so the new image
         * finds the capture card, the adapter and the ports free.
         *
         * If it fails there is nothing sensible left to do -- the
         * program has already given everything back -- so it says so and
         * stops, which the launcher reports as an ordinary exit. */
        fprintf(stderr, "restart: starting again\n");
        fflush(stderr);

        /* argv[0] rather than /proc/self/exe, and that is not a detail.
         *
         * Executing /proc/self/exe works, but the kernel names the new
         * process after the path it was given -- so the program came
         * back called "exe", and the launcher, which finds it by name,
         * could no longer see it. It then reported nothing running while
         * the capture card was still held, and the next start failed
         * with "device is busy".
         *
         * The launcher always invokes this with a full path, so argv[0]
         * is the real one and the name survives. /proc/self/exe remains
         * as the fallback for an invocation that came from somewhere
         * else -- being alive under the wrong name beats not coming
         * back. */
        if (argv[0] && strchr(argv[0], '/')) {
            execv(argv[0], argv);
        }
        execv("/proc/self/exe", argv);
        fprintf(stderr, "restart: could not start again (%s)\n", strerror(errno));
        return 1;
    }
    return 0;
}
