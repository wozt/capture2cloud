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
static const Rect R_BACK    = {  40, 620, 220, 64 };

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
    draw_button(&R_QUIT, "QUIT", UI_DANGER);

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
     * The status lines are the whole diagnostic surface of this
     * console: there is no shell and no log file, and "the host is
     * unreachable" and "the host is there and sending something the
     * decoder will not take" look identical without them.
     */
    ui_text(40, 30, UI_SIZE_BODY, UI_TEXT, "%s:%u -- %s", host, s->port, info->status);
    ui_text(40, 70, UI_SIZE_BODY, UI_DIM, "stream %ux%u codec %u  %s", info->width, info->height,
            info->video_codec, info->may_control ? "player" : "viewer");
    ui_text(40, 110, UI_SIZE_BODY, UI_DIM, "decoded %u  waiting %u  errors %u  %u fps", decoded,
            empty, errors, fps);
    ui_text(40, 150, UI_SIZE_BODY, UI_DIM, "rx %llu KiB  step: %s (errno %d)",
            (unsigned long long)(info->rx_bytes / 1024), info->last_step, info->last_errno);
    {
        const uint32_t ip = net_local_ip();
        ui_text(40, 190, UI_SIZE_BODY, UI_DIM, "this console: %u.%u.%u.%u", (ip >> 24) & 0xFF,
                (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    }

    draw_button(&R_BACK, "settings", UI_PANEL);
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
    const int decoder_ok =
        video_init(MAX_WIDTH, MAX_HEIGHT, decoder_why, sizeof(decoder_why)) == 0;
    WHBLogPrintf("capture2cloud: decoder %s", decoder_ok ? "ready" : decoder_why);

    State state = STATE_SETTINGS;
    char note[160] = { 0 };
    UiInput in;
    memset(&in, 0, sizeof(in));

    VideoFrame frame;
    int have_frame = 0;
    unsigned frames = 0, fps = 0;
    uint32_t fps_at = 0;

    while (proc_running()) {
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
                    proc_stop();
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
                if (kind != C2S_MSG_VIDEO) {
                    continue;   /* audio arrives in step three */
                }
                if (video_decode(payload, size, &frame) == 1) {
                    have_frame = 1;
                    frames++;
                }
            }

            if (in.tapped && hit(&R_BACK, in.touch_x, in.touch_y)) {
                net_disconnect();
                state = STATE_SETTINGS;
                have_frame = 0;
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
            /* The picture itself is the next step: it belongs in a GX2
             * texture fed straight from the decoder's NV12, which is
             * exactly what this rebuild was for. Until then the numbers
             * say whether it is arriving. */
            (void)have_frame;
            draw_streaming(&settings, fps);
        }
        ui_present();
    }

    WHBLogPrintf("capture2cloud: closing");
    net_disconnect();
    net_exit();
    video_exit();
    ui_shutdown();
    proc_shutdown();
    WHBLogUdpDeinit();
    return 0;
}
