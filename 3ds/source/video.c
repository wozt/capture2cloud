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
static uint8_t *g_yuv420;
static bool g_y2r;
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
    g_pixels = linearAlloc(VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint16_t));
    if (!g_pixels) return false;
    g_jpeg.err = jpeg_std_error(&g_error.base);
    g_error.base.error_exit = decode_failed;
    if (setjmp(g_error.jump)) {
        linearFree(g_pixels);
        g_pixels = NULL;
        return false;
    }
    jpeg_create_decompress(&g_jpeg);

    const AVCodec *mpeg4 = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    g_mpeg4 = mpeg4 ? avcodec_alloc_context3(mpeg4) : NULL;
    g_mpeg4_frame = av_frame_alloc();
    if (g_mpeg4) g_mpeg4->flags2 |= AV_CODEC_FLAG2_FAST;
    if (!g_mpeg4 || !g_mpeg4_frame || avcodec_open2(g_mpeg4, mpeg4, NULL) < 0) {
        av_frame_free(&g_mpeg4_frame);
        avcodec_free_context(&g_mpeg4);
        jpeg_destroy_decompress(&g_jpeg);
        linearFree(g_pixels);
        g_pixels = NULL;
        return false;
    }

    /* Y2R is the 3DS' hardware YUV-to-RGB converter.  MPEG-4 still
     * decodes on ARM11, but the colour conversion no longer consumes
     * that same core.  Keep libswscale as a complete runtime fallback. */
    if (R_SUCCEEDED(y2rInit())) {
        g_yuv420 = linearAlloc(VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2);
        if (g_yuv420) {
            g_y2r = true;
        } else {
            y2rExit();
        }
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
    if (g_y2r) y2rExit();
    g_y2r = false;
    linearFree(g_yuv420);
    g_yuv420 = NULL;
    sws_freeContext(g_mpeg4_scale);
    g_mpeg4_scale = NULL;
    av_frame_free(&g_mpeg4_frame);
    avcodec_free_context(&g_mpeg4);
    jpeg_destroy_decompress(&g_jpeg);
    linearFree(g_pixels);
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

    if (g_y2r &&
        (g_mpeg4_frame->format == AV_PIX_FMT_YUV420P ||
         g_mpeg4_frame->format == AV_PIX_FMT_YUVJ420P)) {
        uint8_t *destination = g_yuv420;
        for (int plane = 0; plane < 3; plane++) {
            const int plane_width = plane == 0 ? VIDEO_WIDTH : VIDEO_WIDTH / 2;
            const int plane_height = plane == 0 ? VIDEO_HEIGHT : VIDEO_HEIGHT / 2;
            for (int row = 0; row < plane_height; row++) {
                memcpy(destination + (size_t)row * plane_width,
                       g_mpeg4_frame->data[plane] +
                           (size_t)row * g_mpeg4_frame->linesize[plane],
                       (size_t)plane_width);
            }
            destination += (size_t)plane_width * plane_height;
        }

        Y2RU_ConversionParams parameters;
        memset(&parameters, 0, sizeof(parameters));
        parameters.input_format = INPUT_YUV420_INDIV_8;
        parameters.output_format = OUTPUT_RGB_16_565;
        parameters.rotation = ROTATION_NONE;
        parameters.block_alignment = BLOCK_LINE;
        parameters.input_line_width = VIDEO_WIDTH;
        parameters.input_lines = VIDEO_HEIGHT;
        parameters.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING;
        parameters.alpha = 0xff;

        Handle finished = 0;
        bool conversion_started = false;
        const uint32_t y_size = VIDEO_WIDTH * VIDEO_HEIGHT;
        const uint32_t uv_size = y_size / 4;
        const uint32_t rgb_size = y_size * sizeof(uint16_t);
        Result result = Y2RU_SetConversionParams(&parameters);
        if (R_SUCCEEDED(result)) {
            result = Y2RU_SetSendingY(g_yuv420, y_size, VIDEO_WIDTH, 0);
        }
        if (R_SUCCEEDED(result)) {
            result = Y2RU_SetSendingU(g_yuv420 + y_size, uv_size,
                                      VIDEO_WIDTH / 2, 0);
        }
        if (R_SUCCEEDED(result)) {
            result = Y2RU_SetSendingV(g_yuv420 + y_size + uv_size, uv_size,
                                      VIDEO_WIDTH / 2, 0);
        }
        if (R_SUCCEEDED(result)) {
            /* Four rows per DMA transfer avoids the documented early
             * completion interrupt at exactly 400x240. */
            result = Y2RU_SetReceiving(g_pixels, rgb_size,
                                       VIDEO_WIDTH * (int)sizeof(uint16_t) * 4, 0);
        }
        if (R_SUCCEEDED(result)) {
            result = Y2RU_StartConversion();
            conversion_started = R_SUCCEEDED(result);
        }
        if (R_SUCCEEDED(result)) result = Y2RU_GetTransferEndEvent(&finished);
        if (R_SUCCEEDED(result)) {
            result = svcWaitSynchronization(finished, 100000000LL);
        }
        if (finished) svcCloseHandle(finished);
        if (R_SUCCEEDED(result)) {
            g_stats.hardware_conversion = true;
            return true;
        }
        if (conversion_started) Y2RU_StopConversion();
    }

    g_stats.hardware_conversion = false;

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
    if (codec != C2S_CODEC_OLD3DS_MPEG4) {
        g_stats.hardware_conversion = false;
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

    /* Y2R and libjpeg both produce row-major pixels.  The 3DS framebuffer
     * is rotated, so transpose here.  Y2R's own rotation mode does not
     * produce the byte layout gfxGetFramebuffer exposes on real hardware. */
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
