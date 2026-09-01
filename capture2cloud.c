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
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
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

    g_renderer = SDL_CreateRenderer(g_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        /* Falling back without vsync means the local window WILL tear --
         * worth saying out loud rather than leaving it to be discovered
         * by looking at the picture. */
        fprintf(stderr, "SDL: vsync unavailable (%s), falling back -- the local window may tear\n",
                SDL_GetError());
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    }
    if (!g_renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        return -1;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(g_renderer, &info) == 0) {
        fprintf(stderr, "SDL: renderer '%s', vsync %s\n", info.name,
                (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "ON" : "OFF");
    }
    return 0;
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
    }
    if (want->local_muted != have->local_muted || want->local_volume != have->local_volume) {
        have->local_muted = want->local_muted;
        have->local_volume = want->local_volume;
        audio_capture_set_local_output(g_audio, !want->local_muted, want->local_volume);
    }

    /* The rest are read where they are used -- the controller poll, the
     * drawing -- so storing them is applying them. */
    have->gamepad_enabled = want->gamepad_enabled;
    have->gamepad_index = want->gamepad_index;
    have->invert_ry = want->invert_ry;
    have->lt_threshold = want->lt_threshold;
    have->rt_threshold = want->rt_threshold;
    for (int i = 0; i < 2; i++) {
        have->stick_deadzone[i] = want->stick_deadzone[i];
        have->stick_range[i] = want->stick_range[i];
        have->stick_diagonal[i] = want->stick_diagonal[i];
    }
    have->brightness = want->brightness;
    have->contrast = want->contrast;
    have->vsync = want->vsync;
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
        case GTK_SHELL_ACTION_RESET_DONGLE:
            gamepad_bridge_reset();
            break;
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

int main(int argc, char **argv) {
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
    web_stream_set_native_counter(g_web, count_native_clients, NULL);
    g_switch = g_settings.switch_enabled
                   ? switch_stream_start(g_web, (uint16_t)g_settings.switch_port)
                   : NULL;
    gst_webrtc_stream_set_switch_output(g_gst, g_switch);

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

    GtkShellCallbacks shell_callbacks = {
        .on_settings = on_settings,
        .on_action = on_action,
        .userdata = NULL,
    };
    g_settings.web_port = g_web_port;
    g_settings.capture_mjpeg = (video_capture_format(g_video) == VIDEO_FORMAT_MJPEG);
    g_settings.gamepad_enabled = !g_headless;
    g_shell = gtk_shell_start(&g_settings, &shell_callbacks);


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
                gtk_shell_set_controllers(g_shell, names, n);
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

        if (g_window) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    /* The window's own close button. The program keeps
                     * running in the tray -- closing a monitor is not
                     * quitting a capture that other people are
                     * watching. */
                    SDL_HideWindow(g_window);
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
