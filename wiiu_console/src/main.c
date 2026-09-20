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
#include "audio.h"
#include "keyboard.h"
#include "net.h"
#include "proc.h"
#include "settings.h"
#include "ui.h"
#include "video.h"

/* The biggest picture the decoder reserves for. 720p60 is the path this
 * is built and measured on; see SPEC.md on why 1080p is offered rather
 * than supported. */
#define MAX_WIDTH  1280
#define MAX_HEIGHT 720

typedef enum { STATE_SETTINGS, STATE_STREAMING } State;

typedef struct {
    int x, y, w, h;
} Rect;

static const Rect R_HOST    = { 360, 200, 560, 76 };
static const Rect R_PORT    = { 360, 300, 260, 76 };
static const Rect R_CONNECT = { 360, 430, 260, 84 };
static const Rect R_QUIT    = { 660, 430, 260, 84 };
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

    ui_text(360, 90, UI_SIZE_TITLE, UI_TEXT, "capture2cloud");
    ui_text(360, 150, UI_SIZE_BODY, UI_DIM, "tap a field to type on the console keyboard");

    ui_text(180, 218, UI_SIZE_BODY, UI_DIM, "host");
    ui_box(R_HOST.x, R_HOST.y, R_HOST.w, R_HOST.h, UI_FIELD, UI_DIM);
    const int unset = (strcmp(host, "0.0.0.0") == 0);
    ui_text_centred(R_HOST.x, R_HOST.y, R_HOST.w, R_HOST.h, UI_SIZE_BODY,
                    unset ? UI_DIM : UI_TEXT, unset ? "tap to set" : host);

    ui_text(180, 318, UI_SIZE_BODY, UI_DIM, "port");
    ui_box(R_PORT.x, R_PORT.y, R_PORT.w, R_PORT.h, UI_FIELD, UI_DIM);
    {
        char p[16];
        snprintf(p, sizeof(p), "%u", s->port);
        ui_text_centred(R_PORT.x, R_PORT.y, R_PORT.w, R_PORT.h, UI_SIZE_BODY, UI_TEXT, p);
    }

    draw_button(&R_CONNECT, "CONNECT", UI_ACCENT);
    draw_button(&R_QUIT, "HOME -> EXIT", UI_PANEL);

    if (!decoder_ok) {
        ui_text(360, 550, UI_SIZE_BODY, UI_DANGER, "decoder: %s", why);
    } else if (note && note[0]) {
        ui_text(360, 550, UI_SIZE_BODY, UI_DANGER, "%s", note);
    }
}

static void draw_streaming(const Settings *s, unsigned fps)
{
    const NetInfo *info = net_info();
    char host[32];
    unsigned decoded, empty, errors;

    settings_host_string(s, host, sizeof(host));
    video_stats(&decoded, &empty, &errors);

    /*
     * These diagnostics are shown ONLY while the settings overlay
     * is open. Normal gameplay has no HUD over the video.
     */
    ui_text(180, 535, UI_SIZE_BODY, UI_TEXT,
            "diagnostics");

    ui_text(180, 575, UI_SIZE_BODY, UI_DIM,
            "%s:%u -- %s",
            host, s->port, info->status);

    ui_text(180, 615, UI_SIZE_BODY, UI_DIM,
            "h264 %u decoded  %u waiting  %u errors  %u fps",
            decoded, empty, errors, fps);

    ui_text(180, 655, UI_SIZE_BODY, UI_DIM,
            "rx %llu KiB  step %s  errno %d",
            (unsigned long long)(info->rx_bytes / 1024),
            info->last_step,
            info->last_errno);
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

    Settings settings;
    settings_load(&settings);

    char decoder_why[128] = { 0 };
    int decoder_ok =
        video_init(MAX_WIDTH, MAX_HEIGHT, decoder_why, sizeof(decoder_why)) == 0;
    WHBLogPrintf("capture2cloud: decoder %s", decoder_ok ? "ready" : decoder_why);

    char audio_why[128] = { 0 };
    int audio_ok =
        audio_init(48000, 2,
                   audio_why, sizeof(audio_why)) == 0;

    WHBLogPrintf("capture2cloud: audio %s",
                 audio_ok ? "ready" : audio_why);

    int ui_alive = 1;
    int video_alive = decoder_ok ? 1 : 0;
    int audio_alive = audio_ok ? 1 : 0;

    State state = STATE_SETTINGS;
    char note[160] = { 0 };
    UiInput in;
    memset(&in, 0, sizeof(in));

    VideoFrame frame;
    int have_frame = 0;
    int new_frame = 0;
    int menu_open = 0;
    unsigned frames = 0, fps = 0;
    uint32_t fps_at = 0;

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

            if (video_alive) {
                video_exit();
                video_alive = 0;
            }

            WHBLogPrintf("suspend 3/4: H264DEC done");
            WHBLogPrintf("suspend 4/4: SDL/GX2 begin");

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

            WHBLogPrintf("resume: rebuilding audio");

            audio_why[0] = '\0';

            audio_ok =
                audio_init(48000, 2,
                           audio_why,
                           sizeof(audio_why)) == 0;

            audio_alive = audio_ok ? 1 : 0;

            WHBLogPrintf("resume: audio %s",
                         audio_ok ? "ready" : audio_why);

            WHBLogPrintf("resume: rebuilding H264DEC");

            decoder_why[0] = '\0';
            decoder_ok =
                video_init(MAX_WIDTH, MAX_HEIGHT,
                           decoder_why, sizeof(decoder_why)) == 0;

            video_alive = decoder_ok ? 1 : 0;

            WHBLogPrintf("resume: decoder %s",
                         decoder_ok ? "ready" : decoder_why);

            memset(&in, 0, sizeof(in));
            frames = 0;
            fps = 0;
            fps_at = SDL_GetTicks();

            continue;
        }

        ui_poll(&in);
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
                        WHBLogPrintf("capture2cloud: connecting to %s:%u", host, settings.port);
                    }
                }
            }
        } else {
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
                    if (audio_alive) {
                        audio_decode(payload, size);
                    }
                    continue;
                }

                if (kind != C2S_MSG_VIDEO) {
                    continue;
                }

                if (video_decode(payload, size, &frame) == 1) {
                    have_frame = 1;
                    new_frame = 1;
                    frames++;
                }
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

        const uint32_t now = SDL_GetTicks();
        if (now - fps_at >= 1000) {
            fps = frames;
            frames = 0;
            fps_at = now;
        }

        ui_begin();

        if (state == STATE_SETTINGS) {
            draw_settings(&settings, note, decoder_ok, decoder_why);
        } else {
            /*
             * Still temporary:
             * SDL currently converts NV12 -> RGB on the CPU.
             * The GX2-native path comes next.
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
                ui_box(120, 55, 1040, 650,
                       UI_BG, UI_DIM);

                draw_settings(&settings,
                              note,
                              decoder_ok,
                              decoder_why);

                draw_streaming(&settings, fps);
            }

            draw_menu_marker(menu_open);
        }

        ui_present();
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

    if (video_alive) {
        WHBLogPrintf("shutdown: final H264DEC cleanup");
        video_exit();
        video_alive = 0;
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
