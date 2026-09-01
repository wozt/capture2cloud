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
#include "gtk_shell.h"
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
        if (g_shell) {
            gtk_shell_show_error(g_shell, err);
            gtk_shell_set_stream_status(g_shell, 0, g_web_port);
        }
    } else {
        fprintf(stderr, "web stream: listening on port %d\n", g_web_port);
        if (g_shell) {
            gtk_shell_set_stream_status(g_shell, 1, g_web_port);
        }
    }
}

static void on_menu_toggle_stream(void *userdata, int enable) {
    (void)userdata;
    if (enable) {
        start_or_report_web_stream();
    } else {
        web_stream_stop(g_web);
        if (g_shell) {
            gtk_shell_set_stream_status(g_shell, 0, g_web_port);
        }
    }
}

static void on_menu_set_port(void *userdata, int port) {
    (void)userdata;
    g_web_port = port;
    if (web_stream_is_running(g_web)) {
        web_stream_stop(g_web);
        start_or_report_web_stream();
    }
}

static void on_menu_quit(void *userdata) {
    (void)userdata;
    g_app.running = 0;
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

static void sync_dock(GtkShell *shell, SDL_Window *window) {
    int x, y, w, h;
    SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &w, &h);
    gtk_shell_dock_above(shell, x, y, w, h);
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
    g_switch = switch_stream_start(g_web, (uint16_t)config_get_int("SWITCH_PORT", 0, 0, 65535));
    gst_webrtc_stream_set_switch_output(g_gst, g_switch);
    gst_webrtc_stream_set_switch_output(g_gst, g_switch);

    if (SDL_Init(sdl_flags) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        web_stream_destroy(g_web);
        gst_webrtc_stream_destroy(g_gst);
        video_capture_close(g_video);
        return 1;
    }

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    /* What `texture` was created for. The capture format can change while
     * running, and a texture built for one layout fed the other renders
     * as garbage -- a solid green window, in the case of RGB24 bytes
     * uploaded as YUY2. */
    /* What the current texture holds; -1 until the first frame says. */
    int texture_pixel = -1;

    if (!g_headless) {
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

    /* Borderless: the GTK control window docks right above it (see
     * gtk_shell_dock_above below), giving the illusion of a single window
     * with its own menu bar. Under KWin, Alt+drag (move) and
     * Alt+right-click-drag (resize) still work without a border. */
    window = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          (int)g_app.width, (int)g_app.height,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        web_stream_destroy(g_web);
        gst_webrtc_stream_destroy(g_gst);
        video_capture_close(g_video);
        return 1;
    }

    unsigned long video_xid = 0;
    SDL_SysWMinfo wminfo;
    SDL_VERSION(&wminfo.version);
    if (SDL_GetWindowWMInfo(window, &wminfo) && wminfo.subsystem == SDL_SYSWM_X11) {
        video_xid = (unsigned long)wminfo.info.x11.window;
    }

    GtkShellCallbacks shell_callbacks = {
        .on_toggle_stream = on_menu_toggle_stream,
        .on_set_port = on_menu_set_port,
        .on_quit = on_menu_quit,
        .userdata = NULL,
    };
    g_shell = gtk_shell_start(g_web_port, video_xid, &shell_callbacks);
    if (!g_shell) {
        fprintf(stderr, "gtk_shell_start: failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        web_stream_destroy(g_web);
        gst_webrtc_stream_destroy(g_gst);
        video_capture_close(g_video);
        return 1;
    }

    if (web_autostart) {
        start_or_report_web_stream();
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        /* Falling back without vsync means the local window WILL tear --
         * worth saying out loud rather than leaving it to be discovered
         * by looking at the picture. */
        fprintf(stderr, "SDL: vsync unavailable (%s), falling back -- the local window may tear\n",
                SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    }
    if (renderer) {
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(renderer, &info) == 0) {
            fprintf(stderr, "SDL: renderer '%s', vsync %s\n", info.name,
                    (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "ON" : "OFF");
        }
    }
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        gtk_shell_stop(g_shell);
        web_stream_destroy(g_web);
        gst_webrtc_stream_destroy(g_gst);
        video_capture_close(g_video);
        return 1;
    }

    /* The texture is built on the first frame instead, once what the
     * capture actually produces is known. Asking the card was never
     * quite the same question: MJPEG now arrives as planes whose
     * subsampling comes from the JPEG, not from the format that was
     * requested. */
    texture = NULL;
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        gtk_shell_stop(g_shell);
        web_stream_destroy(g_web);
        gst_webrtc_stream_destroy(g_gst);
        video_capture_close(g_video);
        return 1;
    }
    } /* !g_headless */

    if (g_headless && web_autostart) {
        /* Windowed mode starts it earlier, alongside the control bar. */
        start_or_report_web_stream();
    }

    char audio_source_buf[512];
    const char *audio_source =
        config_get_str("AUDIO_SOURCE", audio_source_buf, sizeof(audio_source_buf), DEFAULT_AUDIO_SOURCE);
    AudioCapture *audio = audio_capture_start(audio_source, &g_app.running, g_web, g_gst);
    if (!audio) {
        fprintf(stderr, "audio capture unavailable, continuing without sound\n");
    }

    if (!g_headless) {
        sync_dock(g_shell, window);
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
        if (app_restart_requested()) {
            /* Out through the ordinary shutdown, which knows the order
             * that matters -- the adapter first, then the sockets and
             * the device. Only once all of that is released does the
             * program replace its own image, at the bottom of main. */
            fprintf(stderr, "restart: shutting down to start again\n");
            g_app.running = 0;
            break;
        }

        if (!g_headless) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                g_app.running = 0;
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.clicks == 2) {
                Uint32 flags = SDL_GetWindowFlags(window);
                Uint32 fullscreen = flags & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                if (fullscreen) {
                    sync_dock(g_shell, window);
                }
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F11) {
                Uint32 flags = SDL_GetWindowFlags(window);
                Uint32 fullscreen = flags & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                if (fullscreen) {
                    sync_dock(g_shell, window);
                }
            } else if (event.type == SDL_WINDOWEVENT &&
                       (event.window.event == SDL_WINDOWEVENT_MOVED ||
                        event.window.event == SDL_WINDOWEVENT_RESIZED ||
                        event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                Uint32 flags = SDL_GetWindowFlags(window);
                if (!(flags & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
                    sync_dock(g_shell, window);
                }
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                /* "Always on top" only while the video has focus: avoids
                 * staying stuck on top of another app once we've switched
                 * away. */
                gtk_shell_set_keep_above(g_shell, 1);
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                gtk_shell_set_keep_above(g_shell, 0);
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                gtk_shell_set_keep_above(g_shell, 0);
                gtk_shell_set_iconified(g_shell, 1);
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESTORED) {
                gtk_shell_set_iconified(g_shell, 0);
                sync_dock(g_shell, window);
            }
        }

        /* The control window may have requested a layout change (a direct
         * move by the user, or toggling pseudo-fullscreen via the
         * maximize button): apply it. */
        int layout_x, layout_y, layout_w, layout_h;
        if (gtk_shell_poll_layout(g_shell, &layout_x, &layout_y, &layout_w, &layout_h)) {
            SDL_SetWindowPosition(window, layout_x, layout_y);
            if (layout_w > 0 && layout_h > 0) {
                SDL_SetWindowSize(window, layout_w, layout_h);
            }
        }

        int iconify_request;
        if (gtk_shell_poll_iconify(g_shell, &iconify_request)) {
            if (iconify_request) {
                SDL_MinimizeWindow(window);
            } else {
                SDL_RestoreWindow(window);
            }
        }
        } /* !g_headless */

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

            if (!g_headless && renderer) {
                show_frame(renderer, &texture, &texture_pixel, &frame, (int)g_app.width,
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
    gamepad_bridge_shutdown();

    switch_stream_stop(g_switch);
    g_switch = NULL;

    audio_capture_stop(audio);
    web_stream_destroy(g_web);
    gst_webrtc_stream_destroy(g_gst);

    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
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
        execv("/proc/self/exe", argv);
        fprintf(stderr, "restart: could not start again (%s)\n", strerror(errno));
        return 1;
    }
    return 0;
}
