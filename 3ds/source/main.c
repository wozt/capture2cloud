#include <3ds.h>

#include "audio.h"
#include "config.h"
#include "input.h"
#include "network.h"
#include "touch_ui.h"
#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool edit_text(char *value, size_t value_size, const char *hint,
                      SwkbdType type, bool password)
{
    SwkbdState keyboard;
    swkbdInit(&keyboard, type, 2, (int)value_size - 1);
    swkbdSetHintText(&keyboard, hint);
    swkbdSetInitialText(&keyboard, value);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "OK", true);
    if (password) swkbdSetPasswordMode(&keyboard, SWKBD_PASSWORD_HIDE_DELAY);
    char result[128];
    snprintf(result, sizeof(result), "%s", value);
    if (swkbdInputText(&keyboard, result, sizeof(result)) == SWKBD_BUTTON_RIGHT) {
        size_t copy_size = strnlen(result, value_size - 1);
        memcpy(value, result, copy_size);
        value[copy_size] = '\0';
        return true;
    }
    return false;
}

static bool edit_port(uint16_t *port, const char *hint)
{
    char value[8];
    snprintf(value, sizeof(value), "%u", *port);
    if (!edit_text(value, sizeof(value), hint, SWKBD_TYPE_NUMPAD, false)) return false;
    long parsed = strtol(value, NULL, 10);
    if (parsed <= 0 || parsed > 65535) return false;
    *port = (uint16_t)parsed;
    return true;
}

int main(void)
{
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    gfxSet3D(false);

    AppConfig config;
    bool loaded = config_load(&config);
    char message[80] = "";
    bool video_ok = video_init();
    if (!video_ok) snprintf(message, sizeof(message), "VIDEO INIT FAILED");
    if (video_ok) {
        /* Initialise both top-screen buffers before the normal double-
         * buffered loop starts, so a disconnected client shows black
         * instead of alternating with uninitialised VRAM. */
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_TOP, false);
        gspWaitForVBlank();
        video_clear();
    }
    bool network_ok = network_init();
    if (!network_ok) snprintf(message, sizeof(message), "NETWORK INIT FAILED");
    bool audio_ok = audio_init();
    if (!audio_ok) snprintf(message, sizeof(message), "AUDIO UNAVAILABLE - VIDEO STILL WORKS");

    if (network_ok) {
        network_apply_config(&config);
        if (config.auto_connect) network_connect();
    }
    bool controller_mode = loaded && config.auto_connect;
    TouchUiEvent ui_event;
    memset(&ui_event, 0, sizeof(ui_event));
    uint32_t last_input_ms = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 keys_down = hidKeysDown();
        u32 keys_held = hidKeysHeld();
        u32 keys_up = hidKeysUp();
        touchPosition touch = {0, 0};
        if (keys_down & KEY_TOUCH || keys_held & KEY_TOUCH) hidTouchRead(&touch);

        NetworkStats network_stats;
        memset(&network_stats, 0, sizeof(network_stats));
        if (network_ok) network_get_stats(&network_stats);

        if (controller_mode) {
            touch_ui_update_controller(keys_down, keys_held, keys_up, &touch,
                                       config.right_stick_deadzone, &ui_event);
            if (ui_event.home_pressed && network_stats.may_control) network_send_home();
            if (ui_event.capture_pressed && network_stats.may_control) network_send_capture();
            if (ui_event.toggle_stats) config.stats_enabled = !config.stats_enabled;
            if (ui_event.toggle_codec) {
                config.video_codec = config.video_codec == C2S_CODEC_OLD3DS_JPEG
                    ? C2S_CODEC_OLD3DS_MPEG4 : C2S_CODEC_OLD3DS_JPEG;
                network_request_codec(config.video_codec);
                config_save(&config);
            }
            if (ui_event.open_config) {
                controller_mode = false;
                int8_t released[C2S_PAD_SLOTS] = {0};
                network_send_input(released);
            }
        } else {
            ConfigAction action = touch_ui_config_action(keys_down, &touch);
            bool changed = false;
            switch (action) {
                case CONFIG_EDIT_HOST:
                    changed = edit_text(config.host, sizeof(config.host),
                                        "Host name or IPv4 address", SWKBD_TYPE_QWERTY, false);
                    break;
                case CONFIG_EDIT_STREAM_PORT:
                    changed = edit_port(&config.stream_port, "OLD3DS stream port");
                    break;
                case CONFIG_EDIT_WEB_PORT:
                    changed = edit_port(&config.web_port, "Web login port");
                    break;
                case CONFIG_EDIT_PASSWORD:
                    changed = edit_text(config.password, sizeof(config.password),
                                        "Server password (empty if disabled)",
                                        SWKBD_TYPE_NORMAL, true);
                    break;
                case CONFIG_TOGGLE_AUTO:
                    config.auto_connect = !config.auto_connect;
                    changed = true;
                    break;
                case CONFIG_TOGGLE_AUDIO:
                    config.audio_enabled = !config.audio_enabled;
                    if (!config.audio_enabled) audio_clear();
                    changed = true;
                    break;
                case CONFIG_TOGGLE_CODEC:
                    config.video_codec = config.video_codec == C2S_CODEC_OLD3DS_JPEG
                        ? C2S_CODEC_OLD3DS_MPEG4 : C2S_CODEC_OLD3DS_JPEG;
                    network_request_codec(config.video_codec);
                    changed = true;
                    break;
                case CONFIG_SAVE:
                    snprintf(message, sizeof(message), "%s",
                             config_save(&config) ? "CONFIG SAVED" : "SAVE FAILED");
                    break;
                case CONFIG_CONNECT:
                    if (network_ok) {
                        network_apply_config(&config);
                        network_connect();
                        snprintf(message, sizeof(message), "CONNECTING");
                    }
                    break;
                case CONFIG_RECONNECT:
                    if (network_ok) {
                        network_apply_config(&config);
                        network_reconnect();
                        snprintf(message, sizeof(message), "RECONNECTING");
                    }
                    break;
                case CONFIG_DISCONNECT:
                    if (network_ok) network_disconnect();
                    audio_clear();
                    snprintf(message, sizeof(message), "DISCONNECTED");
                    break;
                case CONFIG_CONTROLLER:
                    controller_mode = true;
                    break;
                default: break;
            }
            if (changed && network_ok) network_apply_config(&config);
        }

        const uint8_t *jpeg = NULL;
        uint32_t jpeg_size = 0;
        uint32_t received_ms = 0;
        uint8_t video_codec = C2S_CODEC_OLD3DS_JPEG;
        int video_slot = -1;
        if (network_ok && audio_ok && config.audio_enabled) audio_service(true);
        if (network_ok && video_ok && network_acquire_video(&jpeg, &jpeg_size,
                                                &received_ms, &video_codec, &video_slot)) {
            video_note_received_bytes(jpeg_size);
            if (!video_decode_and_present(jpeg, jpeg_size, received_ms, video_codec) &&
                video_codec == C2S_CODEC_OLD3DS_MPEG4) {
                network_request_keyframe();
            }
            network_release_video(video_slot);
        }
        if (network_ok && audio_ok && config.audio_enabled) audio_service(true);

        uint32_t now = osGetTime();
        if (network_ok && now - last_input_ms >= 33) {
            int8_t state[C2S_PAD_SLOTS];
            input_build_state(state, &ui_event.controls, controller_mode);
            network_send_input(state);
            last_input_ms = now;
        }

        VideoStats video_stats;
        AudioStats audio_stats;
        video_get_stats(&video_stats);
        audio_get_stats(&audio_stats);
        if (controller_mode) {
            touch_ui_draw_controller(&ui_event, &network_stats, &video_stats,
                                     &audio_stats, config.stats_enabled,
                                     config.video_codec);
        } else {
            touch_ui_draw_config(&config, &network_stats, message);
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    if (network_ok) {
        int8_t released[C2S_PAD_SLOTS] = {0};
        network_send_input(released);
        network_exit();
    }
    if (audio_ok) audio_exit();
    if (video_ok) video_exit();
    gfxExit();
    return 0;
}
