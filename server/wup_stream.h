#ifndef CAPTURE2CLOUD_WUP_STREAM_H
#define CAPTURE2CLOUD_WUP_STREAM_H
#include "video_capture.h"
#include <stddef.h>
#include <stdint.h>

typedef struct WupStream WupStream;
typedef struct {
    double fps, fresh_fps, captured_fps, frame_call_ms, scale_ms;
    unsigned repeated, dropped, errors;
    int valid;
} WupStreamMetrics;
/* A self-contained helper drives libwupstream; the Capture2Cloud process
 * never loads its patched x264 or blocks the HDMI capture loop. */
WupStream *wup_stream_start(const char *project_dir, const char *pad_ip);
void wup_stream_stop(WupStream *w);
void wup_stream_poll(WupStream *w, int touch_mode);
void wup_stream_push_video(WupStream *w, const VideoFrame *frame, int width, int height);
/* PCM 48 kHz, stereo, filtered by audio_capture.c; realtime, nonblocking. */
void wup_stream_push_audio(WupStream *w, const int16_t *samples, size_t frames);
const char *wup_stream_status(WupStream *w);
void wup_stream_metrics_snapshot(const WupStream *w, WupStreamMetrics *out);
int wup_stream_connected(const WupStream *w);
#endif
