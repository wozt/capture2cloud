/*
 * capture2cloud on a Wii U console -- step one.
 *
 * What this is: the .rpx that connects to the host, decodes what it
 * sends with the console's hardware decoder, and puts it on the
 * television. Nothing else. No sound, no input, no menu, no password --
 * those are steps two to five in SPEC.md, and each of them is easier to
 * get right once this one is known to work.
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
#include <proc_ui/procui.h>
#include <vpad/input.h>
#include <whb/proc.h>

#include "c2s_protocol.h"
#include "net.h"
#include "video.h"

/* Where the host is. A file on the SD card replaces this in step four;
 * until then it is here so there is exactly one thing to edit. */
#define HOST_ADDRESS "192.168.1.10"
#define HOST_PORT    C2S_WIIU_PORT

/* The biggest picture the decoder is asked to reserve for. 720p60 is the
 * path this is built and measured on; 1080p is offered in the menu
 * later, and reserving for it now costs memory on every session that
 * does not use it. SPEC.md has the reasoning. */
#define MAX_WIDTH  1280
#define MAX_HEIGHT 720

/* OSScreen's TV buffer. Every pixel is written by the CPU, so the
 * picture is drawn at a fraction of its size -- see the file comment. */
#define TV_WIDTH   1280
#define TV_HEIGHT  720
#define DRAW_SHIFT 2                       /* 1 pixel drawn per 4x4 block */
#define DRAW_W     (MAX_WIDTH >> DRAW_SHIFT)
#define DRAW_H     (MAX_HEIGHT >> DRAW_SHIFT)

static void *g_tv_buffer;
static void *g_drc_buffer;

static void screen_line(int row, const char *text)
{
    OSScreenPutFontEx(SCREEN_TV, 0, row, text);
    OSScreenPutFontEx(SCREEN_DRC, 0, row, text);
}

/*
 * NV12 to what OSScreen wants, for one pixel.
 *
 * BT.601 limited range, which is what the host's encoders produce. Done
 * in integers because this console has a floating point unit that is
 * not worth waking up for three multiplies.
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

    /* OSScreenPutPixelEx takes 0xRRGGBBAA. */
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

    char why[128] = { 0 };
    const int decoder_ok = video_init(MAX_WIDTH, MAX_HEIGHT, why, sizeof(why)) == 0;
    int net_ok = decoder_ok && net_init() == 0;
    if (net_ok) {
        /* No token: step one connects as a viewer. The host lets a
         * viewer watch and refuses it control, which is exactly the
         * right amount of trust for a client that cannot yet ask for a
         * password. Step four adds the console's own keyboard. */
        net_connect(HOST_ADDRESS, HOST_PORT, NULL);
    }

    VideoFrame frame;
    int have_frame = 0;
    unsigned frames = 0, fps = 0;
    uint64_t fps_at = OSTicksToMilliseconds(OSGetSystemTime());

    while (WHBProcIsRunning()) {
        VPADStatus vpad;
        VPADReadError verr;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
        /*
         * MINUS quits, not HOME, and that is not a preference.
         *
         * HOME is taken by the system: it opens the HOME Menu overlay,
         * which draws itself with GX2. This screen is OSScreen, and the
         * two do not compose -- so on the console the overlay opened
         * INVISIBLY. Reported from the sofa as "it said HOME to quit,
         * that never worked, and I could hear the Wii U menu" -- the
         * menu was there, being heard and not drawn, with no way to
         * reach "Close software".
         *
         * So this app provides its own way out, and says so on screen.
         * HOME still works the moment the picture moves to GX2, which
         * is the next step anyway.
         */
        if (verr == VPAD_READ_SUCCESS && (vpad.trigger & VPAD_BUTTON_MINUS)) {
            break;
        }

        if (!net_ok && verr == VPAD_READ_SUCCESS && (vpad.trigger & VPAD_BUTTON_PLUS)) {
            /* Asked again rather than requiring a relaunch: the usual
             * reason to be here is that the network came up a moment
             * after the program did. */
            net_ok = net_init() == 0;
            if (net_ok) {
                net_connect(HOST_ADDRESS, HOST_PORT, NULL);
            }
        }

        if (net_ok) {
            net_poll();

            /*
             * Every frame that has arrived, not just one.
             *
             * The decoder is faster than this loop's drawing, and a
             * queue that is only ever drained one frame per iteration
             * grows without bound the moment drawing falls behind --
             * which, with a CPU-drawn picture, it does. Only the newest
             * is drawn.
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
        }

        const uint64_t now = OSTicksToMilliseconds(OSGetSystemTime());
        if (now - fps_at >= 1000) {
            fps = frames;
            frames = 0;
            fps_at = now;
        }

        OSScreenClearBufferEx(SCREEN_TV, 0);
        OSScreenClearBufferEx(SCREEN_DRC, 0);

        if (have_frame) {
            draw_frame(&frame);
        }

        /*
         * The status lines are the whole diagnostic surface of this
         * console: there is no shell, no log, and no way to tell "the
         * host is unreachable" from "the host is there and sending
         * something the decoder will not take" without them.
         */
        const NetInfo *info = net_info();
        char line[96];
        unsigned decoded, empty, errors;
        video_stats(&decoded, &empty, &errors);

        screen_line(0, "capture2cloud -- Wii U -- press MINUS to quit, PLUS to retry");
        if (!decoder_ok) {
            snprintf(line, sizeof(line), "decoder: %s", why);
            screen_line(2, line);
        } else if (!net_ok) {
            snprintf(line, sizeof(line), "network: %s", info->status);
            screen_line(2, line);
            screen_line(3, "check System Settings > Internet, then press PLUS");
        } else {
            snprintf(line, sizeof(line), "host %s:%u -- %s", HOST_ADDRESS, (unsigned)HOST_PORT,
                     info->status);
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
        }

        OSScreenFlipBuffersEx(SCREEN_TV);
        OSScreenFlipBuffersEx(SCREEN_DRC);
    }

    if (net_ok) {
        net_disconnect();
        net_exit();
    }
    video_exit();
    screen_stop();
    WHBProcShutdown();
    return 0;
}
