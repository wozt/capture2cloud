#include "video.h"
#include "c2s_protocol.h"

#include <3ds.h>

#include <stdio.h>
#include <jpeglib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/cpu.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
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
static bool g_y2r;
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
static volatile bool g_aspect_16_9;
static LightLock g_stats_lock;

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

    /*
     * Do not merely round MPEG dimensions to a macroblock boundary here.
     *
     * FFmpeg's video buffer allocator calls avcodec_align_dimensions2().
     * For YUV420 in this build that means, among other constraints, a
     * vertical alignment of 32 pixels.  A visible 400x240 picture therefore
     * needs backing storage for 256 rows.
     *
     * The returned stride requirements matter as well.  Increase the backing
     * width using the same rule as FFmpeg's default frame pool until every
     * plane stride satisfies its required alignment.
     */
    int width = frame->width;
    int height = frame->height;
    int stride_align[AV_NUM_DATA_POINTERS] = {0};
    int linesize[4] = {0};

    avcodec_align_dimensions2(context, &width, &height, stride_align);

    for (;;) {
        int result = av_image_fill_linesizes(
            linesize, (enum AVPixelFormat)frame->format, width);
        if (result < 0) return result;

        bool unaligned = false;
        for (int i = 0; i < 4; i++) {
            if (linesize[i] && stride_align[i] &&
                linesize[i] % stride_align[i] != 0) {
                unaligned = true;
                break;
            }
        }

        if (!unaligned) break;

        /*
         * Same progression used by FFmpeg's update_frame_pool():
         * add the lowest set bit of the current width.
         */
        width += width & ~(width - 1);
    }

    const int buffer_size =
        av_image_get_buffer_size((enum AVPixelFormat)frame->format,
                                 width, height, 1);
    if (buffer_size <= 0) return AVERROR(EINVAL);

    uint8_t *buffer = linearAlloc((size_t)buffer_size);
    if (!buffer) return AVERROR(ENOMEM);

    memset(buffer, 0, (size_t)buffer_size);

    const int result =
        av_image_fill_arrays(frame->data, frame->linesize, buffer,
                             (enum AVPixelFormat)frame->format,
                             width, height, 1);
    if (result < 0) {
        linearFree(buffer);
        return result;
    }

    /*
     * One AVBufferRef owns the complete contiguous planar allocation.
     * data[1] and data[2] point inside that allocation.
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

    /*
     * This binary only targets Old 3DS/2DS ARM11 MPCore hardware.
     * Do not depend on FFmpeg's Linux-style runtime probing here:
     * explicitly enable the ARMv6 DSP paths used by MPEG motion
     * compensation and IDCT.
     */
    av_force_cpu_flags(AV_CPU_FLAG_ARMV5TE |
                       AV_CPU_FLAG_ARMV6 |
                       AV_CPU_FLAG_VFP);

    g_video_decoder->flags2 |= AV_CODEC_FLAG2_FAST;
    g_video_decoder->idct_algo = FF_IDCT_SIMPLEARMV6;

    /*
     * MPEG-1 is generated by Capture2Cloud with two slices per frame.
     * Decode those slices in parallel.  Keep MPEG-2/MPEG-4 single
     * threaded until their stream layout is explicitly tuned for it.
     */
    if (codec == C2S_CODEC_OLD3DS_MPEG1 ||
        codec == C2S_CODEC_OLD3DS_MPEG2) {
        /*
         * Capture2Cloud emits four slices per picture.  FFmpeg distributes
         * them over two workers, improving load balancing on Old 3DS.
         */
        g_video_decoder->thread_count = 2;
        g_video_decoder->thread_type = FF_THREAD_SLICE;
    } else if (codec == C2S_CODEC_OLD3DS_MPEG4) {
        /*
         * FFmpeg's MPEG-4 Part 2 decoder does not advertise slice threading.
         * It does support frame threading, so use two workers there instead.
         *
         * This may trade a little pipeline latency for throughput; keep it
         * isolated to MPEG-4 so MPEG-1/MPEG-2 retain their low-latency path.
         */
        g_video_decoder->thread_count = 2;
        g_video_decoder->thread_type = FF_THREAD_FRAME;
    } else {
        g_video_decoder->thread_count = 1;
    }

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
    LightLock_Init(&g_stats_lock);
    memset(&g_stats, 0, sizeof(g_stats));

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

    /*
     * Y2R is the 3DS hardware YUV-to-RGB converter.  MPEG frames are already
     * decoded directly into linear memory by video_linear_get_buffer2(), so
     * no additional 144 KiB YUV staging buffer is required.
     */
    if (R_SUCCEEDED(y2rInit())) {
        g_y2r = true;
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

void video_set_aspect_16_9(bool enabled)
{
    g_aspect_16_9 = enabled;
}

static void refresh_rates(uint32_t now)
{
    uint32_t elapsed = now - g_window_start;
    if (elapsed < 1000) return;
    float seconds = elapsed / 1000.0f;
    LightLock_Lock(&g_stats_lock);
    g_stats.receive_fps = g_window_received / seconds;
    g_stats.decode_fps = g_window_decoded / seconds;
    g_stats.display_fps = g_window_displayed / seconds;
    g_stats.bitrate_kbps = (float)(g_window_bytes * 8u) / elapsed;
    if (g_window_decoded) {
        g_stats.decode_ms = (float)g_decode_total_ms / g_window_decoded;
        g_stats.upload_ms = (float)g_upload_total_ms / g_window_decoded;
    }
    LightLock_Unlock(&g_stats_lock);
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

static VideoDecodeResult decode_mpeg_video(const uint8_t *data, uint32_t size,
                                           bool convert)
{
    AVPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.data = (uint8_t *)data;
    packet.size = (int)size;

    if (!g_video_decoder) return VIDEO_DECODE_ERROR;

    /*
     * Frame-threaded codecs may have a decoded frame waiting when
     * avcodec_send_packet() is called.  Drain that frame and retry the same
     * packet rather than dropping it.
     */
    bool have_frame = false;

    int result = avcodec_send_packet(g_video_decoder, &packet);
    if (result == AVERROR(EAGAIN)) {
        result = avcodec_receive_frame(g_video_decoder, g_video_frame);
        if (result == 0) {
            have_frame = true;

            result = avcodec_send_packet(g_video_decoder, &packet);
            if (result < 0)
                return VIDEO_DECODE_ERROR;
        } else {
            /*
             * EAGAIN from both send and receive would mean the API state
             * cannot make progress without losing this packet.
             */
            return VIDEO_DECODE_ERROR;
        }
    } else if (result < 0) {
        return VIDEO_DECODE_ERROR;
    }

    if (!have_frame) {
        result = avcodec_receive_frame(g_video_decoder, g_video_frame);

        /*
         * Normal while a frame-threaded MPEG-4 decoder fills its pipeline.
         * The packet was accepted successfully; there is simply no output
         * picture to present yet.
         */
        if (result == AVERROR(EAGAIN))
            return VIDEO_DECODE_BUFFERED;

        if (result < 0)
            return VIDEO_DECODE_ERROR;
    }

    if (g_video_frame->width != VIDEO_WIDTH ||
        g_video_frame->height != VIDEO_HEIGHT)
        return VIDEO_DECODE_ERROR;

    /*
     * A predictive stream must decode every reference frame, but an old
     * frame need not also pay for Y2R, the RGB write and framebuffer
     * transpose while catching up.
     */
    if (!convert)
        return VIDEO_DECODE_FRAME;

    if (g_y2r &&
        (g_video_frame->format == AV_PIX_FMT_YUV420P ||
         g_video_frame->format == AV_PIX_FMT_YUVJ420P)) {
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

        result = Y2RU_SetConversionParams(&parameters);
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
            result = Y2RU_SetReceiving(
                g_pixels, rgb_size,
                VIDEO_WIDTH * (int)sizeof(uint16_t) * 4, 0);
        }
        if (R_SUCCEEDED(result)) {
            result = Y2RU_StartConversion();
            conversion_started = R_SUCCEEDED(result);
        }
        if (R_SUCCEEDED(result))
            result = Y2RU_GetTransferEndEvent(&finished);
        if (R_SUCCEEDED(result))
            result = svcWaitSynchronization(finished, 100000000LL);

        if (finished) svcCloseHandle(finished);

        if (R_SUCCEEDED(result)) {
            LightLock_Lock(&g_stats_lock);
            g_stats.hardware_conversion = true;
            LightLock_Unlock(&g_stats_lock);
            return VIDEO_DECODE_FRAME;
        }

        if (conversion_started)
            Y2RU_StopConversion();
    }

    LightLock_Lock(&g_stats_lock);
    g_stats.hardware_conversion = false;
    LightLock_Unlock(&g_stats_lock);

    g_video_scale = sws_getCachedContext(
        g_video_scale,
        g_video_frame->width, g_video_frame->height,
        (enum AVPixelFormat)g_video_frame->format,
        VIDEO_WIDTH, VIDEO_HEIGHT, AV_PIX_FMT_RGB565LE,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_video_scale)
        return VIDEO_DECODE_ERROR;

    uint8_t *destination[4] = {
        (uint8_t *)g_pixels, NULL, NULL, NULL
    };
    int destination_stride[4] = {
        VIDEO_WIDTH * (int)sizeof(uint16_t), 0, 0, 0
    };

    if (sws_scale(g_video_scale,
                  (const uint8_t *const *)g_video_frame->data,
                  g_video_frame->linesize, 0, g_video_frame->height,
                  destination, destination_stride) != VIDEO_HEIGHT) {
        return VIDEO_DECODE_ERROR;
    }

    return VIDEO_DECODE_FRAME;
}

VideoDecodeResult video_decode_and_present(const uint8_t *data, uint32_t size,
                                             uint32_t received_ms, uint8_t codec,
                                             bool present)
{
    uint32_t decode_start = osGetTime();
    if (codec != g_last_codec) {
        if (c2s_old3ds_predictive_codec(codec) &&
            !open_video_decoder(codec)) {
            LightLock_Lock(&g_stats_lock);
            g_stats.decode_errors++;
            LightLock_Unlock(&g_stats_lock);
            refresh_rates(osGetTime());
            return VIDEO_DECODE_ERROR;
        }
        g_last_codec = codec;
    }
    if (!c2s_old3ds_predictive_codec(codec)) {
        LightLock_Lock(&g_stats_lock);
        g_stats.hardware_conversion = false;
        LightLock_Unlock(&g_stats_lock);
    }

    VideoDecodeResult decode_result;
    if (c2s_old3ds_predictive_codec(codec)) {
        decode_result = decode_mpeg_video(data, size, present);
    } else {
        decode_result = decode_jpeg(data, size)
            ? VIDEO_DECODE_FRAME
            : VIDEO_DECODE_ERROR;
    }

    if (decode_result == VIDEO_DECODE_ERROR) {
        LightLock_Lock(&g_stats_lock);
        g_stats.decode_errors++;
        LightLock_Unlock(&g_stats_lock);
        refresh_rates(osGetTime());
        return VIDEO_DECODE_ERROR;
    }

    /*
     * The packet was accepted but a frame-threaded decoder may not have
     * emitted a picture yet.  This is normal and must not trigger a keyframe
     * request or a framebuffer swap.
     */
    if (decode_result == VIDEO_DECODE_BUFFERED) {
        refresh_rates(osGetTime());
        return VIDEO_DECODE_BUFFERED;
    }

    uint32_t decode_end = osGetTime();

    g_decode_total_ms += decode_end - decode_start;
    g_window_decoded++;

    LightLock_Lock(&g_stats_lock);
    g_stats.decoded++;
    LightLock_Unlock(&g_stats_lock);
    if (!present) {
        refresh_rates(decode_end);
        return VIDEO_DECODE_FRAME;
    }

    u16 framebuffer_width = 0;
    u16 framebuffer_height = 0;
    uint16_t *framebuffer = (uint16_t *)gfxGetFramebuffer(
        GFX_TOP, GFX_LEFT, &framebuffer_width, &framebuffer_height);
    if (!framebuffer || framebuffer_width < VIDEO_HEIGHT ||
        framebuffer_height < VIDEO_WIDTH) return false;

    /*
     * Y2R and libjpeg both produce 400x240 row-major pixels.  The physical
     * top LCD is 400x240 (5:3), while normal console output is 16:9.
     *
     * FULL preserves the historical Capture2Cloud behaviour and fills the
     * complete LCD.  16:9 restores the source aspect ratio by vertically
     * scaling 240 rows to 225 and centering them, leaving 7 black rows above
     * and 8 below.
     *
     * Nearest-neighbour vertical scaling is intentional: it is extremely
     * cheap on Old 3DS and avoids adding another filtering pass to the video
     * pipeline.  240/225 simplifies exactly to 16/15.
     */
    if (g_aspect_16_9) {
        enum {
            ASPECT_HEIGHT = 225,
            ASPECT_Y = (VIDEO_HEIGHT - ASPECT_HEIGHT) / 2
        };

        /* Clear only the letterbox rows rather than the entire framebuffer. */
        for (int x = 0; x < VIDEO_WIDTH; x++) {
            uint16_t *dst = framebuffer + (size_t)x * framebuffer_width;

            for (int y = 0; y < ASPECT_Y; y++)
                dst[VIDEO_HEIGHT - 1 - y] = 0;

            for (int y = ASPECT_Y + ASPECT_HEIGHT;
                 y < VIDEO_HEIGHT; y++)
                dst[VIDEO_HEIGHT - 1 - y] = 0;
        }

        /*
         * Keep the same cache-friendly 8-row tiling as the normal path.
         * Mapping is:
         *
         *   source_y = destination_y * 240 / 225
         *            = destination_y * 16 / 15
         */
        for (int block_y = 0; block_y < ASPECT_HEIGHT; block_y += 8) {
            const int block_end =
                block_y + 8 < ASPECT_HEIGHT ? block_y + 8 : ASPECT_HEIGHT;

            for (int block_x = 0; block_x < VIDEO_WIDTH; block_x += 8) {
                for (int x = block_x; x < block_x + 8; x++) {
                    uint16_t *dst =
                        framebuffer + (size_t)x * framebuffer_width;

                    for (int y = block_y; y < block_end; y++) {
                        const int source_y = y * 16 / 15;
                        const int screen_y = ASPECT_Y + y;

                        dst[VIDEO_HEIGHT - 1 - screen_y] =
                            g_pixels[(size_t)source_y * VIDEO_WIDTH + x];
                    }
                }
            }
        }
    } else {
        /*
         * Native 400x240 fill.  Work in 8x8 tiles so the source accesses
         * remain cache-friendly while writes stay contiguous in the rotated
         * 3DS framebuffer.
         */
        for (int block_y = 0; block_y < VIDEO_HEIGHT; block_y += 8) {
            for (int block_x = 0; block_x < VIDEO_WIDTH; block_x += 8) {
                for (int x = block_x; x < block_x + 8; x++) {
                    uint16_t *dst =
                        framebuffer + (size_t)x * framebuffer_width;

                    for (int y = block_y; y < block_y + 8; y++) {
                        dst[VIDEO_HEIGHT - 1 - y] =
                            g_pixels[(size_t)y * VIDEO_WIDTH + x];
                    }
                }
            }
        }
    }
    uint32_t upload_end = osGetTime();
    g_upload_total_ms += upload_end - decode_end;
    g_window_displayed++;
    LightLock_Lock(&g_stats_lock);
    g_stats.local_latency_ms = upload_end - received_ms;
    LightLock_Unlock(&g_stats_lock);

    refresh_rates(upload_end);
    return VIDEO_DECODE_FRAME;
}

void video_get_stats(VideoStats *stats)
{
    LightLock_Lock(&g_stats_lock);
    *stats = g_stats;
    LightLock_Unlock(&g_stats_lock);
}
