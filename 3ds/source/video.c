#include "video.h"
#include "c2s_protocol.h"

#include <3ds.h>
#include <citro2d.h>

#include <stdio.h>
#include <jpeglib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_WIDTH 400
#define VIDEO_HEIGHT 240

/*
 * PICA200 textures must be power-of-two sized.  The actual image remains
 * 400x240; Y2R writes each 8-line tiled band into the 512-wide texture and
 * skips the unused 112-pixel tail using transfer_gap.
 */
#define VIDEO_TEX_WIDTH 512
#define VIDEO_TEX_HEIGHT 256

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} DecodeError;

static struct jpeg_decompress_struct g_jpeg;
static DecodeError g_error;
static uint16_t *g_pixels;
static uint8_t *g_yuv420;
static bool g_y2r;

/*
 * MPEG presentation path:
 *
 * FFmpeg YUV420 -> Y2R BLOCK_8_BY_8 -> RGB565 texture -> Citro2D.
 *
 * The texture lives in linear memory so Y2R can DMA straight into it.
 */
static bool g_gpu_video;
static bool g_gpu_frame_ready;
static C3D_Tex g_video_texture;
static C3D_RenderTarget *g_video_target;
static Tex3DS_SubTexture g_video_subtexture;
static C2D_Image g_video_image;

static AVCodecContext *g_video_decoder;
static AVFrame *g_video_frame;
static struct SwsContext *g_video_scale;
static uint8_t g_last_codec;
static VideoStats g_stats;
static uint32_t g_window_start;
static uint32_t g_window_received;
static uint32_t g_window_decoded;
static uint32_t g_window_displayed;
static uint64_t g_window_bytes;
static uint64_t g_decode_total_ms;
static uint64_t g_upload_total_ms;

/*
 * FFmpeg normally allocates decoded pictures from its regular heap.
 * Y2R is a DMA-backed service and performs best with 3DS linear memory.
 *
 * Allocate MPEG decode pictures directly from linear memory so Y2R can
 * consume the decoder's Y/U/V planes without copying the complete
 * 400x240 YUV420 image into a second staging buffer first.
 *
 * The MPEG decoders used here operate on 16x16 macroblocks.  400x240 is
 * already a multiple of 16, but keep the alignment explicit so this
 * callback remains correct if the stream geometry changes later.
 */
static void video_linear_buffer_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    linearFree(data);
}

static int video_linear_get_buffer2(AVCodecContext *context,
                                    AVFrame *frame, int flags)
{
    (void)flags;

    if (frame->format != AV_PIX_FMT_YUV420P &&
        frame->format != AV_PIX_FMT_YUVJ420P) {
        return avcodec_default_get_buffer2(context, frame, flags);
    }

    int width = frame->width;
    int height = frame->height;

    width = (width + 15) & ~15;
    height = (height + 15) & ~15;

    const int buffer_size =
        av_image_get_buffer_size((enum AVPixelFormat)frame->format,
                                 width, height, 1);
    if (buffer_size <= 0) return AVERROR(ENOMEM);

    uint8_t *buffer = linearAlloc((size_t)buffer_size);
    if (!buffer) return AVERROR(ENOMEM);

    memset(buffer, 0, (size_t)buffer_size);

    if (av_image_fill_arrays(frame->data, frame->linesize, buffer,
                             (enum AVPixelFormat)frame->format,
                             width, height, 1) < 0) {
        linearFree(buffer);
        return AVERROR(EINVAL);
    }

    /*
     * A single AVBufferRef owns the complete planar allocation.  data[1]
     * and data[2] simply point inside it; they do not need separate owners.
     */
    frame->buf[0] =
        av_buffer_create(buffer, (size_t)buffer_size,
                         video_linear_buffer_free, NULL, 0);
    if (!frame->buf[0]) {
        linearFree(buffer);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static enum AVCodecID decoder_id(uint8_t codec)
{
    if (codec == C2S_CODEC_OLD3DS_MPEG1) return AV_CODEC_ID_MPEG1VIDEO;
    if (codec == C2S_CODEC_OLD3DS_MPEG2) return AV_CODEC_ID_MPEG2VIDEO;
    return AV_CODEC_ID_MPEG4;
}

static bool open_video_decoder(uint8_t codec)
{
    const AVCodec *decoder = avcodec_find_decoder(decoder_id(codec));
    if (!decoder) return false;
    avcodec_free_context(&g_video_decoder);
    g_video_decoder = avcodec_alloc_context3(decoder);
    if (!g_video_decoder) return false;
    g_video_decoder->flags2 |= AV_CODEC_FLAG2_FAST;

    /*
     * Decode directly into DMA-friendly linear memory.  This removes the
     * old full-frame YUV420 copy before every Y2R conversion.
     */
    g_video_decoder->get_buffer2 = video_linear_get_buffer2;

    if (avcodec_open2(g_video_decoder, decoder, NULL) < 0) {
        avcodec_free_context(&g_video_decoder);
        return false;
    }
    return true;
}

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

    g_video_frame = av_frame_alloc();
    if (!g_video_frame ||
        !avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO) ||
        !avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO) ||
        !avcodec_find_decoder(AV_CODEC_ID_MPEG4)) {
        av_frame_free(&g_video_frame);
        jpeg_destroy_decompress(&g_jpeg);
        linearFree(g_pixels);
        g_pixels = NULL;
        return false;
    }

    /* Y2R is the 3DS' hardware YUV-to-RGB converter.  MPEG video still
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

    /*
     * Create a tiny GPU presentation pipeline for the top screen.
     *
     * C3D_TexInit() uses linear memory by default, which is exactly what
     * Y2R needs for DMA output.  Y2R will fill only the 400x240 visible
     * portion; the remainder is texture padding.
     */
    if (g_y2r &&
        C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) &&
        C2D_Init(32)) {
        C2D_Prepare();

        g_video_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

        if (g_video_target &&
            C3D_TexInit(&g_video_texture,
                        VIDEO_TEX_WIDTH, VIDEO_TEX_HEIGHT,
                        GPU_RGB565)) {
            C3D_TexSetFilter(&g_video_texture, GPU_NEAREST, GPU_NEAREST);

            memset(g_video_texture.data, 0,
                   VIDEO_TEX_WIDTH * VIDEO_TEX_HEIGHT * sizeof(uint16_t));

            /*
             * Texture coordinates are vertically inverted in Citro2D.
             * Y2R writes starting at the first tiled row of the texture.
             */
            g_video_subtexture.width = VIDEO_WIDTH;
            g_video_subtexture.height = VIDEO_HEIGHT;
            g_video_subtexture.left = 0.0f;
            g_video_subtexture.right =
                (float)VIDEO_WIDTH / (float)VIDEO_TEX_WIDTH;
            g_video_subtexture.top =
                (float)VIDEO_HEIGHT / (float)VIDEO_TEX_HEIGHT;
            g_video_subtexture.bottom = 0.0f;

            g_video_image.tex = &g_video_texture;
            g_video_image.subtex = &g_video_subtexture;

            g_gpu_video = true;
        } else {
            C2D_Fini();
            C3D_Fini();
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
    if (g_gpu_video) {
        C3D_TexDelete(&g_video_texture);
        g_gpu_video = false;
        g_gpu_frame_ready = false;
        C2D_Fini();
        C3D_Fini();
    }

    if (g_y2r) y2rExit();
    g_y2r = false;
    linearFree(g_yuv420);
    g_yuv420 = NULL;
    sws_freeContext(g_video_scale);
    g_video_scale = NULL;
    av_frame_free(&g_video_frame);
    avcodec_free_context(&g_video_decoder);
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

static bool decode_mpeg_video(const uint8_t *data, uint32_t size, bool convert)
{
    AVPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.data = (uint8_t *)data;
    packet.size = (int)size;
    if (!g_video_decoder ||
        avcodec_send_packet(g_video_decoder, &packet) < 0 ||
        avcodec_receive_frame(g_video_decoder, g_video_frame) < 0) {
        return false;
    }
    if (g_video_frame->width != VIDEO_WIDTH ||
        g_video_frame->height != VIDEO_HEIGHT) return false;

    /* A predictive stream must decode every reference frame, but an old
     * frame need not also pay for Y2R, a 192 KiB RGB write and a framebuffer
     * transpose.  The caller uses this when catching up to the live edge. */
    if (!convert) return true;

    if (g_y2r &&
        (g_video_frame->format == AV_PIX_FMT_YUV420P ||
         g_video_frame->format == AV_PIX_FMT_YUVJ420P)) {
        Y2RU_ConversionParams parameters;
        memset(&parameters, 0, sizeof(parameters));
        parameters.input_format = INPUT_YUV420_INDIV_8;
        parameters.output_format = OUTPUT_RGB_16_565;
        parameters.rotation = ROTATION_NONE;

        /*
         * MPEG frames go directly into PICA200 texture layout.
         * JPEG/software fallbacks continue to use the old linear RGB path.
         */
        parameters.block_alignment =
            g_gpu_video ? BLOCK_8_BY_8 : BLOCK_LINE;
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
            result = Y2RU_SetSendingY(
                g_video_frame->data[0], y_size,
                VIDEO_WIDTH,
                g_video_frame->linesize[0] - VIDEO_WIDTH);
        }
        if (R_SUCCEEDED(result)) {
            result = Y2RU_SetSendingU(
                g_video_frame->data[1], uv_size,
                VIDEO_WIDTH / 2,
                g_video_frame->linesize[1] - VIDEO_WIDTH / 2);
        }
        if (R_SUCCEEDED(result)) {
            result = Y2RU_SetSendingV(
                g_video_frame->data[2], uv_size,
                VIDEO_WIDTH / 2,
                g_video_frame->linesize[2] - VIDEO_WIDTH / 2);
        }
        if (R_SUCCEEDED(result)) {
            if (g_gpu_video) {
                /*
                 * BLOCK_8_BY_8 is already the native texture layout.
                 *
                 * One transfer is one 8-line band:
                 *   visible: 400 * 8 * 2 = 6400 bytes
                 *   texture: 512 * 8 * 2 = 8192 bytes
                 *
                 * The 1792-byte gap advances DMA to the next texture band.
                 */
                const int transfer_unit =
                    VIDEO_WIDTH * (int)sizeof(uint16_t) * 8;
                const int transfer_gap =
                    (VIDEO_TEX_WIDTH - VIDEO_WIDTH) *
                    (int)sizeof(uint16_t) * 8;

                result = Y2RU_SetReceiving(
                    g_video_texture.data, rgb_size,
                    transfer_unit, transfer_gap);
            } else {
                /*
                 * Four rows per DMA transfer avoids the documented early
                 * completion interrupt at exactly 400x240.
                 */
                result = Y2RU_SetReceiving(
                    g_pixels, rgb_size,
                    VIDEO_WIDTH * (int)sizeof(uint16_t) * 4, 0);
            }
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

            if (g_gpu_video) {
                /*
                 * Y2R DMA has written the texture.  Invalidate any stale CPU
                 * cache lines before the GPU samples it.
                 */
                GSPGPU_InvalidateDataCache(
                    g_video_texture.data,
                    VIDEO_TEX_WIDTH * VIDEO_TEX_HEIGHT *
                        sizeof(uint16_t));

                g_gpu_frame_ready = true;
            }

            return true;
        }
        if (conversion_started) Y2RU_StopConversion();
    }

    g_stats.hardware_conversion = false;
    g_gpu_frame_ready = false;

    g_video_scale = sws_getCachedContext(
        g_video_scale,
        g_video_frame->width, g_video_frame->height,
        (enum AVPixelFormat)g_video_frame->format,
        VIDEO_WIDTH, VIDEO_HEIGHT, AV_PIX_FMT_RGB565LE,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_video_scale) return false;
    uint8_t *destination[4] = {(uint8_t *)g_pixels, NULL, NULL, NULL};
    int destination_stride[4] = {VIDEO_WIDTH * (int)sizeof(uint16_t), 0, 0, 0};
    return sws_scale(g_video_scale,
                     (const uint8_t *const *)g_video_frame->data,
                     g_video_frame->linesize, 0, g_video_frame->height,
                     destination, destination_stride) == VIDEO_HEIGHT;
}

bool video_decode_and_present(const uint8_t *data, uint32_t size,
                              uint32_t received_ms, uint8_t codec,
                              bool present)
{
    uint32_t decode_start = osGetTime();
    if (codec != g_last_codec) {
        if (c2s_old3ds_predictive_codec(codec) &&
            !open_video_decoder(codec)) {
            g_stats.decode_errors++;
            refresh_rates(osGetTime());
            return false;
        }
        g_last_codec = codec;
    }
    if (!c2s_old3ds_predictive_codec(codec)) {
        g_stats.hardware_conversion = false;
    }
    const bool decoded = c2s_old3ds_predictive_codec(codec)
        ? decode_mpeg_video(data, size, present) : decode_jpeg(data, size);
    if (!decoded) {
        g_stats.decode_errors++;
        refresh_rates(osGetTime());
        return false;
    }
    uint32_t decode_end = osGetTime();

    g_decode_total_ms += decode_end - decode_start;
    g_window_decoded++;
    g_stats.decoded++;
    if (!present) {
        refresh_rates(decode_end);
        return true;
    }

    /*
     * MPEG + Y2R texture path is already completely prepared for the GPU.
     * Do not touch 96,000 pixels on ARM11 just to transpose them again.
     */
    if (g_gpu_frame_ready) {
        uint32_t upload_end = osGetTime();
        g_upload_total_ms += upload_end - decode_end;
        g_window_displayed++;
        g_stats.local_latency_ms = upload_end - received_ms;
        refresh_rates(upload_end);
        return true;
    }

    u16 framebuffer_width = 0;
    u16 framebuffer_height = 0;
    uint16_t *framebuffer = (uint16_t *)gfxGetFramebuffer(
        GFX_TOP, GFX_LEFT, &framebuffer_width, &framebuffer_height);
    if (!framebuffer || framebuffer_width < VIDEO_HEIGHT ||
        framebuffer_height < VIDEO_WIDTH) return false;

    /* Y2R and libjpeg both produce row-major pixels.  The 3DS framebuffer
     * is rotated, so transpose here.  Work in 8x8 tiles: the old full-column
     * walk jumped 800 bytes between every source pixel and repeatedly missed
     * the tiny Old 3DS data cache.  A tile loads eight source rows once while
     * retaining contiguous writes on each framebuffer row. */
    for (int block_y = 0; block_y < VIDEO_HEIGHT; block_y += 8) {
        for (int block_x = 0; block_x < VIDEO_WIDTH; block_x += 8) {
            for (int x = block_x; x < block_x + 8; x++) {
                uint16_t *dst = framebuffer + (size_t)x * framebuffer_width;
                for (int y = block_y; y < block_y + 8; y++) {
                    dst[VIDEO_HEIGHT - 1 - y] =
                        g_pixels[(size_t)y * VIDEO_WIDTH + x];
                }
            }
        }
    }
    uint32_t upload_end = osGetTime();
    g_upload_total_ms += upload_end - decode_end;
    g_window_displayed++;
    g_stats.local_latency_ms = upload_end - received_ms;
    refresh_rates(upload_end);
    return true;
}

bool video_present_gpu(void)
{
    if (!g_gpu_video || !g_gpu_frame_ready || !g_video_target) {
        return false;
    }

    /*
     * Citro2D handles the physical sideways framebuffer layout for us.
     * The actual video texture already contains native PICA200 8x8 tiles.
     */
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(g_video_target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(g_video_target);
    C2D_DrawImageAt(g_video_image, 0.0f, 0.0f, 0.0f,
                    NULL, 1.0f, 1.0f);
    C3D_FrameEnd(0);

    g_gpu_frame_ready = false;
    return true;
}

void video_get_stats(VideoStats *stats) { *stats = g_stats; }
