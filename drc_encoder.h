#ifndef CAPTURE2CLOUD_DRC_ENCODER_H
#define CAPTURE2CLOUD_DRC_ENCODER_H

#include <stddef.h>
#include <stdint.h>

/*
 * The fourth encode: H.264 in the shape a Wii U GamePad can decode.
 *
 * The other three chains choose their size and their bitrate. This one
 * chooses nothing. 864x480, QP 32, exactly five chunks a frame, no slice
 * header -- all of it is the pad's protocol rather than a preference,
 * and every one of those numbers is load-bearing. The specification is
 * libdrc's own `src/h264-encoder.cpp`; this is that file's parameter
 * block, in C, so the host keeps building from one gcc line.
 *
 * Why it exists at all: the pad's decoder cannot read ordinary H.264.
 * It needs DRH slicing -- macroblock rows instead of NAL units, with no
 * slice header -- which only the drc-x264 fork produces. Without this
 * chain the picture has to be decoded and encoded a second time inside
 * the client, at a quantiser pinned to 32.
 *
 *
 * THE LIBRARY IS OPENED, NOT LINKED, AND THAT IS NOT A CONVENIENCE.
 *
 * drc-x264 is libx264.so.140. Anything that pulls libavcodec -- which
 * this host does, and GStreamer's x264enc does too -- brings
 * libx264.so.164 into the same process. x264 stamps its build number
 * into exactly ONE exported symbol, x264_encoder_open, and its header
 * says so: the suffix exists "for purposes of dlopen". Every other
 * symbol, x264_encoder_encode included, is a bare name.
 *
 * So the two builds do NOT safely coexist by themselves. Measured on
 * this machine with LD_DEBUG=bindings, libavcodec already binds
 * x264_encoder_open_164 to the system library and x264_encoder_encode
 * to the vendored one -- an encoder opened by one build and driven by
 * another's code. Nothing has gone wrong yet only because no caller in
 * that process ever asks libavcodec for an x264 encoder.
 *
 * RTLD_LOCAL keeps this library out of the global lookup scope, and
 * every entry point is reached through the handle rather than by name,
 * so neither build can answer for the other. tests/c/test_drc_encoder.c
 * pins that down.
 *
 * It also means a machine with no drc-x264 loses this chain and nothing
 * else: drc_encoder_open() returns NULL, the host says so once, and the
 * client falls back to decoding and re-encoding for itself.
 */

/* Fixed by the protocol. Not configurable, here or anywhere. */
#define DRC_ENC_WIDTH   864
#define DRC_ENC_HEIGHT  480
#define DRC_ENC_CHUNKS  5

typedef struct DrcEncoder DrcEncoder;

/* One encoded frame: five chunks, pointing into the encoder's own
 * buffers and valid only until the next drc_encoder_encode(). */
typedef struct {
    const uint8_t *chunk[DRC_ENC_CHUNKS];
    uint32_t       size[DRC_ENC_CHUNKS];
    int            is_idr;   /* what the frame actually came out as */
} DrcFrame;

/*
 * Loads drc-x264 and opens an encoder, or returns NULL and writes why
 * into `err`. `so_path` may be NULL for the built-in search.
 *
 * `preset` is x264's, and the default is deliberate: libdrc ships
 * "medium", but measured against a moving picture "fast" holds zero
 * keyframe requests where "medium" asks sixty a second. Pass NULL for
 * the measured default.
 */
DrcEncoder *drc_encoder_open(const char *so_path, const char *preset,
                             char *err, size_t err_size);

void drc_encoder_close(DrcEncoder *e);

/*
 * Encodes one I420 frame, which must be DRC_ENC_WIDTH x DRC_ENC_HEIGHT
 * with its three planes packed one after another. Returns 0 on success.
 *
 * `want_idr` asks for a recovery point. Read `out->is_idr` rather than
 * assuming it was granted: x264 emits IDRs of its own, and under intra
 * refresh it may answer the request by restarting its refresh wave
 * instead -- see drc_encoder_restart().
 */
int drc_encoder_encode(DrcEncoder *e, const uint8_t *i420, int want_idr,
                       DrcFrame *out);

/*
 * Throws the encoder away and starts another, which is the only way to
 * get a real IDR out of it.
 *
 * With intra refresh on, a forced IDR restarts the refresh wave and
 * x264 never emits NAL_SLICE_IDR again. That repairs ordinary loss, but
 * a decoder that has lost the sequence outright has nothing to recover
 * from: the pad freezes for good while the sound keeps playing. Call
 * this when a client asks for a keyframe and the last one was not
 * granted.
 */
int drc_encoder_restart(DrcEncoder *e);

/* Which library answered, for the log line that says this chain is up. */
const char *drc_encoder_library(const DrcEncoder *e);

#endif /* CAPTURE2CLOUD_DRC_ENCODER_H */
