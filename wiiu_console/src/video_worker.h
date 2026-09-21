#ifndef CAPTURE2WIIU_VIDEO_WORKER_H
#define CAPTURE2WIIU_VIDEO_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "video.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned submitted;
    unsigned decoded;
    unsigned dropped;
    unsigned errors;
    unsigned queue_depth;
} VideoWorkerStats;

/*
 * H264DEC runs on a worker thread.
 *
 * The main thread stays available for:
 *   recv()
 *   Opus
 *   input
 *   GX2 / SDL present
 */
int video_worker_start(char *why, size_t why_size);
void video_worker_stop(void);

/*
 * Copies one Annex-B access unit into the bounded worker queue.
 *
 *  1 = queued
 *  0 = queue full, caller must resync on an IDR
 * -1 = allocation/thread error
 */
int video_worker_submit(const uint8_t *data, uint32_t size);

/*
 * Returns the newest decoded picture once.
 *
 * Older pictures are intentionally collapsed: for game streaming the
 * live edge matters more than displaying a stale queue.
 */
int video_worker_take(VideoFrame *out);

void video_worker_stats(VideoWorkerStats *out);

#ifdef __cplusplus
}
#endif

#endif
