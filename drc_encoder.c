#include "drc_encoder.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <x264.h>

/*
 * Where to look. The vendored build first, since that is the one this
 * project knows the provenance of; then the bare soname, for a machine
 * that has installed drc-x264 properly. Never "libx264.so", which on
 * any ordinary system is the distribution's build and is exactly the
 * one that must not answer.
 */
static const char *const DRC_SO_CANDIDATES[] = {
    "wiiu_gamepad/vendor/x264/lib/libx264.so.140",
    "libx264.so.140",
};

/*
 * The vendored copy sits beside the binary, not beside whatever
 * directory it was launched from -- the same rule the rest of this host
 * follows for page.html and the .env, and for the same reason: a path
 * built from the working directory is right exactly until someone runs
 * the program from somewhere else.
 */
static void exe_relative(const char *suffix, char *out, size_t out_size) {
    char exe[512];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        out[0] = '\0';
        return;
    }
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) {
        out[0] = '\0';
        return;
    }
    *slash = '\0';
    if ((size_t)snprintf(out, out_size, "%s/%s", exe, suffix) >= out_size) {
        out[0] = '\0';
    }
}

struct DrcEncoder {
    void *lib;
    char  lib_path[256];

    /* Resolved through the handle, never by global name: see the note
     * in the header about the one symbol x264 versions and the many it
     * does not. */
    void  (*param_default_preset)(x264_param_t *, const char *, const char *);
    int   (*param_apply_profile)(x264_param_t *, const char *);
    x264_t *(*encoder_open)(x264_param_t *);
    int   (*encoder_encode)(x264_t *, x264_nal_t **, int *, x264_picture_t *, x264_picture_t *);
    void  (*encoder_close)(x264_t *);
    void  (*picture_init)(x264_picture_t *);

    x264_t   *enc;
    char      preset[32];

    /* Filled by the callback below, one frame at a time. */
    const uint8_t *chunk[DRC_ENC_CHUNKS];
    uint32_t       size[DRC_ENC_CHUNKS];
    int            chunks_seen;
    int            frame_is_idr;
};

/*
 * x264 hands every NAL to this instead of returning them in a list,
 * because in DRH mode they are not NAL units at all -- they are
 * macroblock rows, and which of the five chunks a row belongs to is
 * decided by where it starts. This is libdrc's arithmetic, kept
 * identical on purpose: the pad expects five and only five.
 */
static void on_nal(x264_t *h, x264_nal_t *nal, void *opaque) {
    (void)h;
    DrcEncoder *e = opaque;
    if (!e || !nal) {
        return;
    }
    if (nal->i_type == NAL_SEI) {
        return;
    }
    const int mb_per_frame = ((DRC_ENC_WIDTH + 15) / 16) * ((DRC_ENC_HEIGHT + 15) / 16);
    const int mb_per_chunk = mb_per_frame / DRC_ENC_CHUNKS;
    if (mb_per_chunk <= 0) {
        return;
    }
    const int idx = nal->i_first_mb / mb_per_chunk;
    if (idx < 0 || idx >= DRC_ENC_CHUNKS) {
        return;
    }
    e->chunk[idx] = nal->p_payload;
    e->size[idx]  = (uint32_t)nal->i_payload;
    e->chunks_seen++;
    /*
     * Read off the last chunk rather than from what was asked for: x264
     * emits IDRs on its own as well, and a frame marked non-IDR that
     * really is one leaves the pad without the recovery point it is
     * waiting for.
     */
    if (e->chunks_seen == DRC_ENC_CHUNKS) {
        e->frame_is_idr = (nal->i_ref_idc != NAL_PRIORITY_DISPOSABLE &&
                           nal->i_type == NAL_SLICE_IDR);
    }
}

/* Silences x264's own logging: it writes to stderr on its own schedule,
 * and this host keeps its periodic lines behind VERBOSE. */
static void on_log(void *data, int level, const char *fmt, va_list va) {
    (void)data; (void)level; (void)fmt; (void)va;
}

/*
 * Every parameter below is libdrc's, and the comments say which ones
 * cannot move. Reproduced rather than called into because libdrc is C++
 * and this host is one gcc line of C -- so the risk is that the two
 * drift. If the pad starts refusing frames after a libdrc update, this
 * block is the first place to compare.
 */
static int build_encoder(DrcEncoder *e, char *err, size_t err_size) {
    x264_param_t p;
    e->param_default_preset(&p, e->preset, "zerolatency");

    p.i_width  = DRC_ENC_WIDTH;
    p.i_height = DRC_ENC_HEIGHT;

    p.analyse.inter &= ~X264_ANALYSE_PSUB16x16;

    /* Intra refresh, and the wave's period in frames. Loss is repaired
     * as a sweep across the picture rather than by a keyframe burst of
     * about twenty-five packets, which is what the link cannot take. */
    p.b_intra_refresh = 1;
    p.i_keyint_max = 30;
    p.i_keyint_min = 10;
    const char *refresh_env = getenv("DRC_REFRESH");
    if (refresh_env) {
        const int period = atoi(refresh_env);
        if (period >= 10 && period <= 240) {
            p.i_keyint_max = period;
            p.i_keyint_min = period < 10 ? period : 10;
        }
    }

    p.i_scenecut_threshold = -1;
    p.i_csp = X264_CSP_I420;
    p.b_cabac = 1;
    p.b_interlaced = 0;
    p.i_bframe = 0;
    p.i_bframe_pyramid = 0;
    p.i_frame_reference = 1;

    /*
     * constrained_intra_pred lives in the PPS, and DRH never transmits
     * one -- the pad decodes with a hardcoded set. A flag that
     * disagrees with the pad's makes every intra macroblock beside an
     * inter one predicted one way here and another way there, which
     * lands exactly along the refresh wave and sweeps across the
     * picture with it.
     */
    p.b_constrained_intra = 1;

    p.analyse.i_weighted_pred = 0;
    p.analyse.b_weighted_bipred = 0;
    p.analyse.b_transform_8x8 = 0;
    /*
     * Zero, and not worth moving.
     *
     * Quantising chroma harder than luma looks like free bits -- the
     * eye is far less sensitive to colour error -- and it is not.
     * Measured on three seconds of this bench's picture: +2 saved
     * nothing at all, +4 saved 0.5 percent, +6 saved 1.3. In 4:2:0 on
     * content this flat, chroma is a much smaller share of the bits
     * than it is usually said to be.
     *
     * Against which: chroma_qp_index_offset is signalled in the PPS,
     * and DRH never transmits one -- the pad decodes with a set it
     * holds in firmware. So a non-zero value here is the encoder and
     * the decoder disagreeing about the quantiser, which is the exact
     * shape of the constrained_intra problem above. The rig that would
     * prove it safe cannot: it rebuilds the missing slice headers by
     * hand and is too lossy to measure a drift this small.
     *
     * One point three percent, for a risk that could not be checked.
     * The refresh period is worth forty-five for one setting.
     */
    p.analyse.i_chroma_qp_offset = 0;
    const char *cqp_env = getenv("DRC_CHROMA_QP");
    if (cqp_env) {
        p.analyse.i_chroma_qp_offset = atoi(cqp_env);
    }

    /*
     * QP 32, and nothing else works.
     *
     * With no slice header the decoder cannot learn the slice QP and
     * assumes 32; drc-x264 forces pic_init_qp to 32 and slice_qp_delta
     * to 0 to match. Any rate control that moves it -- CRF, ABR, or
     * simply a different CQP -- quantises at one QP and signals
     * another, and the pad decodes noise: it asks for a keyframe on
     * every frame and the picture never settles.
     */
    p.rc.i_rc_method = X264_RC_CQP;
    p.rc.i_qp_constant = p.rc.i_qp_min = p.rc.i_qp_max = 32;
    p.rc.f_ip_factor = 1.0f;

    /* The frame QP cannot move, but mb_qp_delta is transmitted, so the
     * bits can still be spent where they show. drc-x264 keeps adaptive
     * quantisation alive under DRH, which stock x264 drops in CQP. */
    /*
     * Adaptive quantisation, which only exists here at all because
     * drc-x264 keeps it alive under DRH -- stock x264 drops it in CQP,
     * which is why it cannot be measured with an ordinary ffmpeg.
     */
    p.rc.i_aq_mode = X264_AQ_VARIANCE;
    p.rc.f_aq_strength = 1.0f;
    const char *aq_mode_env = getenv("DRC_AQ");
    if (aq_mode_env) {
        p.rc.i_aq_mode = atoi(aq_mode_env);
    }
    const char *aq_env = getenv("DRC_AQ_STRENGTH");
    if (aq_env) {
        p.rc.f_aq_strength = (float)atof(aq_env);
    }

    /*
     * Trellis OFF, measured rather than assumed.
     *
     * The usual claim is that it lowers the bitrate, and that is true
     * at constant QUALITY -- in CRF, where it lets you raise the
     * quantiser for the same result. At a quantiser pinned to 32 it
     * does the opposite: better decisions, more coefficients kept, more
     * bits. Measured on three seconds of this bench's own picture, it
     * cost 2 percent more bits (121746 against 119291) for 0.0005 of
     * SSIM, which is not visible. Two percent of a budget this tight is
     * worth more than that.
     */
    p.analyse.i_trellis = 0;
    const char *trellis_env = getenv("DRC_TRELLIS");
    if (trellis_env) {
        p.analyse.i_trellis = atoi(trellis_env);
    }

    /* No SPS, PPS, SEI or access unit delimiters: the pad has its own
     * and would choke on ours. */
    p.b_repeat_headers = 0;
    p.b_aud = 0;

    /* The whole reason this fork exists: macroblock rows rather than
     * NAL units, and no slice header. */
    p.b_drh_mode = 1;

    /* One complete frame, serially, so the five chunks arrive in order
     * and the callback can place them by first macroblock. */
    p.i_threads = 1;
    p.b_sliced_threads = 0;
    p.i_slice_count = 1;
    p.nalu_process = on_nal;

    p.i_log_level = X264_LOG_NONE;
    p.pf_log = on_log;

    /* The same line libdrc prints, so the two encoders can be compared
     * at a glance. They must agree: the pad decodes what one of them
     * produces and a difference here is a difference it cannot read. */
    fprintf(stderr, "[drc-enc] preset=%s subme=%d me=%d trellis=%d aq=%d "
                    "refresh=%d keyint=%d/%d qp=%d\n",
            e->preset, p.analyse.i_subpel_refine, p.analyse.i_me_method,
            p.analyse.i_trellis, p.rc.i_aq_mode, p.b_intra_refresh,
            p.i_keyint_min, p.i_keyint_max, p.rc.i_qp_constant);

    if (e->param_apply_profile(&p, "main") < 0) {
        snprintf(err, err_size, "drc-x264 refused the main profile");
        return -1;
    }

    e->enc = e->encoder_open(&p);
    if (!e->enc) {
        snprintf(err, err_size, "drc-x264 would not open an encoder");
        return -1;
    }
    return 0;
}

DrcEncoder *drc_encoder_open(const char *so_path, const char *preset,
                             char *err, size_t err_size) {
    if (err && err_size) {
        err[0] = '\0';
    }
    DrcEncoder *e = calloc(1, sizeof(*e));
    if (!e) {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }

    /*
     * RTLD_LOCAL is the load-bearing half. Without it this library
     * joins the global scope and its unversioned symbols -- every one
     * but x264_encoder_open -- become candidates for anything else in
     * the process that wants an x264, and the other build's for us.
     */
    const char *tried = NULL;
    if (so_path && *so_path) {
        e->lib = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
        tried = so_path;
    } else {
        char beside[512];
        exe_relative("wiiu_gamepad/vendor/x264/lib/libx264.so.140", beside, sizeof(beside));
        if (beside[0]) {
            e->lib = dlopen(beside, RTLD_NOW | RTLD_LOCAL);
            tried = beside;
        }
        for (size_t i = 0; !e->lib && i < sizeof(DRC_SO_CANDIDATES) / sizeof(DRC_SO_CANDIDATES[0]); i++) {
            e->lib = dlopen(DRC_SO_CANDIDATES[i], RTLD_NOW | RTLD_LOCAL);
            tried = DRC_SO_CANDIDATES[i];
        }
    }
    if (!e->lib) {
        snprintf(err, err_size, "no drc-x264 (%s): %s", tried ? tried : "?", dlerror());
        free(e);
        return NULL;
    }
    snprintf(e->lib_path, sizeof(e->lib_path), "%s", tried ? tried : "?");

    /*
     * Through the handle, every one of them. dlsym(RTLD_DEFAULT, ...)
     * here would hand back whichever x264 the process happened to have
     * first, which for the unversioned names is usually the wrong one.
     */
    #define SYM(field, name)                                                  \
        do {                                                                  \
            *(void **)(&e->field) = dlsym(e->lib, name);                      \
            if (!e->field) {                                                  \
                snprintf(err, err_size, "%s has no %s", e->lib_path, name);   \
                dlclose(e->lib);                                              \
                free(e);                                                      \
                return NULL;                                                  \
            }                                                                 \
        } while (0)

    SYM(param_default_preset, "x264_param_default_preset");
    SYM(param_apply_profile,  "x264_param_apply_profile");
    /* The one symbol that carries its build number. Asking for the
     * _140 name is what proves this is drc-x264 and not the system's
     * library answering under a familiar name. */
    SYM(encoder_open,         "x264_encoder_open_140");
    SYM(encoder_encode,       "x264_encoder_encode");
    SYM(encoder_close,        "x264_encoder_close");
    SYM(picture_init,         "x264_picture_init");
    #undef SYM

    snprintf(e->preset, sizeof(e->preset), "%s", (preset && *preset) ? preset : "fast");

    if (build_encoder(e, err, err_size) != 0) {
        dlclose(e->lib);
        free(e);
        return NULL;
    }
    return e;
}

void drc_encoder_close(DrcEncoder *e) {
    if (!e) {
        return;
    }
    if (e->enc) {
        e->encoder_close(e->enc);
    }
    if (e->lib) {
        dlclose(e->lib);
    }
    free(e);
}

int drc_encoder_restart(DrcEncoder *e) {
    if (!e) {
        return -1;
    }
    if (e->enc) {
        e->encoder_close(e->enc);
        e->enc = NULL;
    }
    char err[128];
    return build_encoder(e, err, sizeof(err));
}

int drc_encoder_encode(DrcEncoder *e, const uint8_t *i420, int want_idr,
                       DrcFrame *out) {
    if (!e || !e->enc || !i420 || !out) {
        return -1;
    }

    x264_picture_t in, rec;
    e->picture_init(&in);
    e->picture_init(&rec);

    in.opaque = e;                 /* how the callback finds its way back */
    in.img.i_csp = X264_CSP_I420;
    in.img.i_plane = 3;

    /* Packed planes, the layout the scaler upstream writes. */
    in.img.i_stride[0] = DRC_ENC_WIDTH;
    in.img.plane[0] = (uint8_t *)i420;
    in.img.i_stride[1] = DRC_ENC_WIDTH / 2;
    in.img.plane[1] = in.img.plane[0] + (size_t)DRC_ENC_WIDTH * DRC_ENC_HEIGHT;
    in.img.i_stride[2] = DRC_ENC_WIDTH / 2;
    in.img.plane[2] = in.img.plane[1] + (size_t)DRC_ENC_WIDTH * DRC_ENC_HEIGHT / 4;

    in.i_type = want_idr ? X264_TYPE_IDR : X264_TYPE_P;

    /* Cleared before, not after: the callback fills these, and leaving
     * the previous frame's pointers in place would send buffers x264
     * has since written over. */
    memset(e->chunk, 0, sizeof(e->chunk));
    memset(e->size, 0, sizeof(e->size));
    e->chunks_seen = 0;
    e->frame_is_idr = 0;

    x264_nal_t *nals = NULL;
    int nal_count = 0;
    /*
     * The return value is the size of what x264 would have handed back
     * as NAL units, and in DRH mode that is not where the data is -- it
     * arrives through the callback. A zero here is normal; what matters
     * is whether five chunks turned up.
     */
    (void)e->encoder_encode(e->enc, &nals, &nal_count, &in, &rec);

    if (e->chunks_seen != DRC_ENC_CHUNKS) {
        return -1;
    }
    for (int i = 0; i < DRC_ENC_CHUNKS; i++) {
        if (!e->chunk[i] || !e->size[i]) {
            return -1;
        }
        out->chunk[i] = e->chunk[i];
        out->size[i]  = e->size[i];
    }
    out->is_idr = e->frame_is_idr;
    return 0;
}

const char *drc_encoder_library(const DrcEncoder *e) {
    return e ? e->lib_path : "";
}
