#include "old3ds_encoder.h"
#include "c2s_protocol.h"

#include <stdio.h>
#include <jpeglib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} Old3dsJpegError;

struct Old3dsEncoder {
    struct jpeg_compress_struct jpeg;
    Old3dsJpegError error;
    struct SwsContext *scale;
    int source_width;
    int source_height;
    enum AVPixelFormat source_format;
    uint8_t *rgb;
    unsigned char *output;
    unsigned long output_capacity;
    int quality;
    int bitrate_kbps;
    uint8_t codec;
    AVCodecContext *mpeg4;
    AVFrame *mpeg4_frame;
    AVPacket *mpeg4_packet;
    int64_t mpeg4_pts;
};

static void jpeg_failed(j_common_ptr jpeg)
{
    Old3dsJpegError *error = (Old3dsJpegError *)jpeg->err;
    longjmp(error->jump, 1);
}

static int initialize_jpeg(Old3dsEncoder *encoder)
{
    if (setjmp(encoder->error.jump)) return 0;
    jpeg_create_compress(&encoder->jpeg);
    return 1;
}

static void close_mpeg4(Old3dsEncoder *encoder)
{
    av_packet_free(&encoder->mpeg4_packet);
    av_frame_free(&encoder->mpeg4_frame);
    avcodec_free_context(&encoder->mpeg4);
    encoder->mpeg4_pts = 0;
}

static int open_mpeg4(Old3dsEncoder *encoder)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec) return 0;
    encoder->mpeg4 = avcodec_alloc_context3(codec);
    encoder->mpeg4_frame = av_frame_alloc();
    encoder->mpeg4_packet = av_packet_alloc();
    if (!encoder->mpeg4 || !encoder->mpeg4_frame || !encoder->mpeg4_packet) {
        close_mpeg4(encoder);
        return 0;
    }
    encoder->mpeg4->width = OLD3DS_VIDEO_WIDTH;
    encoder->mpeg4->height = OLD3DS_VIDEO_HEIGHT;
    encoder->mpeg4->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->mpeg4->time_base = (AVRational){1, OLD3DS_VIDEO_FPS};
    encoder->mpeg4->framerate = (AVRational){OLD3DS_VIDEO_FPS, 1};
    encoder->mpeg4->bit_rate = (int64_t)encoder->bitrate_kbps * 1000;
    /* A small recovery interval matters more than compression ratio on
     * this client: if its tiny decode queue ever overflows, the next
     * independent frame is at most a third of a second away. */
    encoder->mpeg4->gop_size = OLD3DS_VIDEO_FPS / 3;
    encoder->mpeg4->max_b_frames = 0;
    encoder->mpeg4->thread_count = 1;
    /* MPEG-4 Part 2 rejects AV_CODEC_FLAG_LOW_DELAY (FFmpeg only
     * permits that flag for MPEG-2).  max_b_frames=0 is the setting
     * that actually removes reordering latency for this encoder. */
    if (avcodec_open2(encoder->mpeg4, codec, NULL) < 0) {
        close_mpeg4(encoder);
        return 0;
    }
    encoder->mpeg4_frame->format = AV_PIX_FMT_YUV420P;
    encoder->mpeg4_frame->width = OLD3DS_VIDEO_WIDTH;
    encoder->mpeg4_frame->height = OLD3DS_VIDEO_HEIGHT;
    if (av_frame_get_buffer(encoder->mpeg4_frame, 32) < 0) {
        close_mpeg4(encoder);
        return 0;
    }
    return 1;
}

Old3dsEncoder *old3ds_encoder_create(void)
{
    Old3dsEncoder *encoder = calloc(1, sizeof(*encoder));
    if (!encoder) return NULL;

    encoder->rgb = malloc((size_t)OLD3DS_VIDEO_WIDTH * OLD3DS_VIDEO_HEIGHT * 3u);
    encoder->output_capacity = 512u * 1024u;
    encoder->output = malloc(encoder->output_capacity);
    if (!encoder->rgb || !encoder->output) {
        free(encoder->output);
        free(encoder->rgb);
        free(encoder);
        return NULL;
    }

    encoder->jpeg.err = jpeg_std_error(&encoder->error.base);
    encoder->error.base.error_exit = jpeg_failed;
    if (!initialize_jpeg(encoder)) {
        free(encoder->rgb);
        free(encoder->output);
        free(encoder);
        return NULL;
    }
    encoder->quality = 55;
    encoder->bitrate_kbps = OLD3DS_VIDEO_BITRATE_KBPS;
    encoder->codec = C2S_CODEC_OLD3DS_JPEG;
    encoder->source_format = AV_PIX_FMT_NONE;
    return encoder;
}

void old3ds_encoder_destroy(Old3dsEncoder *encoder)
{
    if (!encoder) return;
    jpeg_destroy_compress(&encoder->jpeg);
    close_mpeg4(encoder);
    if (encoder->scale) sws_freeContext(encoder->scale);
    free(encoder->output);
    free(encoder->rgb);
    free(encoder);
}

void old3ds_encoder_set_bitrate(Old3dsEncoder *encoder, int bitrate_kbps)
{
    if (!encoder || bitrate_kbps <= 0) return;
    /* JPEG has no rate controller.  This monotonic mapping makes the
     * existing profile control useful while keeping CPU cost bounded. */
    int quality = 35 + bitrate_kbps / 60;
    if (quality < 35) quality = 35;
    if (quality > 80) quality = 80;
    encoder->quality = quality;
    if (encoder->bitrate_kbps != bitrate_kbps) {
        encoder->bitrate_kbps = bitrate_kbps;
        close_mpeg4(encoder);
    }
}

void old3ds_encoder_set_codec(Old3dsEncoder *encoder, uint8_t codec)
{
    if (!encoder ||
        (codec != C2S_CODEC_OLD3DS_JPEG &&
         codec != C2S_CODEC_OLD3DS_MPEG4) ||
        encoder->codec == codec) return;
    encoder->codec = codec;
    if (encoder->scale) {
        sws_freeContext(encoder->scale);
        encoder->scale = NULL;
    }
    /* Re-entering MPEG-4 must begin with an I-frame; keeping the old
     * prediction chain would make a freshly flushed client undecodable. */
    close_mpeg4(encoder);
}

void old3ds_encoder_request_keyframe(Old3dsEncoder *encoder)
{
    if (encoder && encoder->codec == C2S_CODEC_OLD3DS_MPEG4) {
        close_mpeg4(encoder);
    }
}

int old3ds_encoder_encode(Old3dsEncoder *encoder,
                          const uint8_t *const planes[3],
                          const int strides[3],
                          int av_pixel_format,
                          int width,
                          int height,
                          const uint8_t **output,
                          uint32_t *output_size,
                          int *keyframe)
{
    if (!encoder || !planes || !planes[0] || !output || !output_size || !keyframe ||
        width <= 0 || height <= 0) return 0;

    const enum AVPixelFormat format = (enum AVPixelFormat)av_pixel_format;
    const enum AVPixelFormat destination_format =
        encoder->codec == C2S_CODEC_OLD3DS_MPEG4
            ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_RGB24;
    if (!encoder->scale || encoder->source_width != width ||
        encoder->source_height != height || encoder->source_format != format) {
        if (encoder->scale) sws_freeContext(encoder->scale);
        encoder->scale = sws_getContext(width, height, format,
                                        OLD3DS_VIDEO_WIDTH, OLD3DS_VIDEO_HEIGHT,
                                        destination_format,
                                        SWS_LANCZOS | SWS_ACCURATE_RND,
                                        NULL, NULL, NULL);
        if (!encoder->scale) return 0;
        encoder->source_width = width;
        encoder->source_height = height;
        encoder->source_format = format;
    }

    if (encoder->codec == C2S_CODEC_OLD3DS_MPEG4 &&
        !encoder->mpeg4 && !open_mpeg4(encoder)) return 0;

    uint8_t *dst[4] = {encoder->rgb, NULL, NULL, NULL};
    int dst_stride[4] = {OLD3DS_VIDEO_WIDTH * 3, 0, 0, 0};
    if (encoder->codec == C2S_CODEC_OLD3DS_MPEG4) {
        if (av_frame_make_writable(encoder->mpeg4_frame) < 0) return 0;
        for (int i = 0; i < 4; i++) {
            dst[i] = encoder->mpeg4_frame->data[i];
            dst_stride[i] = encoder->mpeg4_frame->linesize[i];
        }
    }
    const uint8_t *src[4] = {planes[0], planes[1], planes[2], NULL};
    int src_stride[4] = {strides[0], strides[1], strides[2], 0};
    if (sws_scale(encoder->scale, src, src_stride, 0, height,
                  dst, dst_stride) != OLD3DS_VIDEO_HEIGHT) return 0;

    if (encoder->codec == C2S_CODEC_OLD3DS_MPEG4) {
        av_packet_unref(encoder->mpeg4_packet);
        encoder->mpeg4_frame->pts = encoder->mpeg4_pts++;
        if (avcodec_send_frame(encoder->mpeg4, encoder->mpeg4_frame) < 0 ||
            avcodec_receive_packet(encoder->mpeg4, encoder->mpeg4_packet) < 0) {
            return 0;
        }
        *output = encoder->mpeg4_packet->data;
        *output_size = (uint32_t)encoder->mpeg4_packet->size;
        *keyframe = (encoder->mpeg4_packet->flags & AV_PKT_FLAG_KEY) != 0;
        return *output_size > 0;
    }

    if (setjmp(encoder->error.jump)) {
        jpeg_abort_compress(&encoder->jpeg);
        return 0;
    }

    unsigned long jpeg_size = encoder->output_capacity;
    encoder->jpeg.image_width = OLD3DS_VIDEO_WIDTH;
    encoder->jpeg.image_height = OLD3DS_VIDEO_HEIGHT;
    encoder->jpeg.input_components = 3;
    encoder->jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder->jpeg);
    /* Keep libjpeg's 4:2:0 sampling.  The previous 4:4:4 override made
     * every frame much larger and materially more expensive to decode;
     * the mature 3DS video-player benchmark that reaches 30 FPS uses
     * ordinary 4:2:0 JPEG/MPEG video. */
    encoder->jpeg.dct_method = JDCT_FASTEST;
    encoder->jpeg.optimize_coding = FALSE;
    jpeg_set_quality(&encoder->jpeg, encoder->quality, TRUE);
    jpeg_mem_dest(&encoder->jpeg, &encoder->output, &jpeg_size);
    jpeg_start_compress(&encoder->jpeg, TRUE);

    while (encoder->jpeg.next_scanline < encoder->jpeg.image_height) {
        JSAMPROW row = encoder->rgb +
            (size_t)encoder->jpeg.next_scanline * OLD3DS_VIDEO_WIDTH * 3u;
        jpeg_write_scanlines(&encoder->jpeg, &row, 1);
    }
    jpeg_finish_compress(&encoder->jpeg);
    if (jpeg_size == 0 || jpeg_size > UINT32_MAX) return 0;
    *output = encoder->output;
    *output_size = (uint32_t)jpeg_size;
    *keyframe = 1;
    return 1;
}
