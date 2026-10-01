#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OLD3DS_CONFIG_PATH "sdmc:/3ds/capture2cloud-old3ds/config.ini"

typedef struct {
    char host[64];
    uint16_t stream_port;
    uint16_t web_port;
    char password[96];
    bool auto_connect;
    bool audio_enabled;
    bool stats_enabled;
    bool aspect_16_9;
    uint8_t video_codec;
    int right_stick_deadzone;
} AppConfig;

void config_defaults(AppConfig *config);
bool config_load(AppConfig *config);
bool config_save(const AppConfig *config);
