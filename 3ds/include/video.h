#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float receive_fps;
    float decode_fps;
    float display_fps;
    float decode_ms;
    float upload_ms;
    float bitrate_kbps;
    uint32_t decoded;
    uint32_t decode_errors;
    uint32_t local_latency_ms;
    bool hardware_conversion;
} VideoStats;

typedef enum {
    VIDEO_DECODE_ERROR = -1,
    VIDEO_DECODE_BUFFERED = 0,
    VIDEO_DECODE_FRAME = 1
} VideoDecodeResult;

bool video_init(void);
void video_clear(void);
void video_exit(void);
VideoDecodeResult video_decode_and_present(const uint8_t *data, uint32_t size,
                                             uint32_t received_ms, uint8_t codec,
                                             bool present);
void video_note_received_bytes(uint32_t frame_size);
void video_set_aspect_16_9(bool enabled);
void video_get_stats(VideoStats *stats);
