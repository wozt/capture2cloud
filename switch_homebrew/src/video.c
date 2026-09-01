#include "video.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>

#define VPX_CODEC_DISABLE_COMPAT 1
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

/*
 * Two decoders, one interface.
 *
 * The Tegra X1 has NVDEC, a dedicated video decode block, and the ffmpeg
 * that devkitPro ships carries averne's nvtegra backend for it --
 * `vp8_nvtegra` among others. Decoding in software on three A57 cores
 * while that sits idle is the reason this could not hold 30 fps at 360p.
 *
 * libvpx stays as the fallback, because a client that shows nothing is
 * worse than one that shows a slow picture, and there is no way to know
 * from here whether every firmware exposes the engine.
 */

typedef enum { DECODER_NONE, DECODER_NVDEC, DECODER_SOFTWARE } DecoderKind;

static DecoderKind g_kind = DECODER_NONE;

/* --- hardware --- */
static AVCodecContext *g_avctx = NULL;
static AVBufferRef *g_hw_device = NULL;
static AVPacket *g_packet = NULL;
static AVFrame *g_hw_frame = NULL;
static AVFrame *g_sw_frame = NULL;

/* --- software fallback --- */
static vpx_codec_ctx_t g_vpx;
static int g_vpx_ready = 0;
static enum AVCodecID g_codec_id = AV_CODEC_ID_VP8;

static SDL_Renderer *g_renderer = NULL;
static SDL_Texture *g_texture = NULL;
static int g_texture_is_nv12 = 0;
static int g_width = 0, g_height = 0;
static int g_have_picture = 0;

static unsigned long g_decoded = 0, g_failed = 0;

/* --- picture adjustments ---------------------------------------------
 *
 * The same four the browser page offers, split by what each one costs.
 *
 * Brightness and contrast are per-channel arithmetic, so the renderer
 * does them: a colour modulation and at most two more passes over a
 * quad, which on this hardware is free. Saturation and hue are NOT
 * per-channel -- they mix colour channels into each other -- and no
 * blend mode can express that.
 *
 * But the picture arrives as YUV, where both are a plain 2x2 matrix on
 * the two chroma bytes and nothing else. That is a quarter of the data
 * of a full-frame pass, and the two settings collapse into ONE matrix,
 * so using both costs exactly what using one costs. Measured on the
 * chroma plane at 720p60 it comes to around 15% of a core -- worth
 * having, not worth paying for when it is not wanted, which is why the
 * pass is skipped entirely while both sit at their defaults. */
static int g_brightness = 100; /* percent, 50..150 */
static int g_contrast = 100;   /* percent, 50..150 */
static int g_saturation = 100; /* percent, 0..200 */
static int g_hue = 0;          /* degrees, -180..180 */

/* The 2x2 matrix, in 8.8 fixed point, rebuilt only when a setting
 * changes rather than per frame. */
static int g_cm[4] = {256, 0, 0, 256};
/* Set while the settings are at their defaults, one for each of the two
 * paths: the chroma pass is skipped outright, and the drawing falls back
 * to the plain copy it was before any of this existed. */
static int g_chroma_identity = 1;
static int g_picture_identity = 1;

static void rebuild_chroma_matrix(void) {
    const double s = g_saturation / 100.0;
    const double a = g_hue * M_PI / 180.0;
    const double c = cos(a) * s, d = sin(a) * s;
    g_cm[0] = (int)(c * 256.0);
    g_cm[1] = (int)(-d * 256.0);
    g_cm[2] = (int)(d * 256.0);
    g_cm[3] = (int)(c * 256.0);
    g_chroma_identity = (g_saturation == 100 && g_hue == 0);
    g_picture_identity = (g_brightness == 100 && g_contrast == 100);
}

void video_set_adjust(int brightness, int contrast, int saturation, int hue) {
    g_brightness = brightness < 50 ? 50 : (brightness > 150 ? 150 : brightness);
    g_contrast = contrast < 50 ? 50 : (contrast > 150 ? 150 : contrast);
    g_saturation = saturation < 0 ? 0 : (saturation > 200 ? 200 : saturation);
    while (hue > 180) hue -= 360;
    while (hue < -180) hue += 360;
    g_hue = hue;
    rebuild_chroma_matrix();
}

void video_get_adjust(int *brightness, int *contrast, int *saturation, int *hue) {
    if (brightness) *brightness = g_brightness;
    if (contrast) *contrast = g_contrast;
    if (saturation) *saturation = g_saturation;
    if (hue) *hue = g_hue;
}

/* In place on the decoded frame, before it is handed to the texture.
 * `stride_pairs` is how many chroma samples a row holds. */
static void adjust_chroma(uint8_t *u, uint8_t *v, int step, int stride, int w, int h) {
    for (int row = 0; row < h; row++) {
        uint8_t *pu = u + (size_t)row * stride;
        uint8_t *pv = v + (size_t)row * stride;
        for (int i = 0; i < w; i++) {
            const int cu = pu[i * step] - 128;
            const int cv = pv[i * step] - 128;
            int nu = 128 + ((cu * g_cm[0] + cv * g_cm[1]) >> 8);
            int nv = 128 + ((cu * g_cm[2] + cv * g_cm[3]) >> 8);
            if (nu < 0) nu = 0; else if (nu > 255) nu = 255;
            if (nv < 0) nv = 0; else if (nv > 255) nv = 255;
            pu[i * step] = (uint8_t)nu;
            pv[i * step] = (uint8_t)nv;
        }
    }
}

/* Why the hardware path was not taken. Shown on screen: "it fell back to
 * software" is not a diagnosis, and this console has no shell to ask. */
static char g_reason[96] = "";

const char *video_decoder_name(void) {
    static char name[128];
    switch (g_kind) {
        case DECODER_NVDEC:
            snprintf(name, sizeof(name), "NVDEC hardware (%s)",
                     g_codec_id == AV_CODEC_ID_H264 ? "H.264" : "VP8");
            return name;
        case DECODER_SOFTWARE:
            snprintf(name, sizeof(name), "libvpx software -- %s",
                     g_reason[0] ? g_reason : "no reason recorded");
            return name;
        default:
            snprintf(name, sizeof(name), "no decoder -- %s",
                     g_reason[0] ? g_reason : "not started");
            return name;
    }
}

/* NVDEC hands back frames in its own hardware format; the transfer to
 * memory the CPU can read comes out as NV12, which SDL uploads directly.
 * The software decoder produces I420.
 *
 * The texture follows what the decoder actually produces rather than
 * what the host announced. Frames whose size did not match were simply
 * dropped, and the two disagree routinely: the announcement of a new
 * resolution arrives before the encoder has finished changing over, so
 * every frame in between was thrown away, and if the change never
 * completed exactly as announced the picture never came back at all. */
static int ensure_texture(int width, int height, int nv12) {
    if (g_texture && width == g_width && height == g_height && nv12 == g_texture_is_nv12) {
        return 0;
    }
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    g_texture = SDL_CreateTexture(g_renderer,
                                  nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV,
                                  SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!g_texture) {
        printf("SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }
    g_texture_is_nv12 = nv12;
    g_width = width;
    g_height = height;
    g_have_picture = 0;
    return 0;
}

/* Chooses the hardware surface when the decoder offers it.
 *
 * ffmpeg asks which pixel format to produce once it knows the stream's
 * shape. Returning the hardware one is what actually engages the engine:
 * without this callback the decoder quietly picks a software format and
 * everything runs on the CPU while looking like it succeeded. */
static enum AVPixelFormat pick_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *formats) {
    (void)ctx;
    for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++) {
        if (*f == AV_PIX_FMT_NVTEGRA) {
            return *f;
        }
    }
    snprintf(g_reason, sizeof(g_reason), "decoder offered no hardware surface");
    return formats[0];
}

static int init_hardware(enum AVCodecID codec_id) {
    /* These are hwaccelS, not decoders in their own right: there is no
     * "vp8_nvtegra" to look up by name, and asking for one is why this
     * silently fell back to software. The ordinary decoder is opened and
     * the engine is attached to it. */
    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        snprintf(g_reason, sizeof(g_reason), "no decoder for this codec");
        return -1;
    }

    /* Confirm this build really pairs that decoder with the Tegra
     * engine, rather than finding out by having it not work. */
    int has_nvtegra = 0;
    for (int i = 0;; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        if (cfg->device_type == AV_HWDEVICE_TYPE_NVTEGRA ||
            cfg->pix_fmt == AV_PIX_FMT_NVTEGRA) {
            has_nvtegra = 1;
            break;
        }
    }
    if (!has_nvtegra) {
        snprintf(g_reason, sizeof(g_reason), "%s has no nvtegra hwaccel", codec->name);
        return -1;
    }

    int err = av_hwdevice_ctx_create(&g_hw_device, AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0);
    if (err < 0) {
        char buf[64] = {0};
        av_strerror(err, buf, sizeof(buf));
        snprintf(g_reason, sizeof(g_reason), "NVDEC device: %s", buf);
        return -1;
    }
    g_avctx = avcodec_alloc_context3(codec);
    if (!g_avctx) {
        snprintf(g_reason, sizeof(g_reason), "out of memory");
        return -1;
    }
    g_avctx->hw_device_ctx = av_buffer_ref(g_hw_device);
    g_avctx->get_format = pick_hw_format;
    g_avctx->width = g_width;
    g_avctx->height = g_height;
    /* Nothing here needs frames in order or in the future: this is a
     * live stream, and a decoder holding frames back to reorder them
     * would be adding latency to hide a problem VP8 does not have. */
    g_avctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    g_avctx->thread_count = 1; /* the engine does the work, not the CPU */

    err = avcodec_open2(g_avctx, codec, NULL);
    if (err < 0) {
        char buf[64] = {0};
        av_strerror(err, buf, sizeof(buf));
        snprintf(g_reason, sizeof(g_reason), "%s: %s", codec->name, buf);
        avcodec_free_context(&g_avctx);
        av_buffer_unref(&g_hw_device);
        return -1;
    }

    g_packet = av_packet_alloc();
    g_hw_frame = av_frame_alloc();
    g_sw_frame = av_frame_alloc();
    if (!g_packet || !g_hw_frame || !g_sw_frame) {
        return -1;
    }
    return 0;
}

static int init_software(void) {
    vpx_codec_dec_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* Three of the four cores; the fourth is the system's. */
    cfg.threads = 3;
    cfg.w = (unsigned)g_width;
    cfg.h = (unsigned)g_height;
    if (vpx_codec_dec_init(&g_vpx, vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK) {
        printf("vpx: %s\n", vpx_codec_error(&g_vpx));
        return -1;
    }
    g_vpx_ready = 1;
    return 0;
}

int video_init(SDL_Renderer *renderer, int width, int height, int codec) {
    g_renderer = renderer;
    g_width = width;
    g_height = height;
    g_have_picture = 0;
    g_reason[0] = '\0';
    g_codec_id = (codec == VIDEO_CODEC_H264) ? AV_CODEC_ID_H264 : AV_CODEC_ID_VP8;

    if (init_hardware(g_codec_id) == 0) {
        g_kind = DECODER_NVDEC;
    } else {
        printf("video: hardware decode unavailable (%s), using software\n", g_reason);
        if (g_codec_id != AV_CODEC_ID_VP8) {
            /* The software fallback here is libvpx, which is VP8 only.
             * An H.264 stream with no engine to decode it has nowhere to
             * go, and pretending otherwise would show a black screen
             * with no explanation. */
            /* Not built from g_reason into itself: overlapping source
             * and destination in snprintf is undefined. */
            char why[sizeof(g_reason)];
            snprintf(why, sizeof(why), "%s", g_reason);
            snprintf(g_reason, sizeof(g_reason), "%.60s + no software H.264", why);
            g_kind = DECODER_NONE;
            return -1;
        }
        if (init_software() != 0) {
            g_kind = DECODER_NONE;
            return -1;
        }
        g_kind = DECODER_SOFTWARE;
    }

    /* A first texture at the announced size so there is something to
     * draw immediately; the decode path resizes it if the stream turns
     * out to differ. */
    g_width = g_height = 0;
    if (ensure_texture(width, height, g_kind == DECODER_NVDEC) != 0) {
        return -1;
    }
    printf("video: %dx%d via %s\n", width, height, video_decoder_name());
    return 0;
}

void video_exit(void) {
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    if (g_avctx) avcodec_free_context(&g_avctx);
    if (g_hw_device) av_buffer_unref(&g_hw_device);
    if (g_packet) av_packet_free(&g_packet);
    if (g_hw_frame) av_frame_free(&g_hw_frame);
    if (g_sw_frame) av_frame_free(&g_sw_frame);
    if (g_vpx_ready) {
        vpx_codec_destroy(&g_vpx);
        g_vpx_ready = 0;
    }
    g_kind = DECODER_NONE;
}

static int decode_avcodec(const uint8_t *data, uint32_t size) {
    /* The packet points at the caller's buffer, which stays valid for
     * the duration of this call -- avcodec_send_packet copies what it
     * needs to keep. */
    g_packet->data = (uint8_t *)data;
    g_packet->size = (int)size;

    if (avcodec_send_packet(g_avctx, g_packet) < 0) {
        g_failed++;
        return 0;
    }

    int produced = 0;
    while (avcodec_receive_frame(g_avctx, g_hw_frame) == 0) {
        AVFrame *display = g_hw_frame;

        if (g_hw_frame->format == AV_PIX_FMT_NVTEGRA) {
            /* Off the engine's memory and into something SDL can upload.
             * Unified memory, so this is a copy rather than a bus
             * transfer, and the hardware's own compositor does the
             * format work. */
            av_frame_unref(g_sw_frame);
            if (av_hwframe_transfer_data(g_sw_frame, g_hw_frame, 0) < 0) {
                g_failed++;
                av_frame_unref(g_hw_frame);
                continue;
            }
            display = g_sw_frame;
        }

        /* Which planes came back decides the texture, rather than which
         * decoder was opened: the transfer off the engine picks its own
         * output format, and assuming NV12 because the hardware path was
         * taken would upload a three-plane picture as a two-plane one --
         * a picture, but the wrong one. */
        int nv12 = (display->format == AV_PIX_FMT_NV12);
        int planar = (display->format == AV_PIX_FMT_YUV420P ||
                      display->format == AV_PIX_FMT_YUVJ420P);
        if (!nv12 && !planar) {
            snprintf(g_reason, sizeof(g_reason), "unsupported pixel format %d", display->format);
            g_failed++;
        } else if (ensure_texture(display->width, display->height, nv12) != 0) {
            g_failed++;
        } else {
            if (nv12) {
                if (!g_chroma_identity) {
                    /* U and V are interleaved, so one plane with a step
                     * of two describes both. */
                    adjust_chroma(display->data[1], display->data[1] + 1, 2,
                                  display->linesize[1],
                                  (display->width + 1) / 2, (display->height + 1) / 2);
                }
                SDL_UpdateNVTexture(g_texture, NULL,
                                    display->data[0], display->linesize[0],
                                    display->data[1], display->linesize[1]);
            } else {
                if (!g_chroma_identity) {
                    adjust_chroma(display->data[1], display->data[2], 1,
                                  display->linesize[1],
                                  (display->width + 1) / 2, (display->height + 1) / 2);
                }
                SDL_UpdateYUVTexture(g_texture, NULL,
                                     display->data[0], display->linesize[0],
                                     display->data[1], display->linesize[1],
                                     display->data[2], display->linesize[2]);
            }
            g_decoded++;
            g_have_picture = 1;
            produced = 1;
        }
        av_frame_unref(g_hw_frame);
        av_frame_unref(g_sw_frame);
    }
    return produced;
}

static int decode_software(const uint8_t *data, uint32_t size) {
    if (vpx_codec_decode(&g_vpx, data, size, NULL, 0) != VPX_CODEC_OK) {
        /* One bad frame is not a reason to tear anything down: the next
         * keyframe recovers, and the host sends one on request. */
        g_failed++;
        return 0;
    }
    vpx_codec_iter_t iter = NULL;
    vpx_image_t *img = vpx_codec_get_frame(&g_vpx, &iter);
    if (!img) {
        return 0;
    }
    if (ensure_texture((int)img->d_w, (int)img->d_h, 0) != 0) {
        g_failed++;
        return 0;
    }
    SDL_UpdateYUVTexture(g_texture, NULL,
                         img->planes[VPX_PLANE_Y], img->stride[VPX_PLANE_Y],
                         img->planes[VPX_PLANE_U], img->stride[VPX_PLANE_U],
                         img->planes[VPX_PLANE_V], img->stride[VPX_PLANE_V]);
    g_decoded++;
    g_have_picture = 1;
    return 1;
}

int video_decode(const uint8_t *data, uint32_t size) {
    if (!g_texture || !data || !size) {
        return 0;
    }
    switch (g_kind) {
        case DECODER_NVDEC:    return decode_avcodec(data, size);
        case DECODER_SOFTWARE: return decode_software(data, size);
        default:               return 0;
    }
}

/* Brightness and contrast, done by the renderer.
 *
 *   out = in * gain + offset,  gain = contrast * brightness
 *                              offset = (1 - contrast) / 2
 *
 * A colour modulation covers the multiply up to 1. Above that the
 * picture is drawn a second time and added to itself, which is the same
 * thing and still one pass over a quad. The offset is a plain quad,
 * added or subtracted depending on its sign -- a blend mode cannot add a
 * negative number, so the operation changes rather than the value. */
void video_draw(SDL_Renderer *renderer, const SDL_Rect *dst) {
    if (!g_texture || !g_have_picture) {
        return;
    }

    /* Untouched settings take the path that existed before they did:
     * one copy, and not a single call that would not otherwise be made.
     * The two setters below are a few nanoseconds each, but "costs
     * nothing when unused" should be true because there is nothing
     * there, not because what is there happens to be small. */
    if (g_picture_identity) {
        SDL_RenderCopy(renderer, g_texture, NULL, dst);
        return;
    }

    const double gain = (g_contrast / 100.0) * (g_brightness / 100.0);
    const double offset = (1.0 - g_contrast / 100.0) / 2.0;

    const double first = gain > 1.0 ? 1.0 : gain;
    SDL_SetTextureColorMod(g_texture, (Uint8)(first * 255), (Uint8)(first * 255),
                           (Uint8)(first * 255));
    SDL_SetTextureBlendMode(g_texture, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(renderer, g_texture, NULL, dst);

    if (gain > 1.0) {
        const double extra = gain - 1.0 > 1.0 ? 1.0 : gain - 1.0;
        SDL_SetTextureColorMod(g_texture, (Uint8)(extra * 255), (Uint8)(extra * 255),
                               (Uint8)(extra * 255));
        SDL_SetTextureBlendMode(g_texture, SDL_BLENDMODE_ADD);
        SDL_RenderCopy(renderer, g_texture, NULL, dst);
    }

    if (offset > 0.001 || offset < -0.001) {
        const int level = (int)(fabs(offset) * 255.0);
        SDL_BlendMode mode = SDL_ComposeCustomBlendMode(
            SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE,
            offset > 0 ? SDL_BLENDOPERATION_ADD : SDL_BLENDOPERATION_REV_SUBTRACT,
            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
        SDL_SetRenderDrawBlendMode(renderer, mode);
        SDL_SetRenderDrawColor(renderer, (Uint8)level, (Uint8)level, (Uint8)level, 255);
        SDL_RenderFillRect(renderer, dst);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    }
}

void video_stats(unsigned long *decoded, unsigned long *failed) {
    if (decoded) *decoded = g_decoded;
    if (failed) *failed = g_failed;
}
