#include "video.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <coreinit/cache.h>
#include <h264/decode.h>

/*
 * The decoder writes with the GPU's view of memory, not the CPU's.
 *
 * Both its working memory and the frame buffer have to be aligned and
 * have to be flushed out of the CPU's cache before it reads them, and
 * invalidated before we read what it wrote. Getting this wrong does not
 * fail -- it shows a picture made of stale cache lines, which looks
 * like a decoder bug and is not one.
 */
#define DEC_ALIGN  0x100

/* Baseline is what the other clients' encoders are configured to
 * produce, but the host's VA encoders emit High when left alone, so the
 * decoder is opened for High and accepts all three. Level 4.0 covers
 * 1080p30 and 720p60; see SPEC.md on why 720p60 is the tested path. */
#define DEC_PROFILE 100
#define DEC_LEVEL   40

static void    *g_mem;         /* the decoder's working memory */
static uint32_t g_mem_size;
static void    *g_fb;          /* where it writes the picture */
static uint32_t g_fb_size;
static int      g_max_w, g_max_h;
static int      g_open;

static unsigned g_decoded, g_empty, g_errors;

/* Filled by the callback below, read straight after H264DECExecute. */
static H264DecodeResult g_last;
static int              g_have_last;

/*
 * Called by the decoder, once per picture, from inside H264DECExecute.
 *
 * Copied rather than kept by pointer: the struct belongs to the decoder
 * and the documentation does not promise it outlives the call. The
 * framebuffer it points AT is ours, so that pointer is fine to keep.
 */
static void on_frame_output(H264DecodeOutput *output)
{
    if (!output || output->frameCount <= 0 || !output->decodeResults) {
        return;
    }
    /* The last one, when several come out at once: this stream has no
     * B-frames and no reordering, so "several" means the decoder was
     * catching up and only the newest is worth drawing. */
    H264DecodeResult *r = output->decodeResults[output->frameCount - 1];
    if (!r) {
        return;
    }
    memcpy(&g_last, r, sizeof(g_last));
    g_have_last = 1;
}

int video_init(int max_width, int max_height, char *why, unsigned why_size)
{
    H264Error err;

    video_exit();

    err = H264DECMemoryRequirement(DEC_PROFILE, DEC_LEVEL, max_width, max_height, &g_mem_size);
    if (err != H264_ERROR_OK) {
        snprintf(why, why_size, "decoder refused %dx%d (error %d)", max_width, max_height,
                 (int)err);
        return -1;
    }

    g_mem = memalign(DEC_ALIGN, g_mem_size);
    if (!g_mem) {
        snprintf(why, why_size, "no room for the decoder (%u bytes)", (unsigned)g_mem_size);
        return -1;
    }

    /* NV12: a full luma plane, then half as many lines of interleaved
     * chroma. The decoder's stride can exceed the width, so this is
     * sized from a padded width rather than from the width itself. */
    const int padded = (max_width + 0xFF) & ~0xFF;
    g_fb_size = (uint32_t)padded * max_height * 3 / 2;
    g_fb = memalign(DEC_ALIGN, g_fb_size);
    if (!g_fb) {
        snprintf(why, why_size, "no room for a frame (%u bytes)", (unsigned)g_fb_size);
        free(g_mem);
        g_mem = NULL;
        return -1;
    }

    if ((err = H264DECInitParam(g_mem_size, g_mem)) != H264_ERROR_OK ||
        (err = H264DECSetParam_FPTR_OUTPUT(g_mem, on_frame_output)) != H264_ERROR_OK ||
        /* Told as soon as a picture exists rather than after the
         * decoder has buffered a few. Buffering is latency, and latency
         * is the thing this whole project is trying not to have. */
        (err = H264DECSetParam_OUTPUT_PER_FRAME(g_mem, 1)) != H264_ERROR_OK ||
        (err = H264DECOpen(g_mem)) != H264_ERROR_OK) {
        snprintf(why, why_size, "cannot open the decoder (error %d)", (int)err);
        free(g_fb);
        free(g_mem);
        g_fb = NULL;
        g_mem = NULL;
        return -1;
    }
    g_open = 1;

    if ((err = H264DECBegin(g_mem)) != H264_ERROR_OK) {
        snprintf(why, why_size, "decoder will not begin (error %d)", (int)err);
        video_exit();
        return -1;
    }

    g_max_w = max_width;
    g_max_h = max_height;
    g_decoded = g_empty = g_errors = 0;
    return 0;
}

void video_exit(void)
{
    if (g_open) {
        H264DECEnd(g_mem);
        H264DECClose(g_mem);
        g_open = 0;
    }
    free(g_fb);
    free(g_mem);
    g_fb = NULL;
    g_mem = NULL;
    g_fb_size = g_mem_size = 0;
    g_have_last = 0;
}

int video_decode(const uint8_t *annexb, uint32_t size, VideoFrame *out)
{
    if (!g_open || !annexb || size == 0) {
        return -1;
    }

    g_have_last = 0;

    /* Out of our cache and into memory, where the decoder reads. The
     * cast is because the API takes a non-const pointer it does not
     * write through. */
    DCFlushRange((void *)annexb, size);

    H264Error err = H264DECSetBitstream(g_mem, (uint8_t *)annexb, size, 0.0);
    if (err != H264_ERROR_OK) {
        g_errors++;
        return -1;
    }

    err = H264DECExecute(g_mem, g_fb);
    if (err != H264_ERROR_OK) {
        g_errors++;
        return -1;
    }

    if (!g_have_last || g_last.status != 0) {
        /* Normal, not a failure: a stream joined mid-flight produces
         * nothing until a recovery point arrives. */
        g_empty++;
        return 0;
    }

    /* What the decoder wrote is not in our cache yet. */
    DCInvalidateRange(g_fb, g_fb_size);

    const int stride = g_last.nextLine;
    out->luma   = (const uint8_t *)g_last.framebuffer;
    out->chroma = out->luma + (size_t)stride * g_last.height;
    out->stride = stride;
    out->width  = g_last.width;
    out->height = g_last.height;
    g_decoded++;
    return 1;
}

void video_flush(void)
{
    if (!g_open) {
        return;
    }
    H264DECFlush(g_mem);
    H264DECEnd(g_mem);
    H264DECBegin(g_mem);
    g_have_last = 0;
}

void video_stats(unsigned *decoded, unsigned *empty, unsigned *errors)
{
    if (decoded) *decoded = g_decoded;
    if (empty)   *empty   = g_empty;
    if (errors)  *errors  = g_errors;
}
