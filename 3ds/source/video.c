#include "video.h"
#include "c2s_protocol.h"

#include <3ds.h>

#include <stdio.h>
#include <jpeglib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_WIDTH 400
#define VIDEO_HEIGHT 240

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} DecodeError;

static struct jpeg_decompress_struct g_jpeg;
static DecodeError g_error;
static uint16_t *g_pixels;
static AVCodecContext *g_mpeg4;
static AVFrame *g_mpeg4_frame;
static struct SwsContext *g_mpeg4_scale;
static uint8_t g_last_codec;
static VideoStats g_stats;
static uint32_t g_window_start;
static uint32_t g_window_received;
static uint32_t g_window_decoded;
static uint32_t g_window_displayed;
static uint64_t g_window_bytes;
static uint64_t g_decode_total_ms;
static uint64_t g_upload_total_ms;

static void decode_failed(j_common_ptr jpeg)
{
    DecodeError *error = (DecodeError *)jpeg->err;
    longjmp(error->jump, 1);
}

bool video_init(void)
{
    g_pixels = malloc(VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint16_t));
    if (!g_pixels) return false;
    g_jpeg.err = jpeg_std_error(&g_error.base);
    g_error.base.error_exit = decode_failed;
    if (setjmp(g_error.jump)) {
        free(g_pixels);
        g_pixels = NULL;
        return false;
    }
    jpeg_create_decompress(&g_jpeg);

    const AVCodec *mpeg4 = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    g_mpeg4 = mpeg4 ? avcodec_alloc_context3(mpeg4) : NULL;
    g_mpeg4_frame = av_frame_alloc();
    if (!g_mpeg4 || !g_mpeg4_frame || avcodec_open2(g_mpeg4, mpeg4, NULL) < 0) {
        av_frame_free(&g_mpeg4_frame);
        avcodec_free_context(&g_mpeg4);
        jpeg_destroy_decompress(&g_jpeg);
        free(g_pixels);
        g_pixels = NULL;
        return false;
    }
    g_window_start = osGetTime();
    video_clear();
    return true;
}

void video_clear(void)
{
    u16 width = 0, height = 0;
    uint16_t *framebuffer = (uint16_t *)gfxGetFramebuffer(
        GFX_TOP, GFX_LEFT, &width, &height);
    if (framebuffer) memset(framebuffer, 0, (size_t)width * height * sizeof(uint16_t));
}

void video_exit(void)
{
    sws_freeContext(g_mpeg4_scale);
    g_mpeg4_scale = NULL;
    av_frame_free(&g_mpeg4_frame);
    avcodec_free_context(&g_mpeg4);
    jpeg_destroy_decompress(&g_jpeg);
    free(g_pixels);
    g_pixels = NULL;
}

void video_note_received_bytes(uint32_t frame_size)
{
    g_window_received++;
    g_window_bytes += frame_size;
}

static void refresh_rates(uint32_t now)
{
    uint32_t elapsed = now - g_window_start;
    if (elapsed < 1000) return;
    float seconds = elapsed / 1000.0f;
    g_stats.receive_fps = g_window_received / seconds;
    g_stats.decode_fps = g_window_decoded / seconds;
    g_stats.display_fps = g_window_displayed / seconds;
    g_stats.bitrate_kbps = (float)(g_window_bytes * 8u) / elapsed;
    if (g_window_decoded) {
        g_stats.decode_ms = (float)g_decode_total_ms / g_window_decoded;
        g_stats.upload_ms = (float)g_upload_total_ms / g_window_decoded;
    }
    g_window_start = now;
    g_window_received = 0;
    g_window_decoded = 0;
    g_window_displayed = 0;
    g_window_bytes = 0;
    g_decode_total_ms = 0;
    g_upload_total_ms = 0;
}

static bool decode_jpeg(const uint8_t *data, uint32_t size)
{
    if (setjmp(g_error.jump)) {
        jpeg_abort_decompress(&g_jpeg);
        return false;
    }
    jpeg_mem_src(&g_jpeg, data, size);
    if (jpeg_read_header(&g_jpeg, TRUE) != JPEG_HEADER_OK ||
        g_jpeg.image_width != VIDEO_WIDTH || g_jpeg.image_height != VIDEO_HEIGHT) {
        jpeg_abort_decompress(&g_jpeg);
        return false;
    }
    g_jpeg.out_color_space = JCS_RGB565;
    g_jpeg.dct_method = JDCT_IFAST;
    g_jpeg.do_fancy_upsampling = FALSE;
    g_jpeg.do_block_smoothing = FALSE;
    jpeg_start_decompress(&g_jpeg);
    while (g_jpeg.output_scanline < g_jpeg.output_height) {
        JSAMPROW row = (uint8_t *)g_pixels +
            (size_t)g_jpeg.output_scanline * VIDEO_WIDTH * sizeof(uint16_t);
        jpeg_read_scanlines(&g_jpeg, &row, 1);
    }
    jpeg_finish_decompress(&g_jpeg);
    return true;
}

static bool decode_mpeg4(const uint8_t *data, uint32_t size)
{
    AVPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.data = (uint8_t *)data;
    packet.size = (int)size;
    if (avcodec_send_packet(g_mpeg4, &packet) < 0 ||
        avcodec_receive_frame(g_mpeg4, g_mpeg4_frame) < 0) {
        return false;
    }
    if (g_mpeg4_frame->width != VIDEO_WIDTH ||
        g_mpeg4_frame->height != VIDEO_HEIGHT) return false;

    g_mpeg4_scale = sws_getCachedContext(
        g_mpeg4_scale,
        g_mpeg4_frame->width, g_mpeg4_frame->height,
        (enum AVPixelFormat)g_mpeg4_frame->format,
        VIDEO_WIDTH, VIDEO_HEIGHT, AV_PIX_FMT_RGB565LE,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_mpeg4_scale) return false;
    uint8_t *destination[4] = {(uint8_t *)g_pixels, NULL, NULL, NULL};
    int destination_stride[4] = {VIDEO_WIDTH * (int)sizeof(uint16_t), 0, 0, 0};
    return sws_scale(g_mpeg4_scale,
                     (const uint8_t *const *)g_mpeg4_frame->data,
                     g_mpeg4_frame->linesize, 0, g_mpeg4_frame->height,
                     destination, destination_stride) == VIDEO_HEIGHT;
}

bool video_decode_and_present(const uint8_t *data, uint32_t size,
                              uint32_t received_ms, uint8_t codec)
{
    uint32_t decode_start = osGetTime();
    if (codec != g_last_codec) {
        if (codec == C2S_CODEC_OLD3DS_MPEG4) avcodec_flush_buffers(g_mpeg4);
        g_last_codec = codec;
    }
    const bool decoded = codec == C2S_CODEC_OLD3DS_MPEG4
        ? decode_mpeg4(data, size) : decode_jpeg(data, size);
    if (!decoded) {
        g_stats.decode_errors++;
        refresh_rates(osGetTime());
        return false;
    }
    uint32_t decode_end = osGetTime();

    u16 framebuffer_width = 0;
    u16 framebuffer_height = 0;
    uint16_t *framebuffer = (uint16_t *)gfxGetFramebuffer(
        GFX_TOP, GFX_LEFT, &framebuffer_width, &framebuffer_height);
    if (!framebuffer || framebuffer_width < VIDEO_HEIGHT ||
        framebuffer_height < VIDEO_WIDTH) return false;

    /* 3DS framebuffers are rotated: physical columns are contiguous.
     * Decode stays row-major for libjpeg, then this single required copy
     * rotates into the display buffer. */
    for (int x = 0; x < VIDEO_WIDTH; x++) {
        uint16_t *dst = framebuffer + (size_t)x * framebuffer_width;
        for (int y = 0; y < VIDEO_HEIGHT; y++) {
            dst[VIDEO_HEIGHT - 1 - y] = g_pixels[y * VIDEO_WIDTH + x];
        }
    }
    uint32_t upload_end = osGetTime();
    g_decode_total_ms += decode_end - decode_start;
    g_upload_total_ms += upload_end - decode_end;
    g_window_decoded++;
    g_window_displayed++;
    g_stats.decoded++;
    g_stats.local_latency_ms = upload_end - received_ms;
    refresh_rates(upload_end);
    return true;
}

void video_get_stats(VideoStats *stats) { *stats = g_stats; }
