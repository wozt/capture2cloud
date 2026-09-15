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
#define BTN_OCTET_W 150
#define BTN_OCTET_H 60
#define OCTET_X(i)  (40 + (i) * 190)
#define ROW_PLUS_Y  110
#define ROW_VALUE_Y 180
#define ROW_MINUS_Y 225

static const Rect RECT_PORT_MINUS = { 20, 350, 100, 60 };
static const Rect RECT_PORT_PLUS  = { 130, 350, 100, 60 };
static const Rect RECT_CONNECT    = { 280, 350, 250, 60 };
static const Rect RECT_QUIT       = { 560, 350, 250, 60 };

#define C_BUTTON  0x404a56FF
#define C_ACCENT  0x2f6f4eFF
#define C_DANGER  0x6f2f2fFF

static void draw_settings(const Settings *s, const char *note, int touch_x, int touch_y)
{
    char buf[96];

    OSScreenPutFontEx(SCREEN_DRC, 0, 0, "capture2cloud -- where is the host?");
    OSScreenPutFontEx(SCREEN_DRC, 0, 2, "host address");

    for (int i = 0; i < 4; i++) {
        Rect plus  = { OCTET_X(i), ROW_PLUS_Y,  BTN_OCTET_W, BTN_OCTET_H };
        Rect minus = { OCTET_X(i), ROW_MINUS_Y, BTN_OCTET_W, BTN_OCTET_H };
        Rect value = { OCTET_X(i), ROW_VALUE_Y, BTN_OCTET_W, CELL_H };
        fill_rect(SCREEN_DRC, &plus, C_BUTTON);
        fill_rect(SCREEN_DRC, &minus, C_BUTTON);
        text_in_rect(SCREEN_DRC, &plus, "+");
        text_in_rect(SCREEN_DRC, &minus, "-");
        snprintf(buf, sizeof(buf), "%u", s->host[i]);
        text_in_rect(SCREEN_DRC, &value, buf);
    }

    snprintf(buf, sizeof(buf), "port %u", s->port);
    OSScreenPutFontEx(SCREEN_DRC, 2, 12, buf);

    fill_rect(SCREEN_DRC, &RECT_PORT_MINUS, C_BUTTON);
    fill_rect(SCREEN_DRC, &RECT_PORT_PLUS, C_BUTTON);
    fill_rect(SCREEN_DRC, &RECT_CONNECT, C_ACCENT);
    fill_rect(SCREEN_DRC, &RECT_QUIT, C_DANGER);
    text_in_rect(SCREEN_DRC, &RECT_PORT_MINUS, "port -");
    text_in_rect(SCREEN_DRC, &RECT_PORT_PLUS, "port +");
    text_in_rect(SCREEN_DRC, &RECT_CONNECT, "CONNECT");
    text_in_rect(SCREEN_DRC, &RECT_QUIT, "QUIT");

    /*
     * The live touch position, on screen.
     *
     * Not decoration. The calibrated point is documented only as
     * "calibrated", and whether it arrives in this panel's 854x480 or in
     * something else is the difference between buttons that work and
     * buttons that do nothing at all. Until it has been seen on
     * hardware once, this is how it gets seen.
     */
    snprintf(buf, sizeof(buf), "touch %d,%d", touch_x, touch_y);
    OSScreenPutFontEx(SCREEN_DRC, 2, 18, buf);
    if (note && note[0]) {
        OSScreenPutFontEx(SCREEN_DRC, 20, 18, note);
    }

    /* The television says the same thing, for whoever is not holding
     * the pad. */
    settings_host_string(s, buf, sizeof(buf));
    OSScreenPutFontEx(SCREEN_TV, 2, 2, "capture2cloud -- Wii U");
    {
        char line[96];
        snprintf(line, sizeof(line), "host %s:%u", buf, s->port);
        OSScreenPutFontEx(SCREEN_TV, 2, 4, line);
    }
    OSScreenPutFontEx(SCREEN_TV, 2, 6, "set the address on the GamePad, then CONNECT");
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
             * ...Ex, with the resolution named.
             *
             * VPADGetTPCalibratedPoint without the suffix calibrates
             * into a DEFAULT resolution, which is not this panel's. The
             * buttons below are laid out in 854x480 and were being
             * hit-tested against coordinates in something else, so not
             * one of them ever responded -- reported from the sofa as
             * "your touch buttons do not even work", quite rightly.
             */
            VPADTouchData cal;
            VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &cal, &vpad.tpNormal);
            tx = cal.x;
            ty = cal.y;
            const int touching = vpad.tpNormal.touched != 0;
            if (was_touching && !touching) {
                tapped = 1;   /* released: this is the tap */
            }
            was_touching = touching;
        }

        if (state == STATE_SETTINGS) {
            if (tapped) {
                for (int i = 0; i < 4; i++) {
                    Rect plus  = { OCTET_X(i), ROW_PLUS_Y,  BTN_OCTET_W, BTN_OCTET_H };
                    Rect minus = { OCTET_X(i), ROW_MINUS_Y, BTN_OCTET_W, BTN_OCTET_H };
                    if (in_rect(&plus, tx, ty)) {
                        settings.host[i] = (uint8_t)((settings.host[i] + 1) % 256);
                    } else if (in_rect(&minus, tx, ty)) {
                        settings.host[i] = (uint8_t)((settings.host[i] + 255) % 256);
                    }
                }
                if (in_rect(&RECT_PORT_MINUS, tx, ty) && settings.port > 1) {
                    settings.port--;
                } else if (in_rect(&RECT_PORT_PLUS, tx, ty) && settings.port < 65535) {
                    settings.port++;
                } else if (in_rect(&RECT_QUIT, tx, ty)) {
                    quitting = 1;
                } else if (in_rect(&RECT_CONNECT, tx, ty)) {
                    if (!decoder_ok) {
                        snprintf(note, sizeof(note), "no decoder");
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
