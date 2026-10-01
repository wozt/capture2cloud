#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool available;
    uint32_t decoded_packets;
    uint32_t dropped_packets;
    uint32_t decode_errors;
    uint32_t queued_buffers;
    uint32_t underruns;
} AudioStats;

bool audio_init(void);
void audio_exit(void);
void audio_service(bool enabled);
void audio_clear(void);
void audio_get_stats(AudioStats *stats);
