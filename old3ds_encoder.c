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
    unsigned int jpeg_under_budget_frames;
    int bitrate_kbps;
    uint8_t codec;
    AVCodecContext *video;
    AVFrame *video_frame;
    AVPacket *video_packet;
    int64_t video_pts;
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

static void close_video(Old3dsEncoder *encoder)
{
    av_packet_free(&encoder->video_packet);
    av_frame_free(&encoder->video_frame);
    avcodec_free_context(&encoder->video);
    encoder->video_pts = 0;
}

static enum AVCodecID codec_id(uint8_t codec)
{
    if (codec == C2S_CODEC_OLD3DS_MPEG1) return AV_CODEC_ID_MPEG1VIDEO;
    if (codec == C2S_CODEC_OLD3DS_MPEG2) return AV_CODEC_ID_MPEG2VIDEO;
    return AV_CODEC_ID_MPEG4;
}

static int open_video(Old3dsEncoder *encoder)
{
    const AVCodec *codec = avcodec_find_encoder(codec_id(encoder->codec));
    if (!codec) return 0;
    encoder->video = avcodec_alloc_context3(codec);
    encoder->video_frame = av_frame_alloc();
    encoder->video_packet = av_packet_alloc();
    if (!encoder->video || !encoder->video_frame || !encoder->video_packet) {
        close_video(encoder);
        return 0;
    }
    encoder->video->width = OLD3DS_VIDEO_WIDTH;
    encoder->video->height = OLD3DS_VIDEO_HEIGHT;
    encoder->video->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->video->time_base = (AVRational){1, OLD3DS_VIDEO_FPS};
    encoder->video->framerate = (AVRational){OLD3DS_VIDEO_FPS, 1};
    encoder->video->bit_rate = (int64_t)encoder->bitrate_kbps * 1000;
    /* The MPEG encoders otherwise inherit a very loose 4 Mbit/s tolerance.
     * At the Old 3DS' sub-megabit target that creates short bursts large
     * enough to fill its TCP receive path even though the long-term average
     * looks correct.  Keep the rate controller close to the negotiated rate. */
    encoder->video->bit_rate_tolerance =
        (int)((encoder->video->bit_rate + 7) / 8);
    /* Periodic I-frames are much larger than P-frames on this profile.
     * Transport-side loss detection requests an immediate recovery frame. */
    encoder->video->gop_size = OLD3DS_VIDEO_FPS * 10;
    encoder->video->max_b_frames = 0;
    encoder->video->thread_count = 1;

    /*
     * Old 3DS MPEG-1 profile:
     *
     * Prefer a simpler, more heavily quantised bitstream over squeezing
     * every possible detail into the negotiated bitrate.  On the ARM11
     * decoder, high-motion frames are the expensive case; fewer residual
     * coefficients matter more than compression efficiency.
     *
     * Keep this MPEG-1-specific so MPEG-2/MPEG-4 remain useful comparison
     * modes from the client menu.
     */
    if (encoder->codec == C2S_CODEC_OLD3DS_MPEG1) {
        encoder->video->qmin = 8;
        encoder->video->qmax = 24;
        encoder->video->max_qdiff = 3;
        encoder->video->qcompress = 0.5f;
        encoder->video->mb_decision = FF_MB_DECISION_SIMPLE;

        /*
         * Split each 400x240 MPEG-1 picture into two independent slices.
         * The Old 3DS decoder can then reconstruct both halves in
         * parallel on its two ARM11 cores.
         *
         * This changes packetisation inside the MPEG picture, not the
         * visual quality or the Capture2Cloud wire protocol.
         */
        encoder->video->slices = 4;
    }
    /* max_b_frames=0 removes reordering latency for all three MPEG
     * comparison codecs without relying on codec-specific flags. */
    if (avcodec_open2(encoder->video, codec, NULL) < 0) {
        close_video(encoder);
        return 0;
    }
    encoder->video_frame->format = AV_PIX_FMT_YUV420P;
    encoder->video_frame->width = OLD3DS_VIDEO_WIDTH;
    encoder->video_frame->height = OLD3DS_VIDEO_HEIGHT;
    if (av_frame_get_buffer(encoder->video_frame, 32) < 0) {
        close_video(encoder);
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
    encoder->quality = 45;
    encoder->bitrate_kbps = OLD3DS_VIDEO_BITRATE_KBPS;
    encoder->codec = C2S_CODEC_OLD3DS_MPEG1;
    encoder->source_format = AV_PIX_FMT_NONE;
    return encoder;
}

void old3ds_encoder_destroy(Old3dsEncoder *encoder)
{
    if (!encoder) return;
    jpeg_destroy_compress(&encoder->jpeg);
    close_video(encoder);
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
    int quality = 20 + bitrate_kbps / 40;
    if (quality < 18) quality = 18;
    if (quality > 65) quality = 65;
    encoder->quality = quality;
    encoder->jpeg_under_budget_frames = 0;
    if (encoder->bitrate_kbps != bitrate_kbps) {
        encoder->bitrate_kbps = bitrate_kbps;
        close_video(encoder);
    }
}

void old3ds_encoder_set_codec(Old3dsEncoder *encoder, uint8_t codec)
{
    if (!encoder ||
        !c2s_old3ds_video_codec(codec) ||
        encoder->codec == codec) return;
    encoder->codec = codec;
    if (encoder->scale) {
        sws_freeContext(encoder->scale);
        encoder->scale = NULL;
    }
    /* Re-entering MPEG video must begin with an I-frame; keeping the old
     * prediction chain would make a freshly flushed client undecodable. */
    close_video(encoder);
}

void old3ds_encoder_request_keyframe(Old3dsEncoder *encoder)
{
    if (encoder && c2s_old3ds_predictive_codec(encoder->codec)) {
        close_video(encoder);
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
        c2s_old3ds_predictive_codec(encoder->codec)
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

    if (c2s_old3ds_predictive_codec(encoder->codec) &&
        !encoder->video && !open_video(encoder)) return 0;

    uint8_t *dst[4] = {encoder->rgb, NULL, NULL, NULL};
    int dst_stride[4] = {OLD3DS_VIDEO_WIDTH * 3, 0, 0, 0};
    if (c2s_old3ds_predictive_codec(encoder->codec)) {
        if (av_frame_make_writable(encoder->video_frame) < 0) return 0;
        for (int i = 0; i < 4; i++) {
            dst[i] = encoder->video_frame->data[i];
            dst_stride[i] = encoder->video_frame->linesize[i];
        }
    }
    const uint8_t *src[4] = {planes[0], planes[1], planes[2], NULL};
    int src_stride[4] = {strides[0], strides[1], strides[2], 0};
    if (sws_scale(encoder->scale, src, src_stride, 0, height,
                  dst, dst_stride) != OLD3DS_VIDEO_HEIGHT) return 0;

    if (c2s_old3ds_predictive_codec(encoder->codec)) {
        av_packet_unref(encoder->video_packet);
        encoder->video_frame->pts = encoder->video_pts++;
        if (avcodec_send_frame(encoder->video, encoder->video_frame) < 0 ||
            avcodec_receive_packet(encoder->video, encoder->video_packet) < 0) {
            return 0;
        }
        *output = encoder->video_packet->data;
        *output_size = (uint32_t)encoder->video_packet->size;
        *keyframe = (encoder->video_packet->flags & AV_PKT_FLAG_KEY) != 0;
        return *output_size > 0;
    }

    unsigned long jpeg_size = 0;
    const unsigned long target_bytes = (unsigned long)(
        ((uint64_t)encoder->bitrate_kbps * 1000u) /
        (8u * OLD3DS_VIDEO_FPS));
    int quality = encoder->quality;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (setjmp(encoder->error.jump)) {
            jpeg_abort_compress(&encoder->jpeg);
            return 0;
        }

        jpeg_size = encoder->output_capacity;
        encoder->jpeg.image_width = OLD3DS_VIDEO_WIDTH;
        encoder->jpeg.image_height = OLD3DS_VIDEO_HEIGHT;
        encoder->jpeg.input_components = 3;
        encoder->jpeg.in_color_space = JCS_RGB;
        jpeg_set_defaults(&encoder->jpeg);
        /* 4:2:0 plus optimized Huffman tables saves WLAN bandwidth without
         * adding work to the decoder.  The host has ample CPU for this. */
        encoder->jpeg.dct_method = JDCT_FASTEST;
        encoder->jpeg.optimize_coding = TRUE;
        jpeg_set_quality(&encoder->jpeg, quality, TRUE);
        jpeg_mem_dest(&encoder->jpeg, &encoder->output, &jpeg_size);
        jpeg_start_compress(&encoder->jpeg, TRUE);

        while (encoder->jpeg.next_scanline < encoder->jpeg.image_height) {
            JSAMPROW row = encoder->rgb +
                (size_t)encoder->jpeg.next_scanline * OLD3DS_VIDEO_WIDTH * 3u;
            jpeg_write_scanlines(&encoder->jpeg, &row, 1);
        }
        jpeg_finish_compress(&encoder->jpeg);

        /* Re-encode an exceptional frame immediately instead of allowing
         * one large picture to create several hundred milliseconds of TCP
         * backlog.  Two retries are cheap at 400x240 and occur on the host. */
        if (!target_bytes || jpeg_size <= target_bytes * 115u / 100u ||
            quality <= 18) break;
        int drop = (int)(((jpeg_size - target_bytes) * (unsigned long)quality) /
                         (jpeg_size * 2u));
        if (drop < 2) drop = 2;
        quality -= drop;
        if (quality < 18) quality = 18;
    }

    encoder->quality = quality;
    if (target_bytes && jpeg_size < target_bytes * 75u / 100u) {
        encoder->jpeg_under_budget_frames++;
        if (encoder->jpeg_under_budget_frames >= OLD3DS_VIDEO_FPS &&
            encoder->quality < 65) {
            encoder->quality++;
            encoder->jpeg_under_budget_frames = 0;
        }
    } else {
        encoder->jpeg_under_budget_frames = 0;
    }
    if (jpeg_size == 0 || jpeg_size > UINT32_MAX) return 0;
    *output = encoder->output;
    *output_size = (uint32_t)jpeg_size;
    *keyframe = 1;
    return 1;
}
