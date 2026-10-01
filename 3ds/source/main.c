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

static Thread g_video_thread;
static LightLock g_video_present_lock;
static volatile bool g_video_thread_running;
static volatile bool g_video_frame_ready;

static void video_worker(void *unused)
{
    (void)unused;
    while (g_video_thread_running) {
        LightLock_Lock(&g_video_present_lock);
        const bool waiting_for_swap = g_video_frame_ready;
        LightLock_Unlock(&g_video_present_lock);
        if (waiting_for_swap) {
            svcSleepThread(500000LL);
            continue;
        }

        bool did_work = false;
        for (int video_work = 0; g_video_thread_running && video_work < 4;
             video_work++) {
            const uint8_t *frame = NULL;
            uint32_t frame_size = 0;
            uint32_t received_ms = 0;
            uint8_t video_codec = C2S_CODEC_OLD3DS_JPEG;
            int video_slot = -1;
            if (!network_acquire_video(&frame, &frame_size, &received_ms,
                                       &video_codec, &video_slot)) break;

            NetworkStats queued_stats;
            network_get_stats(&queued_stats);
            const bool predictive = c2s_old3ds_predictive_codec(video_codec);
            const bool present = !predictive || queued_stats.queue_depth == 0 ||
                                 video_work == 3;
            video_note_received_bytes(frame_size);
            const bool decoded = video_decode_and_present(
                frame, frame_size, received_ms, video_codec, present);
            network_release_video(video_slot);
            did_work = true;
            if (!decoded && predictive) {
                network_request_keyframe();
                break;
            }
            if (decoded && present) {
                LightLock_Lock(&g_video_present_lock);
                g_video_frame_ready = true;
                LightLock_Unlock(&g_video_present_lock);
                break;
            }
            if (present || !predictive) break;
        }
        if (!did_work) svcSleepThread(500000LL);
    }
}

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

static uint8_t next_video_codec(uint8_t codec)
{
    if (codec == C2S_CODEC_OLD3DS_JPEG) return C2S_CODEC_OLD3DS_MPEG4;
    if (codec == C2S_CODEC_OLD3DS_MPEG4) return C2S_CODEC_OLD3DS_MPEG1;
    if (codec == C2S_CODEC_OLD3DS_MPEG1) return C2S_CODEC_OLD3DS_MPEG2;
    return C2S_CODEC_OLD3DS_JPEG;
}

int main(void)
{
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    gfxSet3D(false);

    /* Old 3DS reserves CPU1 for the system by default.  Giving the app
     * 70% of that core lets the network worker run there while MPEG-4
     * decoding and presentation stay on the application core.  This is
     * the Old 3DS value used by Video_player_for_3DS; 80% is its New 3DS
     * setting. */
    APT_SetAppCpuTimeLimit(70);

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
    if (audio_ok) audio_service(config.audio_enabled);

    if (network_ok) {
        network_apply_config(&config);
        if (config.auto_connect) network_connect();
    }

    LightLock_Init(&g_video_present_lock);
    bool video_thread_ok = false;
    if (network_ok && video_ok) {
        g_video_thread_running = true;
        /* Decode on the application core while the main/UI thread sleeps on
         * VBlank.  The worker stops after preparing one backbuffer; only the
         * main thread swaps it, so top and bottom retain clean VBlank timing. */
        g_video_thread = threadCreate(video_worker, NULL, 128 * 1024,
                                      0x2f, 0, false);
        if (!g_video_thread) {
            g_video_thread = threadCreate(video_worker, NULL, 128 * 1024,
                                          0x2f, -2, false);
        }
        video_thread_ok = g_video_thread != NULL;
        if (!video_thread_ok) g_video_thread_running = false;
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
                config.video_codec = next_video_codec(config.video_codec);
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
                    config.video_codec = next_video_codec(config.video_codec);
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

        if (audio_ok) audio_service(config.audio_enabled);
        /* Fallback for an environment where the worker could not be created.
         * It preserves a usable picture rather than a black top LCD. */
        bool fallback_video_presented = false;
        if (!video_thread_ok && network_ok && video_ok) {
            const uint8_t *frame = NULL;
            uint32_t frame_size = 0;
            uint32_t received_ms = 0;
            uint8_t video_codec = C2S_CODEC_OLD3DS_JPEG;
            int video_slot = -1;
            if (network_acquire_video(&frame, &frame_size, &received_ms,
                                      &video_codec, &video_slot)) {
                video_note_received_bytes(frame_size);
                fallback_video_presented = video_decode_and_present(
                    frame, frame_size, received_ms, video_codec, true);
                network_release_video(video_slot);
                if (!fallback_video_presented &&
                    c2s_old3ds_predictive_codec(video_codec)) {
                    network_request_keyframe();
                }
            }
        }
        uint32_t now = osGetTime();
        if (network_ok && network_stats.state == NETWORK_CONNECTED &&
            now - last_input_ms >= 33) {
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

        LightLock_Lock(&g_video_present_lock);
        gfxFlushBuffers();
        /* The controller UI is redrawn every loop, but video arrives at
         * 30 FPS while the LCD refreshes at 60 Hz.  Swapping the top screen
         * on an empty video iteration alternated the freshly decoded frame
         * with the stale contents of the other framebuffer, which looked
         * like severe video stutter.  Keep the current top framebuffer on
         * screen until a replacement frame has actually been decoded. */
        if (g_video_frame_ready || fallback_video_presented) {
            gfxScreenSwapBuffers(GFX_TOP, false);
            g_video_frame_ready = false;
        }
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
        LightLock_Unlock(&g_video_present_lock);
        gspWaitForVBlank();
    }

    if (network_ok) {
        int8_t released[C2S_PAD_SLOTS] = {0};
        network_send_input(released);
    }
    g_video_thread_running = false;
    if (g_video_thread) {
        threadJoin(g_video_thread, U64_MAX);
        threadFree(g_video_thread);
        g_video_thread = NULL;
    }
    /* Stop and join the audio worker while the network queue and its lock
     * still exist.  The worker calls network_clear_audio() as part of its
     * teardown, so destroying networking first was the wrong lifetime. */
    if (audio_ok) audio_exit();
    if (network_ok) network_exit();
    if (video_ok) video_exit();
    APT_SetAppCpuTimeLimit(10);
    gfxExit();
    return 0;
}
