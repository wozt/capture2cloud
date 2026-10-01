#pragma once

#include "config.h"
#include "c2s_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NETWORK_DISCONNECTED,
    NETWORK_CONNECTING,
    NETWORK_CONNECTED,
    NETWORK_ERROR
} NetworkState;

typedef struct {
    NetworkState state;
    bool may_control;
    char status[96];
    uint64_t bytes_received;
    uint32_t video_received;
    uint32_t video_dropped;
    uint32_t audio_dropped;
    uint32_t reconnects;
    uint32_t last_receive_ms;
    uint32_t queue_depth;
    uint8_t video_codec;
    uint8_t audio_codec;
    uint8_t worker_core;
    float video_fps;
    float video_kbps;
    float receive_ms;
} NetworkStats;

bool network_init(void);
void network_exit(void);
void network_apply_config(const AppConfig *config);
void network_connect(void);
void network_disconnect(void);
void network_reconnect(void);
void network_set_auto_reconnect(bool enabled);
void network_get_stats(NetworkStats *stats);

bool network_acquire_video(const uint8_t **data, uint32_t *size,
                           uint32_t *received_ms, uint8_t *codec, int *slot);
void network_release_video(int slot);
bool network_take_audio(uint8_t *data, uint32_t capacity, uint32_t *size,
                        uint8_t *codec);
uint32_t network_audio_depth(void);
bool network_peek_audio_codec(uint8_t *codec);
void network_clear_audio(void);

void network_send_input(const int8_t state[C2S_PAD_SLOTS]);
void network_send_home(void);
void network_send_capture(void);
void network_request_codec(uint8_t codec);
void network_request_keyframe(void);
