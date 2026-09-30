#pragma once

#include "audio.h"
#include "config.h"
#include "input.h"
#include "network.h"
#include "video.h"

#include <3ds.h>
#include <stdbool.h>

typedef enum {
    CONFIG_NONE,
    CONFIG_EDIT_HOST,
    CONFIG_EDIT_STREAM_PORT,
    CONFIG_EDIT_WEB_PORT,
    CONFIG_EDIT_PASSWORD,
    CONFIG_TOGGLE_AUTO,
    CONFIG_TOGGLE_AUDIO,
    CONFIG_TOGGLE_CODEC,
    CONFIG_SAVE,
    CONFIG_CONNECT,
    CONFIG_RECONNECT,
    CONFIG_DISCONNECT,
    CONFIG_CONTROLLER
} ConfigAction;

typedef struct {
    TouchControls controls;
    bool home_pressed;
    bool capture_pressed;
    bool open_config;
    bool toggle_stats;
    bool toggle_codec;
} TouchUiEvent;

void touch_ui_update_controller(u32 keys_down, u32 keys_held, u32 keys_up,
                                const touchPosition *position, int deadzone,
                                TouchUiEvent *event);
ConfigAction touch_ui_config_action(u32 keys_down, const touchPosition *position);
void touch_ui_draw_controller(const TouchUiEvent *event,
                              const NetworkStats *network,
                              const VideoStats *video,
                              const AudioStats *audio,
                              bool show_stats,
                              uint8_t video_codec);
void touch_ui_draw_config(const AppConfig *config, const NetworkStats *network,
                          const char *message);
