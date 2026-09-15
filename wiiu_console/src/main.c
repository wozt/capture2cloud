/*
 * capture2cloud on a Wii U console -- step one.
 *
 * Connects to the host, decodes what it sends with the console's
 * hardware decoder, and puts it on the television. No sound, no
 * controller input, no password -- those are steps two to five in
 * SPEC.md.
 *
 * It opens on a settings screen and touches nothing until you press
 * CONNECT. That is deliberate, and it answers two separate things the
 * first hardware run turned up: the host address was compiled in and
 * wrong, and the console was reporting Nintendo Network activity at
 * launch. The address is editable on the pad before anything is
 * dialled, and the network layer is not so much as initialised until
 * you ask for it -- so if those messages still appear, they are the
 * console's and not this program's.
 *
 * The picture goes out through OSScreen, which is the console's simple
 * framebuffer: one 32-bit pixel at a time, from the CPU. That is fast
 * enough for a proof and nowhere near fast enough for the real thing --
 * 720p60 is 55 million pixel writes a second. It is here because it
 * needs no shaders, so the network and the decoder can be proven before
 * GX2 is written, and it draws a reduced picture rather than pretending
 * otherwise. GX2 sampling the decoder's NV12 directly is what replaces
 * it, and the only file that should have to change is this one.
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/proc.h>

#include "c2s_protocol.h"
#include "keyboard.h"
#include "net.h"
#include "settings.h"
#include "video.h"

/* The biggest picture the decoder is asked to reserve for. 720p60 is the
 * path this is built and measured on; 1080p is offered in the menu
 * later, and reserving for it now costs memory on every session that
 * does not use it. SPEC.md has the reasoning. */
#define MAX_WIDTH  1280
#define MAX_HEIGHT 720

#define TV_WIDTH   1280
#define TV_HEIGHT  720
#define DRC_WIDTH  854
#define DRC_HEIGHT 480

/* OSScreen's font cell. Text is placed by cell and the touch buttons by
 * pixel, so the two have to agree about where a row is. */
#define CELL_W 12
#define CELL_H 24

/* Every pixel of the picture is written by the CPU, so it is drawn at a
 * fraction of its size -- see the file comment. */
#define DRAW_SHIFT 2   /* one pixel per 4x4 block */

typedef enum { STATE_SETTINGS, STATE_STREAMING } State;

typedef struct {
    int x, y, w, h;
} Rect;

static void *g_tv_buffer;
static void *g_drc_buffer;

/* --- drawing ---------------------------------------------------------- */

static void fill_rect(OSScreenID screen, const Rect *r, uint32_t colour)
{
    for (int y = 0; y < r->h; y++) {
        for (int x = 0; x < r->w; x++) {
            OSScreenPutPixelEx(screen, (uint32_t)(r->x + x), (uint32_t)(r->y + y), colour);
        }
    }
}

/* A label centred in a rectangle, as near as a character grid allows. */
static void text_in_rect(OSScreenID screen, const Rect *r, const char *text)
{
    const int len = (int)strlen(text);
    int col = (r->x + (r->w - len * CELL_W) / 2) / CELL_W;
    int row = (r->y + (r->h - CELL_H) / 2) / CELL_H;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    OSScreenPutFontEx(screen, (uint32_t)col, (uint32_t)row, text);
}

static void screen_line(int row, const char *text)
{
    OSScreenPutFontEx(SCREEN_TV, 0, (uint32_t)row, text);
    OSScreenPutFontEx(SCREEN_DRC, 0, (uint32_t)row, text);
}

static int in_rect(const Rect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* --- the settings screen ---------------------------------------------- */
/*
 * Laid out in pixels on the pad's 854x480 panel. The buttons are large
 * on purpose: this is a finger on a resistive panel, and the first
 * version of this had no way out at all.
 */
/*
 * Fields, not a row of plus and minus.
 *
 * The first version put a [+] and a [-] under each octet of the
 * address. Typing 192.168.2.100 that way is 462 taps, and the verdict
 * from the sofa was the right one. A field is tapped, the console's own
 * keyboard opens, you type it.
 */
static const Rect RECT_HOST    = { 40, 90, 600, 70 };
static const Rect RECT_PORT    = { 40, 185, 300, 70 };
static const Rect RECT_CONNECT = { 40, 310, 320, 80 };
static const Rect RECT_QUIT    = { 480, 310, 320, 80 };

#define C_BUTTON  0x404a56FF
#define C_FIELD   0x1c2430FF
#define C_ACCENT  0x2f6f4eFF
#define C_DANGER  0x6f2f2fFF

static void draw_settings(const Settings *s, const char *note, int touch_x, int touch_y)
{
    char host[32], buf[96];
    settings_host_string(s, host, sizeof(host));

    OSScreenPutFontEx(SCREEN_DRC, 1, 0, "capture2cloud");
    OSScreenPutFontEx(SCREEN_DRC, 1, 1, "tap a field to type with the console keyboard");

    fill_rect(SCREEN_DRC, &RECT_HOST, C_FIELD);
    fill_rect(SCREEN_DRC, &RECT_PORT, C_FIELD);
    OSScreenPutFontEx(SCREEN_DRC, 1, 3, "host");
    OSScreenPutFontEx(SCREEN_DRC, 1, 7, "port");
    text_in_rect(SCREEN_DRC, &RECT_HOST, host[0] && strcmp(host, "0.0.0.0") != 0
                                             ? host : "tap to set");
    snprintf(buf, sizeof(buf), "%u", s->port);
    text_in_rect(SCREEN_DRC, &RECT_PORT, buf);

    fill_rect(SCREEN_DRC, &RECT_CONNECT, C_ACCENT);
    fill_rect(SCREEN_DRC, &RECT_QUIT, C_DANGER);
    text_in_rect(SCREEN_DRC, &RECT_CONNECT, "CONNECT");
    text_in_rect(SCREEN_DRC, &RECT_QUIT, "QUIT");

    if (note && note[0]) {
        OSScreenPutFontEx(SCREEN_DRC, 1, 17, note);
    }
    /* Still on screen because the touch mapping has been wrong twice.
     * One look at these numbers while touching a corner settles it,
     * where another round trip does not. */
    {
        char t[48];
        snprintf(t, sizeof(t), "touch %d,%d", touch_x, touch_y);
        OSScreenPutFontEx(SCREEN_DRC, 1, 19, t);
    }

    /* The television says the same thing, for whoever is not holding
     * the pad. */
    OSScreenPutFontEx(SCREEN_TV, 2, 2, "capture2cloud -- Wii U");
    snprintf(buf, sizeof(buf), "host %s:%u", host, s->port);
    OSScreenPutFontEx(SCREEN_TV, 2, 4, buf);
    OSScreenPutFontEx(SCREEN_TV, 2, 6, "set the address on the GamePad, then CONNECT");
    if (note && note[0]) {
        OSScreenPutFontEx(SCREEN_TV, 2, 8, note);
    }
}

/* --- the picture ------------------------------------------------------ */

/*
 * NV12 to what OSScreen wants, for one pixel. BT.601 limited range,
 * which is what the host's encoders produce. Integers because this
 * console has a floating point unit not worth waking for three
 * multiplies.
 */
static uint32_t nv12_pixel(const VideoFrame *f, int x, int y)
{
    const int Y = f->luma[(size_t)y * f->stride + x] - 16;
    const uint8_t *cr = f->chroma + (size_t)(y / 2) * f->stride + (x & ~1);
    const int U = cr[0] - 128;
    const int V = cr[1] - 128;

    int r = (298 * Y + 409 * V + 128) >> 8;
    int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
    int b = (298 * Y + 516 * U + 128) >> 8;

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;
}

static void draw_frame(const VideoFrame *f)
{
    const int step = 1 << DRAW_SHIFT;
    const int w = f->width / step;
    const int h = f->height / step;
    const int x0 = (TV_WIDTH - w) / 2;
    const int y0 = (TV_HEIGHT - h) / 2;

    for (int y = 0; y < h; y++) {
        const int sy = y * step;
        for (int x = 0; x < w; x++) {
            OSScreenPutPixelEx(SCREEN_TV, (uint32_t)(x0 + x), (uint32_t)(y0 + y),
                               nv12_pixel(f, x * step, sy));
        }
    }
}

/* --- plumbing --------------------------------------------------------- */

static int screen_start(void)
{
    OSScreenInit();

    const uint32_t tv = OSScreenGetBufferSizeEx(SCREEN_TV);
    const uint32_t drc = OSScreenGetBufferSizeEx(SCREEN_DRC);
    g_tv_buffer = memalign(0x100, tv);
    g_drc_buffer = memalign(0x100, drc);
    if (!g_tv_buffer || !g_drc_buffer) {
        return -1;
    }
    OSScreenSetBufferEx(SCREEN_TV, g_tv_buffer);
    OSScreenSetBufferEx(SCREEN_DRC, g_drc_buffer);
    OSScreenEnableEx(SCREEN_TV, 1);
    OSScreenEnableEx(SCREEN_DRC, 1);
    return 0;
}

static void screen_stop(void)
{
    OSScreenShutdown();
    free(g_tv_buffer);
    free(g_drc_buffer);
    g_tv_buffer = g_drc_buffer = NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBProcInit();
    VPADInit();

    if (screen_start() != 0) {
        WHBProcShutdown();
        return 1;
    }

    Settings settings;
    settings_load(&settings);

    char why[128] = { 0 };
    const int decoder_ok = video_init(MAX_WIDTH, MAX_HEIGHT, why, sizeof(why)) == 0;

    State state = STATE_SETTINGS;
    /* Big enough for net_info()->status (96) and a prefix: the
     * compiler checks this, and a silently truncated diagnostic is
     * worse than no diagnostic. */
    char note[160] = { 0 };
    int quitting = 0;
    int was_touching = 0;

    VideoFrame frame;
    int have_frame = 0;
    unsigned frames = 0, fps = 0;
    uint64_t fps_at = OSTicksToMilliseconds(OSGetSystemTime());

    while (WHBProcIsRunning()) {
        VPADStatus vpad;
        VPADReadError verr;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
        const int have_pad = (verr == VPAD_READ_SUCCESS);

        /* A touch, in the panel's own coordinates, taken on the release
         * rather than the press: a finger resting on a button must not
         * count as twenty presses. */
        int tx = 0, ty = 0, tapped = 0;
        if (have_pad) {
            /*
             * Two things here were wrong twice over, and both are taken
             * from dimok's homebrew_launcher, which has been driving
             * this panel with a finger since 2016.
             *
             * The point to calibrate is tpFiltered1, NOT tpNormal.
             * tpNormal is the raw sample; passing it produced
             * coordinates that never matched anything, which is why
             * nothing responded at all.
             *
             * And what comes back is in 1280x720, not this panel's
             * 854x480 -- that reference divides by exactly those
             * numbers. So the point is scaled into the layout's space
             * rather than the layout being guessed into the point's.
             */
            VPADTouchData cal;
            VPADGetTPCalibratedPoint(VPAD_CHAN_0, &cal, &vpad.tpFiltered1);
            tx = (int)cal.x * DRC_WIDTH / 1280;
            ty = (int)cal.y * DRC_HEIGHT / 720;

            const int touching = vpad.tpNormal.touched != 0;
            if (was_touching && !touching) {
                tapped = 1;   /* released: this is the tap */
            }
            was_touching = touching;
        }

        if (state == STATE_SETTINGS) {
            if (tapped) {
                char typed[64], kb_why[96];
                if (in_rect(&RECT_HOST, tx, ty)) {
                    /*
                     * The console's keyboard draws with GX2 and this
                     * menu is OSScreen, so the display is handed over
                     * for the duration and taken back afterwards. That
                     * is why this blocks rather than being another
                     * state in this loop.
                     */
                    char current[32];
                    settings_host_string(&settings, current, sizeof(current));
                    screen_stop();
                    const int r = keyboard_prompt("host address", current, 1, typed,
                                                  sizeof(typed), kb_why, sizeof(kb_why));
                    if (screen_start() != 0) {
                        break;   /* nothing can be drawn any more */
                    }
                    if (r < 0) {
                        snprintf(note, sizeof(note), "%s", kb_why);
                    } else if (r == 1) {
                        if (settings_set_host_string(&settings, typed) != 0) {
                            snprintf(note, sizeof(note), "not an address: %s", typed);
                        } else {
                            note[0] = '\0';
                        }
                    }
                } else if (in_rect(&RECT_PORT, tx, ty)) {
                    char current[16];
                    snprintf(current, sizeof(current), "%u", settings.port);
                    screen_stop();
                    const int r = keyboard_prompt("port", current, 1, typed, sizeof(typed),
                                                  kb_why, sizeof(kb_why));
                    if (screen_start() != 0) {
                        break;
                    }
                    if (r < 0) {
                        snprintf(note, sizeof(note), "%s", kb_why);
                    } else if (r == 1) {
                        const int v = atoi(typed);
                        if (v > 0 && v < 65536) {
                            settings.port = (uint16_t)v;
                            note[0] = '\0';
                        } else {
                            snprintf(note, sizeof(note), "not a port: %s", typed);
                        }
                    }
                } else if (in_rect(&RECT_QUIT, tx, ty)) {
                    quitting = 1;
                } else if (in_rect(&RECT_CONNECT, tx, ty)) {
                    if (!decoder_ok) {
                        snprintf(note, sizeof(note), "no decoder");
                    } else if (settings.host[0] == 0) {
                        snprintf(note, sizeof(note), "set the host address first");
                    } else if (net_init() != 0) {
                        snprintf(note, sizeof(note), "%s", net_info()->status);
                    } else {
                        char host[32];
                        settings_host_string(&settings, host, sizeof(host));
                        char save_why[64];
                        if (settings_save(&settings, save_why, sizeof(save_why)) != 0) {
                            /* Worth saying, not worth stopping for: the
                             * address still applies to this session. */
                            snprintf(note, sizeof(note), "not saved: %s", save_why);
                        }
                        /* No token: step one connects as a viewer. */
                        net_connect(host, settings.port, NULL);
                        state = STATE_STREAMING;
                    }
                }
            }
            /* The buttons are the way out, but a stick-and-buttons way
             * out costs nothing and helps when a panel is unresponsive. */
            if (have_pad && (vpad.trigger & VPAD_BUTTON_MINUS)) {
                quitting = 1;
            }
        } else {
            net_poll();

            /*
             * Every frame that has arrived, not just one. The decoder is
             * faster than this loop's drawing, and a queue drained one
             * frame per iteration grows without bound the moment drawing
             * falls behind -- which, with a CPU-drawn picture, it does.
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
            if (tapped && in_rect(&RECT_QUIT, tx, ty)) {
                quitting = 1;
            }
            if (have_pad && (vpad.trigger & VPAD_BUTTON_MINUS)) {
                quitting = 1;
            }
            if (have_pad && (vpad.trigger & VPAD_BUTTON_B)) {
                net_disconnect();
                state = STATE_SETTINGS;
                have_frame = 0;
            }
        }

        if (quitting) {
            break;
        }

        const uint64_t now = OSTicksToMilliseconds(OSGetSystemTime());
        if (now - fps_at >= 1000) {
            fps = frames;
            frames = 0;
            fps_at = now;
        }

        OSScreenClearBufferEx(SCREEN_TV, 0);
        OSScreenClearBufferEx(SCREEN_DRC, 0);

        if (state == STATE_SETTINGS) {
            if (!decoder_ok) {
                char line[160];
                snprintf(line, sizeof(line), "decoder: %s", why);
                OSScreenPutFontEx(SCREEN_TV, 2, 8, line);
            }
            draw_settings(&settings, note, tx, ty);
        } else {
            if (have_frame) {
                draw_frame(&frame);
            }

            /*
             * The status lines are the whole diagnostic surface of this
             * console: there is no shell, no log, and no way to tell
             * "the host is unreachable" from "the host is there and
             * sending something the decoder will not take" without them.
             */
            const NetInfo *info = net_info();
            char line[192], host[32];
            unsigned decoded, empty, errors;
            video_stats(&decoded, &empty, &errors);
            settings_host_string(&settings, host, sizeof(host));

            screen_line(0, "capture2cloud -- B settings, MINUS quits");
            snprintf(line, sizeof(line), "host %s:%u -- %s", host, settings.port, info->status);
            screen_line(2, line);
            snprintf(line, sizeof(line), "stream %ux%u codec %u  %s", info->width, info->height,
                     info->video_codec, info->may_control ? "player" : "viewer");
            screen_line(3, line);
            snprintf(line, sizeof(line), "decoded %u  waiting %u  errors %u  %u fps", decoded,
                     empty, errors, fps);
            screen_line(4, line);
            snprintf(line, sizeof(line), "rx %llu KiB  step: %s (errno %d)",
                     (unsigned long long)(info->rx_bytes / 1024), info->last_step,
                     info->last_errno);
            screen_line(5, line);
            const uint32_t ip = net_local_ip();
            snprintf(line, sizeof(line), "this console: %u.%u.%u.%u",
                     (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
            screen_line(6, line);

            fill_rect(SCREEN_DRC, &RECT_QUIT, C_DANGER);
            text_in_rect(SCREEN_DRC, &RECT_QUIT, "QUIT");
        }

        OSScreenFlipBuffersEx(SCREEN_TV);
        OSScreenFlipBuffersEx(SCREEN_DRC);
    }

    net_disconnect();
    net_exit();
    video_exit();
    screen_stop();

    /*
     * Asked for, rather than assumed.
     *
     * Returning from main() does not send this console anywhere: the
     * first version simply ended, and the report was "it does not bring
     * me back to the Wii U menu". The system has to be told where to go
     * next, and then given the chance to take the program down in its
     * own time -- which is what the loop below is for.
     */
    SYSLaunchMenu();
    while (WHBProcIsRunning()) {
        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    WHBProcShutdown();
    return 0;
}
