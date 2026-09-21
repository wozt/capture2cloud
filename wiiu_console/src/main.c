/*
 * capture2cloud on a Wii U console.
 *
 * Connects to the host, decodes with the console's hardware H.264, and
 * draws on the television. Sound, controller input and the password are
 * steps three to five in SPEC.md.
 *
 * Rebuilt on SDL2 after four probes settled the one thing that decides
 * the shape of this program: OSScreen and GX2 cannot both be used. Once
 * OSScreen has been up, GX2 renders nothing at all -- WHBGfxInit still
 * returns 1, frames still count, and the screen stays black. Everything
 * that matters here is GX2 (the console's keyboard, the HOME overlay,
 * and an NV12 video texture), so the program is GX2 from its first
 * line. SDL2's Wii U renderer is GX2, and it brings text and touch
 * without a shader assembler, which this toolchain does not have.
 *
 * Nothing touches the network until CONNECT is pressed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <whb/log.h>
#include <whb/log_udp.h>

#include "c2s_protocol.h"
#include "gx2_video.h"
#include "audio.h"
#include "input.h"
#include "keyboard.h"
#include "net.h"
#include "proc.h"
#include "settings.h"
#include "ui.h"
#include "video.h"
#include "video_worker.h"

/* The biggest picture the decoder reserves for. 720p60 is the path this
 * is built and measured on; see SPEC.md on why 1080p is offered rather
 * than supported. */
#define MAX_WIDTH  1280
#define MAX_HEIGHT 720

typedef enum { STATE_SETTINGS, STATE_STREAMING } State;

typedef struct {
    int x, y, w, h;
} Rect;


typedef struct {
    unsigned rx_fps;
    unsigned decode_fps;
    unsigned display_fps;
    unsigned loop_fps;
    unsigned net_kbps;

    unsigned video_queue;
    unsigned video_dropped;
} StreamPerf;

static const Rect R_HOST    = { 300, 125, 520, 54 };
static const Rect R_PORT    = { 300, 195, 220, 54 };
static const Rect R_CONNECT = { 300, 270, 220, 58 };
static const Rect R_QUIT    = { 540, 270, 280, 58 };
static const Rect R_MENU    = { 1248,   8, 24, 24 };

static int hit(const Rect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void draw_button(const Rect *r, const char *label, UiColour fill)
{
    ui_box(r->x, r->y, r->w, r->h, fill, UI_DIM);
    ui_text_centred(r->x, r->y, r->w, r->h, UI_SIZE_BODY, UI_TEXT, label);
}

static void draw_settings(const Settings *s, const char *note, int decoder_ok, const char *why)
{
    char host[32];
    settings_host_string(s, host, sizeof(host));

    ui_text(300, 70, UI_SIZE_BODY, UI_TEXT, "capture2cloud");

    ui_text(180, 140, UI_SIZE_BODY, UI_DIM, "host");
    ui_box(R_HOST.x, R_HOST.y, R_HOST.w, R_HOST.h, UI_FIELD, UI_DIM);
    const int unset = (strcmp(host, "0.0.0.0") == 0);
    ui_text_centred(R_HOST.x, R_HOST.y, R_HOST.w, R_HOST.h, UI_SIZE_BODY,
                    unset ? UI_DIM : UI_TEXT, unset ? "tap to set" : host);

    ui_text(180, 210, UI_SIZE_BODY, UI_DIM, "port");
    ui_box(R_PORT.x, R_PORT.y, R_PORT.w, R_PORT.h, UI_FIELD, UI_DIM);
    {
        char p[16];
        snprintf(p, sizeof(p), "%u", s->port);
        ui_text_centred(R_PORT.x, R_PORT.y, R_PORT.w, R_PORT.h, UI_SIZE_BODY, UI_TEXT, p);
    }

    draw_button(&R_CONNECT, "CONNECT", UI_ACCENT);
    draw_button(&R_QUIT, "HOME -> EXIT", UI_PANEL);

    if (!decoder_ok) {
        ui_text(300, 340, UI_SIZE_BODY, UI_DANGER, "decoder: %s", why);
    } else if (note && note[0]) {
        ui_text(300, 340, UI_SIZE_BODY, UI_DANGER, "%s", note);
    }
}

static void draw_streaming(const Settings *s,
                           const StreamPerf *perf)
{
    (void)s;

    const NetInfo *info = net_info();

    VideoStats vs;
    Gx2VideoStats gs;

    unsigned long audio_failed = 0;
    unsigned long audio_dropped = 0;

    AudioDiag audio_diag_now;
    memset(
        &audio_diag_now,
        0,
        sizeof(audio_diag_now));

    uint32_t present_avg_us = 0;
    uint32_t present_max_us = 0;

    video_stats_ex(&vs);
    gx2_video_stats(&gs);

    audio_stats(NULL,
                &audio_failed,
                &audio_dropped);

    audio_diag(
        &audio_diag_now);

    ui_present_stats(
        &present_avg_us,
        &present_max_us);

    /*
     * Compact diagnostics: one useful fact per line.
     */
    ui_text(
        180, 385,
        UI_SIZE_BODY,
        UI_TEXT,
        "%s | %ux%u H264 | %u.%u Mb/s",
        ui_video_renderer_name(),
        info->width,
        info->height,
        perf->net_kbps / 1000,
        (perf->net_kbps % 1000) / 100);

    ui_text(
        180, 425,
        UI_SIZE_BODY,
        UI_DIM,
        "FPS  RX %u | DEC %u | SHOW %u | LOOP %u",
        perf->rx_fps,
        perf->decode_fps,
        perf->display_fps,
        perf->loop_fps);

    ui_text(
        180, 465,
        UI_SIZE_BODY,
        UI_DIM,
        "H264 %u.%u ms | bind %u.%u ms | present %u.%u ms",
        vs.execute_avg_us / 1000,
        (vs.execute_avg_us % 1000) / 100,
        gs.copy_avg_us / 1000,
        (gs.copy_avg_us % 1000) / 100,
        present_avg_us / 1000,
        (present_avg_us % 1000) / 100);

    ui_text(
        180, 505,
        UI_SIZE_BODY,
        UI_DIM,
        "VQ %u drop %u | AQ %u ms drop %lu bad %lu",
        perf->video_queue,
        perf->video_dropped,
        audio_queue_ms(),
        audio_dropped,
        audio_failed);

    ui_text(
        180, 545,
        UI_SIZE_BODY,
        UI_DIM,
        "AIN %u | AX %u | USE %u | CB %u | under %u | %s",
        audio_diag_now.input_fps,
        audio_diag_now.device_fps,
        audio_diag_now.used_fps,
        audio_diag_now.callback_frames,
        audio_diag_now.underruns,
        info->audio_udp ? "UDP" : "TCP");

    PadState21 pad;
    input_snapshot(pad);

    if (input_available()) {
        ui_text(
            180, 585,
            UI_SIZE_BODY,
            info->may_control
                ? UI_TEXT
                : UI_DANGER,
            "INPUT %s | L %+d,%+d R %+d,%+d | A%d B%d X%d Y%d",
            info->may_control
                ? "CONTROL"
                : "VIEWER",
            pad[PAD_LX],
            pad[PAD_LY],
            pad[PAD_RX],
            pad[PAD_RY],
            pad[PAD_A],
            pad[PAD_B],
            pad[PAD_X],
            pad[PAD_Y]);
    } else {
        ui_text(
            180, 585,
            UI_SIZE_BODY,
            UI_DANGER,
            "INPUT: Wii U GamePad unavailable");
    }
}

static void draw_menu_marker(int open)
{
    /*
     * Tiny persistent marker requested by the UI spec.
     *
     * It is deliberately not labelled: it must take almost no space
     * over the game picture.
     */
    ui_box(R_MENU.x, R_MENU.y,
           R_MENU.w, R_MENU.h,
           open ? UI_ACCENT : UI_PANEL,
           UI_DIM);
}

static unsigned video_worker_decoded_total(void)
{
    VideoWorkerStats stats;
    video_worker_stats(&stats);
    return stats.decoded;
}


static int parse_host(const char *text, void *target)
{
    return settings_set_host_string((Settings *)target, text);
}

static int parse_port(const char *text, void *target)
{
    const int v = atoi(text);
    if (v <= 0 || v > 65535) {
        return -1;
    }
    *(uint16_t *)target = (uint16_t)v;
    return 0;
}

/* Opens the console's keyboard for one field and puts the answer back.
 * `parse` returns 0 when the text was acceptable. */
static void edit_field(const char *hint, const char *current, char *note, size_t note_size,
                       int (*parse)(const char *, void *), void *target)
{
    char typed[64], why[96];
    const int r = keyboard_prompt(hint, current, 1, typed, sizeof(typed), why, sizeof(why));
    if (r < 0) {
        snprintf(note, note_size, "%s", why);
    } else if (r == 1) {
        if (parse(typed, target) != 0) {
            snprintf(note, note_size, "not a %s: %s", hint, typed);
        } else {
            note[0] = '\0';
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBLogUdpInit();
    WHBLogPrintf("capture2cloud: starting");
    proc_init();

    char why[128] = { 0 };
    if (ui_init(why, sizeof(why)) != 0) {
        WHBLogPrintf("capture2cloud: no interface -- %s", why);
        ui_shutdown();
        proc_shutdown();
        WHBLogUdpDeinit();
        return 1;
    }

    char input_why[96] = { 0 };

    int input_alive =
        input_init(
            input_why,
            sizeof(input_why)) == 0;

    WHBLogPrintf(
        "capture2cloud: input %s",
        input_alive
            ? "Wii U GamePad ready"
            : input_why);

    Settings settings;
    settings_load(&settings);

    char decoder_why[128] = { 0 };

    int decoder_ok =
        video_init(
            MAX_WIDTH,
            MAX_HEIGHT,
            decoder_why,
            sizeof(decoder_why)) == 0;

    int video_worker_alive = 0;

    if (decoder_ok) {
        if (video_worker_start(
                decoder_why,
                sizeof(decoder_why)) == 0) {
            video_worker_alive = 1;
        } else {
            video_exit();
            decoder_ok = 0;
        }
    }

    WHBLogPrintf(
        "capture2cloud: decoder %s",
        decoder_ok
            ? "ready + threaded"
            : decoder_why);

    char audio_why[128] = { 0 };

    /*
     * Audio is intentionally NOT opened before CONNECT.
     *
     * This also makes the settings screen a clean test: if anything is
     * audible there, it cannot come from Capture2Cloud audio.
     */
    int audio_ok = 1;

    WHBLogPrintf(
        "capture2cloud: audio deferred until CONNECT");

    int ui_alive = 1;
    int video_alive = decoder_ok ? 1 : 0;
    int audio_alive = 0;

    State state = STATE_SETTINGS;
    char note[160] = { 0 };
    UiInput in;
    memset(&in, 0, sizeof(in));

    VideoFrame frame;
    int have_frame = 0;
    int new_frame = 0;
    int menu_open = 0;
    StreamPerf perf;
    memset(&perf, 0, sizeof(perf));

    unsigned rx_count = 0;
    unsigned display_count = 0;
    unsigned loop_count = 0;

    unsigned worker_decoded_at =
        video_worker_decoded_total();

    int video_synced = 0;
    int keyframe_requested = 0;

    uint32_t fps_at = SDL_GetTicks();
    uint64_t rx_bytes_at = 0;

    while (proc_running()) {
        new_frame = 0;

        /*
         * ProcUI has asked us to release the foreground.
         *
         * THIS ORDER IS REQUIRED:
         *
         *   stop/drain application work
         *   -> H264DECEnd/H264DECClose
         *   -> SDL/GX2 shutdown
         *   -> ProcUIDrawDoneRelease
         *
         * Previously DrawDoneRelease happened first, and H264DECEnd
         * then hung permanently while returning to the Wii U menu.
         */
        if (proc_release_pending()) {
            WHBLogPrintf("suspend 1/3: disconnect network stream");

            net_disconnect();
            state = STATE_SETTINGS;
            have_frame = 0;

            WHBLogPrintf("suspend 2/4: audio begin");

            if (audio_alive) {
                audio_exit();
                audio_alive = 0;
            }

            WHBLogPrintf("suspend 2/4: audio done");

            WHBLogPrintf("suspend 3/4: H264DEC begin");

            if (video_worker_alive) {
                video_worker_stop();
                video_worker_alive = 0;
            }

            if (video_alive) {
                video_exit();
                video_alive = 0;
            }

            WHBLogPrintf("suspend 3/4: H264DEC done");
            WHBLogPrintf("suspend 4/4: SDL/GX2 begin");

            if (input_alive) {
                input_exit();
                input_alive = 0;
            }

            if (ui_alive) {
                ui_shutdown();
                ui_alive = 0;
            }

            WHBLogPrintf("suspend 4/4: SDL/GX2 done");

            /*
             * All foreground resources are now gone. This call performs
             * ProcUIDrawDoneRelease and waits while HOME owns the screen.
             */
            if (!proc_release_and_wait()) {
                break;  /* HOME -> Quitter */
            }

            /*
             * User closed HOME instead of quitting: rebuild everything
             * that had to be surrendered.
             */
            WHBLogPrintf("resume: rebuilding SDL/GX2");

            why[0] = '\0';
            if (ui_init(why, sizeof(why)) != 0) {
                WHBLogPrintf("resume: UI failed -- %s", why);
                break;
            }

            ui_alive = 1;

            input_why[0] = '\0';

            input_alive =
                input_init(
                    input_why,
                    sizeof(input_why)) == 0;

            WHBLogPrintf(
                "resume: input %s",
                input_alive
                    ? "Wii U GamePad ready"
                    : input_why);

            audio_ok = 1;
            audio_alive = 0;

            WHBLogPrintf(
                "resume: audio deferred until CONNECT");

            WHBLogPrintf("resume: rebuilding H264DEC");

            decoder_why[0] = '\0';

            decoder_ok =
                video_init(
                    MAX_WIDTH,
                    MAX_HEIGHT,
                    decoder_why,
                    sizeof(decoder_why)) == 0;

            video_alive =
                decoder_ok ? 1 : 0;

            video_worker_alive = 0;

            if (decoder_ok) {
                if (video_worker_start(
                        decoder_why,
                        sizeof(decoder_why)) == 0) {
                    video_worker_alive = 1;
                } else {
                    video_exit();
                    video_alive = 0;
                    decoder_ok = 0;
                }
            }

            WHBLogPrintf(
                "resume: decoder %s",
                decoder_ok
                    ? "ready + threaded"
                    : decoder_why);

            memset(&in, 0, sizeof(in));
            memset(&perf, 0, sizeof(perf));

            rx_count = 0;
            display_count = 0;
            loop_count = 0;

            worker_decoded_at =
                video_worker_decoded_total();

            video_synced = 0;
            keyframe_requested = 0;

            fps_at = SDL_GetTicks();
            rx_bytes_at = net_info()->rx_bytes;

            continue;
        }

        ui_poll(&in);

        /*
         * Forward physical controls only while actually playing.
         *
         * Opening the local menu immediately neutralises the remote pad
         * while we continue sampling it for the diagnostics screen.
         */
        if (input_alive) {
            input_update(
                state == STATE_STREAMING &&
                !menu_open);
        }

        if (in.quit) {
            proc_stop();
        }

        if (state == STATE_SETTINGS) {
            if (in.tapped) {
                if (hit(&R_HOST, in.touch_x, in.touch_y)) {
                    char current[32];
                    settings_host_string(&settings, current, sizeof(current));
                    edit_field("host", current, note, sizeof(note), parse_host, &settings);
                } else if (hit(&R_PORT, in.touch_x, in.touch_y)) {
                    char current[16];
                    snprintf(current, sizeof(current), "%u", settings.port);
                    edit_field("port", current, note, sizeof(note), parse_port, &settings.port);
                } else if (hit(&R_QUIT, in.touch_x, in.touch_y)) {
                    snprintf(note, sizeof(note),
                             "Press HOME, then choose Quitter");
                } else if (hit(&R_CONNECT, in.touch_x, in.touch_y)) {
                    if (!decoder_ok) {
                        snprintf(note, sizeof(note), "no decoder: %s", decoder_why);
                    } else if (settings.host[0] == 0) {
                        snprintf(note, sizeof(note), "set the host address first");
                    } else if (net_init() != 0) {
                        snprintf(note, sizeof(note), "%s", net_info()->status);
                    } else {
                        if (!audio_alive) {
                            audio_why[0] = '\0';

                            audio_ok =
                                audio_init(
                                    48000,
                                    2,
                                    audio_why,
                                    sizeof(audio_why)) == 0;

                            audio_alive =
                                audio_ok ? 1 : 0;

                            WHBLogPrintf(
                                "capture2cloud: AX audio %s",
                                audio_ok
                                    ? "ready"
                                    : audio_why);
                        }

                        char host[32], save_why[64];
                        settings_host_string(&settings, host, sizeof(host));
                        if (settings_save(&settings, save_why, sizeof(save_why)) != 0) {
                            /* Worth saying, not worth stopping for: the
                             * address still applies to this session. */
                            snprintf(note, sizeof(note), "not saved: %s", save_why);
                        }
                        /* No token: this connects as a viewer, which is
                         * the right amount of trust for a client that
                         * cannot yet ask for a password. */
                        net_connect(host, settings.port, NULL);
                        state = STATE_STREAMING;
                        menu_open = 0;

                        memset(&perf, 0, sizeof(perf));
                        rx_count = 0;
                        display_count = 0;
                        loop_count = 0;

                        worker_decoded_at =
                            video_worker_decoded_total();

                        video_synced = 0;
                        keyframe_requested = 0;

                        fps_at = SDL_GetTicks();
                        rx_bytes_at = net_info()->rx_bytes;

                        WHBLogPrintf("capture2cloud: connecting to %s:%u", host, settings.port);
                    }
                }
            }
        } else {
            /*
             * Drain low-latency PCM before processing large H264 TCP
             * frames.
             */
            {
                const uint8_t *audio_payload;
                uint32_t audio_size;

                while (net_take_audio(
                           &audio_payload,
                           &audio_size)) {

                    if (audio_alive) {
                        audio_push_pcm_s16le(
                            audio_payload,
                            audio_size);
                    }
                }
            }

            net_poll();

            /*
             * Every frame that has arrived, not just one. The decoder is
             * faster than this loop, and a queue drained one frame per
             * iteration grows without bound the moment drawing falls
             * behind. Only the newest is kept.
             */
            const uint8_t *payload;
            uint32_t size;
            uint8_t flags;
            int kind;
            while ((kind = net_take_frame(&payload, &size, &flags)) != 0) {
                if (kind == C2S_MSG_AUDIO) {
                    /*
                     * TCP PCM is only the compatibility fallback.
                     */
                    if (audio_alive &&
                        net_info()->audio_codec ==
                            C2S_CODEC_PCM_S16LE &&
                        !net_info()->audio_udp) {

                        audio_push_pcm_s16le(
                            payload,
                            size);
                    }

                    continue;
                }

                if (kind != C2S_MSG_VIDEO) {
                    continue;
                }

                rx_count++;

                /*
                 * If the bounded queue ever overflows we have skipped a
                 * predictive H.264 AU. Do not feed dependent pictures to
                 * the decoder afterwards: wait for the next IDR.
                 */
                if (!video_synced) {
                    if (!(flags & C2S_FLAG_KEYFRAME)) {
                        if (!keyframe_requested) {
                            net_send_keyframe_request();
                            keyframe_requested = 1;
                        }

                        continue;
                    }

                    video_synced = 1;
                    keyframe_requested = 0;
                }

                const int queued =
                    video_worker_submit_wait(
                        payload,
                        size,
                        12000);

                if (queued <= 0) {
                    video_synced = 0;

                    if (!keyframe_requested) {
                        net_send_keyframe_request();
                        keyframe_requested = 1;
                    }
                }
            }

            /*
             * H264DEC runs independently now. Take only the newest
             * completed picture and immediately hand its NV12 buffer to
             * GX2.
             */
            /*
             * SDL_RenderPresent() normally waits most of the remaining
             * 16.7 ms VBlank interval. If H264DEC is already working on
             * the next AU, spend at most 2 ms of that otherwise-idle
             * time here so decode and presentation do not continually
             * miss each other by a fraction of a millisecond.
             */
            if (video_worker_alive &&
                video_worker_take_wait(
                    &frame,
                    2000)) {

                have_frame = 1;
                new_frame = 1;
            }

            if (in.tapped) {
                if (hit(&R_MENU, in.touch_x, in.touch_y)) {
                    menu_open = !menu_open;
                } else if (menu_open) {
                    if (hit(&R_HOST, in.touch_x, in.touch_y)) {
                        char current[32];

                        settings_host_string(
                            &settings,
                            current,
                            sizeof(current));

                        edit_field(
                            "host",
                            current,
                            note,
                            sizeof(note),
                            parse_host,
                            &settings);

                    } else if (hit(&R_PORT,
                                   in.touch_x,
                                   in.touch_y)) {
                        char current[16];

                        snprintf(current,
                                 sizeof(current),
                                 "%u",
                                 settings.port);

                        edit_field(
                            "port",
                            current,
                            note,
                            sizeof(note),
                            parse_port,
                            &settings.port);

                    } else if (hit(&R_CONNECT,
                                   in.touch_x,
                                   in.touch_y)) {
                        char host[32];
                        char save_why[64];

                        settings_host_string(
                            &settings,
                            host,
                            sizeof(host));

                        if (settings_save(
                                &settings,
                                save_why,
                                sizeof(save_why)) != 0) {
                            snprintf(note,
                                     sizeof(note),
                                     "not saved: %s",
                                     save_why);
                        }

                        net_disconnect();
                        net_connect(
                            host,
                            settings.port,
                            NULL);

                        have_frame = 0;
                        menu_open = 0;

                        video_synced = 0;
                        keyframe_requested = 0;

                        worker_decoded_at =
                            video_worker_decoded_total();

                        WHBLogPrintf(
                            "capture2cloud: reconnecting to %s:%u",
                            host,
                            settings.port);

                    } else if (hit(&R_QUIT,
                                   in.touch_x,
                                   in.touch_y)) {
                        snprintf(note,
                                 sizeof(note),
                                 "Press HOME, then choose Quitter");
                    }
                }
            }
        }

        ui_begin();

        if (state == STATE_SETTINGS) {
            draw_settings(&settings, note, decoder_ok, decoder_why);
        } else {
            /*
             * NV12 comes directly from H264DEC's rotating framebuffers
             * and is sampled directly by the GX2 shader.
             */
            if (new_frame) {
                if (ui_video_update_nv12(frame.luma,
                                         frame.chroma,
                                         frame.stride,
                                         frame.width,
                                         frame.height) != 0) {
                    WHBLogPrintf("capture2cloud: video upload failed");
                    have_frame = 0;
                }
            }

            if (have_frame) {
                ui_video_draw();
            }

            /*
             * Settings are an overlay, not part of the permanent
             * streaming picture.
             */
            if (menu_open) {
                ui_box(120, 45, 1040, 560,
                       UI_BG, UI_DIM);

                draw_settings(&settings,
                              note,
                              decoder_ok,
                              decoder_why);

                draw_streaming(&settings, &perf);
            }

            draw_menu_marker(menu_open);
        }

        ui_present();

        if (state == STATE_STREAMING) {
            loop_count++;

            if (new_frame && have_frame) {
                display_count++;
            }

            const uint32_t now =
                SDL_GetTicks();

            const uint32_t elapsed =
                now - fps_at;

            if (elapsed >= 1000) {
                const uint64_t rx_now =
                    net_info()->rx_bytes;

                const uint64_t rx_delta =
                    rx_now >= rx_bytes_at
                        ? rx_now - rx_bytes_at
                        : 0;

                perf.rx_fps =
                    (unsigned)(
                        ((uint64_t)rx_count * 1000ull) /
                        elapsed);

                VideoWorkerStats vw;
                video_worker_stats(&vw);

                const unsigned decoded_delta =
                    vw.decoded -
                    worker_decoded_at;

                perf.decode_fps =
                    (unsigned)(
                        ((uint64_t)decoded_delta *
                         1000ull) /
                        elapsed);

                perf.video_queue =
                    vw.queue_depth;

                perf.video_dropped =
                    vw.dropped;

                worker_decoded_at =
                    vw.decoded;

                perf.display_fps =
                    (unsigned)(
                        ((uint64_t)display_count * 1000ull) /
                        elapsed);

                perf.loop_fps =
                    (unsigned)(
                        ((uint64_t)loop_count * 1000ull) /
                        elapsed);

                /*
                 * bytes * 8 / milliseconds numerically gives kbit/s.
                 */
                perf.net_kbps =
                    (unsigned)(
                        (rx_delta * 8ull) /
                        elapsed);

                rx_count = 0;
                display_count = 0;
                loop_count = 0;

                fps_at = now;
                rx_bytes_at = rx_now;
            }
        }
    }

    /*
     * Keep ProcUI alive while every resource owned by this application
     * is released. The AX safe-exit probe has almost nothing to tear
     * down; this client also owns TCP, H264DEC and SDL/GX2.
     *
     * Every boundary is logged so if the menu transition ever sticks
     * again we know the exact subsystem that did not return.
     */
    WHBLogPrintf("shutdown: final network cleanup");
    net_disconnect();
    net_exit();

    /*
     * Normally these are already gone because RELEASE_FOREGROUND
     * happened before EXITING. Keep the guards for other exit paths.
     */
    if (audio_alive) {
        WHBLogPrintf("shutdown: final audio cleanup");
        audio_exit();
        audio_alive = 0;
    }

    if (video_worker_alive) {
        WHBLogPrintf("shutdown: final video worker cleanup");
        video_worker_stop();
        video_worker_alive = 0;
    }

    if (video_alive) {
        WHBLogPrintf("shutdown: final H264DEC cleanup");
        video_exit();
        video_alive = 0;
    }

    if (input_alive) {
        WHBLogPrintf("shutdown: final input cleanup");
        input_exit();
        input_alive = 0;
    }

    if (ui_alive) {
        WHBLogPrintf("shutdown: final SDL/GX2 cleanup");
        ui_shutdown();
        ui_alive = 0;
    }

    WHBLogPrintf("shutdown: all application resources released");

    WHBLogUdpDeinit();
    proc_shutdown();

    return 0;
}
