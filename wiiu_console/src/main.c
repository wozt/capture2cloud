/*
 * capture2cloud on a Wii U console.
 *
 * Connects to the host, decodes H.264 with H264DEC, displays its NV12
 * output directly through GX2, plays PCM through AX, and forwards the
 * Wii U GamePad to the remote console.
 *
 * Rebuilt on SDL2 after four probes settled the one thing that decides
 * the shape of this program: OSScreen and GX2 cannot both be used. Once
 * OSScreen has been up, GX2 renders nothing at all -- WHBGfxInit still
 * returns 1, frames still count, and the screen stays black. Everything
 * that matters here is GX2 (the console's keyboard, the HOME overlay,
 * and an NV12 video texture), so the program is GX2 from its first
 * line. SDL2's Wii U renderer is GX2, and it brings text and touch
 * without a shader assembler, which this toolchain does not have.
 *
 * Nothing touches the network until CONNECT is pressed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <whb/log.h>
#include <whb/log_udp.h>

#include "c2s_protocol.h"
#include "gx2_video.h"
#include "audio.h"
#include "input.h"
#include "keyboard.h"
#include "menu.h"
#include "net.h"
#include "proc.h"
#include "settings.h"
#include "ui.h"
#include "video.h"
#include "video_worker.h"

/* The biggest picture the decoder reserves for. 720p60 is the path this
 * is built and measured on; see SPEC.md on why 1080p is offered rather
 * than supported. */
#define MAX_WIDTH  1280
#define MAX_HEIGHT 720

/*
 * The password exists only in RAM long enough to exchange it for a
 * session token. It is never written to the SD card.
 */
#define PASSWORD_CAP 96

typedef enum { STATE_SETTINGS, STATE_STREAMING } State;

static unsigned video_worker_decoded_total(void)
{
    VideoWorkerStats stats;
    video_worker_stats(&stats);
    return stats.decoded;
}


static int parse_host(const char *text, void *target)
{
    return settings_set_host_string((Settings *)target, text);
}

static int parse_port(const char *text, void *target)
{
    const int v = atoi(text);
    if (v <= 0 || v > 65535) {
        return -1;
    }
    *(uint16_t *)target = (uint16_t)v;
    return 0;
}

/* Opens the console's keyboard for one field and puts the answer back.
 * `parse` returns 0 when the text was acceptable. */
static void edit_field(
    const char *hint,
    const char *current,
    int numeric,
    char *note,
    size_t note_size,
    int (*parse)(const char *, void *),
    void *target)
{
    char typed[64];
    char why[96];

    const int r =
        keyboard_prompt(
            hint,
            current,
            numeric,
            typed,
            sizeof(typed),
            why,
            sizeof(why));

    if (r < 0) {
        snprintf(
            note,
            note_size,
            "%s",
            why);

    } else if (r == 1) {

        if (parse(
                typed,
                target) != 0) {

            snprintf(
                note,
                note_size,
                "not a %s: %s",
                hint,
                typed);

        } else {
            note[0] = '\0';
        }
    }
}


static void edit_password(
    char password[PASSWORD_CAP],
    char *note,
    size_t note_size)
{
    char typed[PASSWORD_CAP];
    char why[96];

    /*
     * Do not pre-fill the keyboard with the existing password.
     * It stays in RAM but is never redisplayed in plain text.
     */
    const int r =
        keyboard_prompt(
            "capture2cloud password",
            NULL,
            0,
            typed,
            sizeof(typed),
            why,
            sizeof(why));

    if (r < 0) {
        snprintf(
            note,
            note_size,
            "%s",
            why);

        return;
    }

    if (r == 0) {
        return;
    }

    snprintf(
        password,
        PASSWORD_CAP,
        "%s",
        typed);

    if (password[0]) {
        snprintf(
            note,
            note_size,
            "password ready - press CONNECT");

    } else {
        snprintf(
            note,
            note_size,
            "password cleared");
    }
}


/*
 * swkbd owns and modifies raw GX2 state while it is visible.  The Wii U
 * SDL renderer caches that state internally and has no public way to
 * invalidate it afterwards.  Recreate the display and controller just as
 * we already do after the HOME overlay; otherwise cached glyph textures
 * are drawn with swkbd's colour shader and become solid rectangles.
 */
static int rebuild_ui_after_keyboard(
    int *ui_alive,
    int *input_alive,
    const Settings *settings,
    char *why,
    size_t why_size)
{
    WHBLogPrintf("swkbd: rebuilding SDL/GX2 renderer");

    if (*input_alive) {
        input_exit();
        *input_alive = 0;
    }

    if (*ui_alive) {
        ui_shutdown();
        *ui_alive = 0;
    }

    why[0] = '\0';
    if (ui_init(why, why_size) != 0) {
        WHBLogPrintf("swkbd: UI rebuild failed -- %s", why);
        return -1;
    }
    *ui_alive = 1;

    char input_why[96] = { 0 };
    *input_alive =
        input_init(input_why, sizeof(input_why)) == 0;

    if (*input_alive) {
        input_set_config(&settings->input);
    }

    WHBLogPrintf(
        "swkbd: renderer rebuilt, input %s",
        *input_alive ? "ready" : input_why);

    return 0;
}


/*
 * Exchange a password for the host's temporary session token.
 *
 * Returns 1 when connection may continue.
 * Returns 0 on login failure; the existing stream is left untouched.
 *
 * An empty password means "do not login now": use the saved token if
 * one exists, otherwise connect as a viewer.
 */
static int authenticate_if_needed(
    Settings *settings,
    char password[PASSWORD_CAP],
    char *note,
    size_t note_size)
{
    if (!password[0]) {
        return 1;
    }

    char host[32];
    char token[C2S_MAX_TOKEN_LEN + 1];
    char error[96];

    settings_host_string(
        settings,
        host,
        sizeof(host));

    token[0] = '\0';
    error[0] = '\0';

    if (!net_login(
            host,
            settings->web_port,
            password,
            token,
            sizeof(token),
            error,
            sizeof(error))) {

        snprintf(
            note,
            note_size,
            "login: %s",
            error);

        return 0;
    }

    snprintf(
        settings->token,
        sizeof(settings->token),
        "%s",
        token);

    /*
     * Password has served its purpose. Keep only the token.
     */
    memset(
        password,
        0,
        PASSWORD_CAP);

    snprintf(
        note,
        note_size,
        "login OK - player token received");

    return 1;
}


int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBLogUdpInit();
    WHBLogPrintf("capture2cloud: starting");
    proc_init();

    char why[128] = { 0 };
    if (ui_init(why, sizeof(why)) != 0) {
        WHBLogPrintf("capture2cloud: no interface -- %s", why);
        ui_shutdown();
        proc_shutdown();
        WHBLogUdpDeinit();
        return 1;
    }

    char input_why[96] = { 0 };

    int input_alive =
        input_init(
            input_why,
            sizeof(input_why)) == 0;

    WHBLogPrintf(
        "capture2cloud: input %s",
        input_alive
            ? "Wii U GamePad ready"
            : input_why);

    Settings settings;
    settings_load(&settings);
    input_set_config(&settings.input);

    char decoder_why[128] = { 0 };

    int decoder_ok =
        video_init(
            MAX_WIDTH,
            MAX_HEIGHT,
            decoder_why,
            sizeof(decoder_why)) == 0;

    int video_worker_alive = 0;

    if (decoder_ok) {
        if (video_worker_start(
                decoder_why,
                sizeof(decoder_why)) == 0) {
            video_worker_alive = 1;
        } else {
            video_exit();
            decoder_ok = 0;
        }
    }

    WHBLogPrintf(
        "capture2cloud: decoder %s",
        decoder_ok
            ? "ready + threaded"
            : decoder_why);

    char audio_why[128] = { 0 };

    /*
     * Audio is intentionally NOT opened before CONNECT.
     *
     * This also makes the settings screen a clean test: if anything is
     * audible there, it cannot come from Capture2Cloud audio.
     */
    int audio_ok = 1;

    WHBLogPrintf(
        "capture2cloud: audio deferred until CONNECT");

    int ui_alive = 1;
    int video_alive = decoder_ok ? 1 : 0;
    int audio_alive = 0;

    State state = STATE_SETTINGS;

    char note[160] = { 0 };

    char password[PASSWORD_CAP] = { 0 };

    UiInput in;
    memset(&in, 0, sizeof(in));

    VideoFrame frame;
    int have_frame = 0;
    int new_frame = 0;
    MenuState menu;
    menu_init(&menu);
    MenuPerf perf;
    memset(&perf, 0, sizeof(perf));

    unsigned rx_count = 0;
    unsigned display_count = 0;
    unsigned loop_count = 0;

    unsigned worker_decoded_at =
        video_worker_decoded_total();

    int video_synced = 0;
    int keyframe_requested = 0;

    uint32_t fps_at = SDL_GetTicks();
    uint64_t rx_bytes_at = 0;

    while (proc_running()) {
        new_frame = 0;

        /*
         * ProcUI has asked us to release the foreground.
         *
         * THIS ORDER IS REQUIRED:
         *
         *   stop/drain application work
         *   -> H264DECEnd/H264DECClose
         *   -> SDL/GX2 shutdown
         *   -> ProcUIDrawDoneRelease
         *
         * Previously DrawDoneRelease happened first, and H264DECEnd
         * then hung permanently while returning to the Wii U menu.
         */
        if (proc_release_pending()) {
            WHBLogPrintf("suspend 1/3: disconnect network stream");

            net_disconnect();
            state = STATE_SETTINGS;
            menu_force_open(&menu, 1);
            have_frame = 0;

            WHBLogPrintf("suspend 2/4: audio begin");

            if (audio_alive) {
                audio_exit();
                audio_alive = 0;
            }

            WHBLogPrintf("suspend 2/4: audio done");

            WHBLogPrintf("suspend 3/4: H264DEC begin");

            if (video_worker_alive) {
                video_worker_stop();
                video_worker_alive = 0;
            }

            if (video_alive) {
                video_exit();
                video_alive = 0;
            }

            WHBLogPrintf("suspend 3/4: H264DEC done");
            WHBLogPrintf("suspend 4/4: SDL/GX2 begin");

            if (input_alive) {
                input_exit();
                input_alive = 0;
            }

            if (ui_alive) {
                ui_shutdown();
                ui_alive = 0;
            }

            WHBLogPrintf("suspend 4/4: SDL/GX2 done");

            /*
             * All foreground resources are now gone. This call performs
             * ProcUIDrawDoneRelease and waits while HOME owns the screen.
             */
            if (!proc_release_and_wait()) {
                break;  /* HOME -> Quitter */
            }

            /*
             * User closed HOME instead of quitting: rebuild everything
             * that had to be surrendered.
             */
            WHBLogPrintf("resume: rebuilding SDL/GX2");

            why[0] = '\0';
            if (ui_init(why, sizeof(why)) != 0) {
                WHBLogPrintf("resume: UI failed -- %s", why);
                break;
            }

            ui_alive = 1;

            input_why[0] = '\0';

            input_alive =
                input_init(
                    input_why,
                    sizeof(input_why)) == 0;

            WHBLogPrintf(
                "resume: input %s",
                input_alive
                    ? "Wii U GamePad ready"
                    : input_why);

            audio_ok = 1;
            audio_alive = 0;

            WHBLogPrintf(
                "resume: audio deferred until CONNECT");

            WHBLogPrintf("resume: rebuilding H264DEC");

            decoder_why[0] = '\0';

            decoder_ok =
                video_init(
                    MAX_WIDTH,
                    MAX_HEIGHT,
                    decoder_why,
                    sizeof(decoder_why)) == 0;

            video_alive =
                decoder_ok ? 1 : 0;

            video_worker_alive = 0;

            if (decoder_ok) {
                if (video_worker_start(
                        decoder_why,
                        sizeof(decoder_why)) == 0) {
                    video_worker_alive = 1;
                } else {
                    video_exit();
                    video_alive = 0;
                    decoder_ok = 0;
                }
            }

            WHBLogPrintf(
                "resume: decoder %s",
                decoder_ok
                    ? "ready + threaded"
                    : decoder_why);

            memset(&in, 0, sizeof(in));
            memset(&perf, 0, sizeof(perf));

            rx_count = 0;
            display_count = 0;
            loop_count = 0;

            worker_decoded_at =
                video_worker_decoded_total();

            video_synced = 0;
            keyframe_requested = 0;

            fps_at = SDL_GetTicks();
            rx_bytes_at = net_info()->rx_bytes;

            continue;
        }

        ui_poll(&in);

        /*
         * Forward physical controls only while actually playing.
         *
         * Opening the local menu immediately neutralises the remote pad
         * while we continue sampling it for the diagnostics screen.
         */
        if (input_alive) {
            input_update(
                state == STATE_STREAMING &&
                !menu_is_open(&menu));
        }

        if (in.quit) {
            proc_stop();
        }

        const MenuAction menu_action =
            menu_input(
                &menu,
                &in,
                state == STATE_STREAMING);

        if (menu_action == MENU_ACTION_RESOLUTION ||
            menu_action == MENU_ACTION_FRAME_RATE ||
            menu_action == MENU_ACTION_BITRATE) {

            unsigned width, height, fps, bitrate;
            if (menu_change_stream(
                    &menu,
                    menu_action,
                    &width,
                    &height,
                    &fps,
                    &bitrate)) {

                if (net_info()->state == NET_CONNECTED) {
                    net_send_profile(width, height, fps, bitrate);
                    (void)menu_take_profile_dirty(
                        &menu, NULL, NULL, NULL, NULL);
                    snprintf(note, sizeof(note),
                             "%ux%u %u fps %u kbps requested",
                             width, height, fps, bitrate);
                } else {
                    snprintf(note, sizeof(note),
                             "profile selected; it will be sent after connect");
                }
            }
        }

        if (menu_action == MENU_ACTION_LEFT_DEADZONE ||
            menu_action == MENU_ACTION_LEFT_RANGE ||
            menu_action == MENU_ACTION_RIGHT_DEADZONE ||
            menu_action == MENU_ACTION_RIGHT_RANGE ||
            menu_action == MENU_ACTION_INVERT_Y ||
            menu_action == MENU_ACTION_FACE_MAPPING) {

            if (menu_action == MENU_ACTION_LEFT_DEADZONE ||
                menu_action == MENU_ACTION_RIGHT_DEADZONE) {
                const int stick =
                    menu_action == MENU_ACTION_LEFT_DEADZONE ? 0 : 1;
                unsigned value = settings.input.deadzone[stick] + 2;
                settings.input.deadzone[stick] = value > 25 ? 0 : value;
            } else if (menu_action == MENU_ACTION_LEFT_RANGE ||
                       menu_action == MENU_ACTION_RIGHT_RANGE) {
                const int stick =
                    menu_action == MENU_ACTION_LEFT_RANGE ? 0 : 1;
                unsigned value = settings.input.range[stick];
                settings.input.range[stick] = value <= 45 ? 100 : value - 5;
            } else if (menu_action == MENU_ACTION_INVERT_Y) {
                settings.input.invert_y = !settings.input.invert_y;
            } else {
                settings.input.face_by_position =
                    !settings.input.face_by_position;
            }

            input_set_config(&settings.input);

            char save_why[96];
            if (settings_save(
                    &settings,
                    save_why,
                    sizeof(save_why)) != 0) {
                snprintf(note, sizeof(note), "not saved: %s", save_why);
            } else {
                snprintf(note, sizeof(note), "controller setting saved");
            }
        }

        if (state == STATE_SETTINGS) {
            if (menu_action == MENU_ACTION_HOST) {

                    char current[32];

                    settings_host_string(
                        &settings,
                        current,
                        sizeof(current));

                    edit_field(
                        "host",
                        current,
                        1,
                        note,
                        sizeof(note),
                        parse_host,
                        &settings);

                    if (rebuild_ui_after_keyboard(
                            &ui_alive, &input_alive, &settings,
                            why, sizeof(why)) != 0) {
                        proc_stop();
                        continue;
                    }
                    memset(&in, 0, sizeof(in));

            } else if (menu_action == MENU_ACTION_NATIVE_PORT) {

                    char current[16];

                    snprintf(
                        current,
                        sizeof(current),
                        "%u",
                        settings.port);

                    edit_field(
                        "native port",
                        current,
                        1,
                        note,
                        sizeof(note),
                        parse_port,
                        &settings.port);

                    if (rebuild_ui_after_keyboard(
                            &ui_alive, &input_alive, &settings,
                            why, sizeof(why)) != 0) {
                        proc_stop();
                        continue;
                    }
                    memset(&in, 0, sizeof(in));

            } else if (menu_action == MENU_ACTION_WEB_PORT) {

                    char current[16];

                    snprintf(
                        current,
                        sizeof(current),
                        "%u",
                        settings.web_port);

                    edit_field(
                        "web port",
                        current,
                        1,
                        note,
                        sizeof(note),
                        parse_port,
                        &settings.web_port);

                    if (rebuild_ui_after_keyboard(
                            &ui_alive, &input_alive, &settings,
                            why, sizeof(why)) != 0) {
                        proc_stop();
                        continue;
                    }
                    memset(&in, 0, sizeof(in));

            } else if (menu_action == MENU_ACTION_PASSWORD) {

                    edit_password(
                        password,
                        note,
                        sizeof(note));

                    if (rebuild_ui_after_keyboard(
                            &ui_alive, &input_alive, &settings,
                            why, sizeof(why)) != 0) {
                        proc_stop();
                        continue;
                    }
                    memset(&in, 0, sizeof(in));

            } else if (menu_action == MENU_ACTION_REMOTE_HOME ||
                       menu_action == MENU_ACTION_WAKE ||
                       menu_action == MENU_ACTION_RESET_DONGLE ||
                       menu_action == MENU_ACTION_RESTART_HOST) {

                    const NetInfo *info =
                        net_info();

                    if (info->state ==
                            NET_CONNECTED &&
                        info->may_control) {

                        if (menu_action == MENU_ACTION_REMOTE_HOME)
                            net_send_home();
                        else if (menu_action == MENU_ACTION_WAKE)
                            net_send_wake();
                        else if (menu_action == MENU_ACTION_RESET_DONGLE)
                            net_send_reset_dongle();
                        else
                            net_send_restart();

                        snprintf(
                            note,
                            sizeof(note),
                            "%s sent",
                            menu_action == MENU_ACTION_REMOTE_HOME ? "remote HOME" :
                            menu_action == MENU_ACTION_WAKE ? "wake request" :
                            menu_action == MENU_ACTION_RESET_DONGLE ? "adapter reset" :
                            "host restart");

                    } else {
                        snprintf(
                            note,
                            sizeof(note),
                            "remote commands require CONTROL");
                    }

            } else if (menu_action == MENU_ACTION_EXIT_HELP) {

                    snprintf(
                        note,
                        sizeof(note),
                        "Press HOME, then choose Quitter");

            } else if (menu_action == MENU_ACTION_CONNECT) {

                    if (!decoder_ok) {

                        snprintf(
                            note,
                            sizeof(note),
                            "no decoder: %s",
                            decoder_why);

                    } else if (
                        settings.host[0] == 0) {

                        snprintf(
                            note,
                            sizeof(note),
                            "set the host address first");

                    } else if (
                        net_init() != 0) {

                        snprintf(
                            note,
                            sizeof(note),
                            "%s",
                            net_info()->status);

                    } else if (
                        authenticate_if_needed(
                            &settings,
                            password,
                            note,
                            sizeof(note))) {

                        char host[32];
                        char save_why[96];

                        settings_host_string(
                            &settings,
                            host,
                            sizeof(host));

                        if (!audio_alive) {
                            audio_why[0] = '\0';

                            audio_ok =
                                audio_init(
                                    48000,
                                    2,
                                    audio_why,
                                    sizeof(audio_why)) == 0;

                            audio_alive =
                                audio_ok
                                    ? 1
                                    : 0;

                            WHBLogPrintf(
                                "capture2cloud: AX audio %s",
                                audio_ok
                                    ? "ready"
                                    : audio_why);
                        }

                        if (settings_save(
                                &settings,
                                save_why,
                                sizeof(save_why)) != 0) {

                            snprintf(
                                note,
                                sizeof(note),
                                "not saved: %s",
                                save_why);
                        }

                        net_connect(
                            host,
                            settings.port,
                            settings.token[0]
                                ? settings.token
                                : NULL);

                        state =
                            STATE_STREAMING;

                        menu_force_open(&menu, 0);

                        memset(
                            &perf,
                            0,
                            sizeof(perf));

                        rx_count = 0;
                        display_count = 0;
                        loop_count = 0;

                        worker_decoded_at =
                            video_worker_decoded_total();

                        video_synced = 0;
                        keyframe_requested = 0;

                        fps_at =
                            SDL_GetTicks();

                        rx_bytes_at =
                            net_info()->rx_bytes;

                        WHBLogPrintf(
                            "capture2cloud: connecting to %s:%u auth=%s",
                            host,
                            settings.port,
                                settings.token[0]
                                    ? "token"
                                    : "viewer");
                    }
            }
        } else {
            /*
             * Drain low-latency PCM before processing large H264 TCP
             * frames.
             */
            {
                const uint8_t *audio_payload;
                uint32_t audio_size;

                while (net_take_audio(
                           &audio_payload,
                           &audio_size)) {

                    if (audio_alive) {
                        audio_push_pcm_s16le(
                            audio_payload,
                            audio_size);
                    }
                }
            }

            net_poll();

            if (net_info()->state == NET_CONNECTED) {
                unsigned width, height, fps, bitrate;
                if (menu_take_profile_dirty(
                        &menu,
                        &width,
                        &height,
                        &fps,
                        &bitrate)) {
                    net_send_profile(width, height, fps, bitrate);
                    snprintf(note, sizeof(note),
                             "%ux%u %u fps %u kbps requested",
                             width, height, fps, bitrate);
                }
            }

            /*
             * Every frame that has arrived, not just one. The decoder is
             * faster than this loop, and a queue drained one frame per
             * iteration grows without bound the moment drawing falls
             * behind. Only the newest is kept.
             */
            const uint8_t *payload;
            uint32_t size;
            uint8_t flags;
            int kind;
            while ((kind = net_take_frame(&payload, &size, &flags)) != 0) {
                if (kind == C2S_MSG_SHARED &&
                    size == sizeof(C2sShared)) {

                    C2sShared shared;
                    memcpy(&shared, payload, sizeof(shared));

                    menu_adopt_stream(
                        &menu,
                        c2s_le16(shared.width),
                        c2s_le16(shared.height),
                        c2s_le16(shared.fps),
                        c2s_le16(shared.bitrate_kbps));

                    continue;
                }

                if (kind == C2S_MSG_STREAM_INFO &&
                    size == sizeof(C2sStreamInfo)) {

                    C2sStreamInfo stream;
                    memcpy(&stream, payload, sizeof(stream));

                    net_set_stream_info(
                        c2s_le16(stream.width),
                        c2s_le16(stream.height),
                        stream.video_codec);

                    video_synced = 0;
                    keyframe_requested = 0;
                    continue;
                }

                if (kind == C2S_MSG_AUDIO) {
                    /*
                     * TCP PCM is only the compatibility fallback.
                     */
                    if (audio_alive &&
                        net_info()->audio_codec ==
                            C2S_CODEC_PCM_S16LE &&
                        !net_info()->audio_udp) {

                        audio_push_pcm_s16le(
                            payload,
                            size);
                    }

                    continue;
                }

                if (kind != C2S_MSG_VIDEO) {
                    continue;
                }

                rx_count++;

                /*
                 * If the bounded queue ever overflows we have skipped a
                 * predictive H.264 AU. Do not feed dependent pictures to
                 * the decoder afterwards: wait for the next IDR.
                 */
                if (!video_synced) {
                    if (!(flags & C2S_FLAG_KEYFRAME)) {
                        if (!keyframe_requested) {
                            net_send_keyframe_request();
                            keyframe_requested = 1;
                        }

                        continue;
                    }

                    video_synced = 1;
                    keyframe_requested = 0;
                }

                const int queued =
                    video_worker_submit_wait(
                        payload,
                        size,
                        12000);

                if (queued <= 0) {
                    video_synced = 0;

                    if (!keyframe_requested) {
                        net_send_keyframe_request();
                        keyframe_requested = 1;
                    }
                }
            }

            /*
             * H264DEC runs independently now. Take only the newest
             * completed picture and immediately hand its NV12 buffer to
             * GX2.
             */
            /*
             * SDL_RenderPresent() normally waits most of the remaining
             * 16.7 ms VBlank interval. If H264DEC is already working on
             * the next AU, spend at most 2 ms of that otherwise-idle
             * time here so decode and presentation do not continually
             * miss each other by a fraction of a millisecond.
             */
            if (video_worker_alive &&
                video_worker_take_wait(
                    &frame,
                    2000)) {

                have_frame = 1;
                new_frame = 1;
            }

            if (menu_action == MENU_ACTION_HOST) {

                        char current[32];

                        settings_host_string(
                            &settings,
                            current,
                            sizeof(current));

                        edit_field(
                            "host",
                            current,
                            1,
                            note,
                            sizeof(note),
                            parse_host,
                            &settings);

                        if (rebuild_ui_after_keyboard(
                                &ui_alive, &input_alive, &settings,
                                why, sizeof(why)) != 0) {
                            proc_stop();
                            continue;
                        }
                        memset(&in, 0, sizeof(in));

            } else if (menu_action == MENU_ACTION_NATIVE_PORT) {

                        char current[16];

                        snprintf(
                            current,
                            sizeof(current),
                            "%u",
                            settings.port);

                        edit_field(
                            "native port",
                            current,
                            1,
                            note,
                            sizeof(note),
                            parse_port,
                            &settings.port);

                        if (rebuild_ui_after_keyboard(
                                &ui_alive, &input_alive, &settings,
                                why, sizeof(why)) != 0) {
                            proc_stop();
                            continue;
                        }
                        memset(&in, 0, sizeof(in));

            } else if (menu_action == MENU_ACTION_WEB_PORT) {

                        char current[16];

                        snprintf(
                            current,
                            sizeof(current),
                            "%u",
                            settings.web_port);

                        edit_field(
                            "web port",
                            current,
                            1,
                            note,
                            sizeof(note),
                            parse_port,
                            &settings.web_port);

                        if (rebuild_ui_after_keyboard(
                                &ui_alive, &input_alive, &settings,
                                why, sizeof(why)) != 0) {
                            proc_stop();
                            continue;
                        }
                        memset(&in, 0, sizeof(in));

            } else if (menu_action == MENU_ACTION_PASSWORD) {

                        edit_password(
                            password,
                            note,
                            sizeof(note));

                        if (rebuild_ui_after_keyboard(
                                &ui_alive, &input_alive, &settings,
                                why, sizeof(why)) != 0) {
                            proc_stop();
                            continue;
                        }
                        memset(&in, 0, sizeof(in));

            } else if (menu_action == MENU_ACTION_REMOTE_HOME ||
                       menu_action == MENU_ACTION_WAKE ||
                       menu_action == MENU_ACTION_RESET_DONGLE ||
                       menu_action == MENU_ACTION_RESTART_HOST) {

                        const NetInfo *info =
                            net_info();

                        if (info->state ==
                                NET_CONNECTED &&
                            info->may_control) {

                            if (menu_action == MENU_ACTION_REMOTE_HOME)
                                net_send_home();
                            else if (menu_action == MENU_ACTION_WAKE)
                                net_send_wake();
                            else if (menu_action == MENU_ACTION_RESET_DONGLE)
                                net_send_reset_dongle();
                            else
                                net_send_restart();

                            snprintf(
                                note,
                                sizeof(note),
                                "%s sent",
                                menu_action == MENU_ACTION_REMOTE_HOME ? "remote HOME" :
                                menu_action == MENU_ACTION_WAKE ? "wake request" :
                                menu_action == MENU_ACTION_RESET_DONGLE ? "adapter reset" :
                                "host restart");

                        } else {
                            snprintf(
                                note,
                                sizeof(note),
                                "remote commands require CONTROL");
                        }

            } else if (menu_action == MENU_ACTION_CONNECT) {

                        char host[32];
                        char save_why[96];

                        settings_host_string(
                            &settings,
                            host,
                            sizeof(host));

                        /*
                         * net_login() uses its own short HTTP socket.
                         * A failed password therefore leaves the current
                         * media stream untouched and keeps the menu open.
                         */
                        if (authenticate_if_needed(
                                &settings,
                                password,
                                note,
                                sizeof(note))) {

                            if (settings_save(
                                    &settings,
                                    save_why,
                                    sizeof(save_why)) != 0) {

                                snprintf(
                                    note,
                                    sizeof(note),
                                    "not saved: %s",
                                    save_why);
                            }

                            net_disconnect();

                            net_connect(
                                host,
                                settings.port,
                                settings.token[0]
                                    ? settings.token
                                    : NULL);

                            have_frame = 0;
                            menu_force_open(&menu, 0);

                            video_synced = 0;
                            keyframe_requested = 0;

                            worker_decoded_at =
                                video_worker_decoded_total();

                            WHBLogPrintf(
                                "capture2cloud: reconnecting to %s:%u auth=%s",
                                host,
                                settings.port,
                                settings.token[0]
                                    ? "token"
                                    : "viewer");
                        }

            } else if (menu_action == MENU_ACTION_EXIT_HELP) {

                        snprintf(
                            note,
                            sizeof(note),
                            "Press HOME, then choose Quitter");
            }
        }

        ui_begin();

        if (state == STATE_STREAMING) {
            /*
             * NV12 comes directly from H264DEC's rotating framebuffers
             * and is sampled directly by the GX2 shader.
             */
            if (new_frame) {
                if (ui_video_update_nv12(frame.luma,
                                         frame.chroma,
                                         frame.stride,
                                         frame.width,
                                         frame.height) != 0) {
                    WHBLogPrintf("capture2cloud: video upload failed");
                    have_frame = 0;
                }
            }

            if (have_frame) {
                ui_video_draw();
            }
        }

        const MenuView menu_view = {
            .settings = &settings,
            .password = password,
            .note = note,
            .decoder_why = decoder_why,
            .decoder_ok = decoder_ok,
            .streaming = state == STATE_STREAMING,
            .perf = &perf
        };

        menu_draw(&menu, &menu_view);

        ui_present();

        if (state == STATE_STREAMING) {
            loop_count++;

            if (new_frame && have_frame) {
                display_count++;
            }

            const uint32_t now =
                SDL_GetTicks();

            const uint32_t elapsed =
                now - fps_at;

            if (elapsed >= 1000) {
                const uint64_t rx_now =
                    net_info()->rx_bytes;

                const uint64_t rx_delta =
                    rx_now >= rx_bytes_at
                        ? rx_now - rx_bytes_at
                        : 0;

                perf.rx_fps =
                    (unsigned)(
                        ((uint64_t)rx_count * 1000ull) /
                        elapsed);

                VideoWorkerStats vw;
                video_worker_stats(&vw);

                const unsigned decoded_delta =
                    vw.decoded -
                    worker_decoded_at;

                perf.decode_fps =
                    (unsigned)(
                        ((uint64_t)decoded_delta *
                         1000ull) /
                        elapsed);

                perf.video_queue =
                    vw.queue_depth;

                perf.video_dropped =
                    vw.dropped;

                worker_decoded_at =
                    vw.decoded;

                perf.display_fps =
                    (unsigned)(
                        ((uint64_t)display_count * 1000ull) /
                        elapsed);

                perf.loop_fps =
                    (unsigned)(
                        ((uint64_t)loop_count * 1000ull) /
                        elapsed);

                /*
                 * bytes * 8 / milliseconds numerically gives kbit/s.
                 */
                perf.net_kbps =
                    (unsigned)(
                        (rx_delta * 8ull) /
                        elapsed);

                rx_count = 0;
                display_count = 0;
                loop_count = 0;

                fps_at = now;
                rx_bytes_at = rx_now;
            }
        }
    }

    /*
     * Keep ProcUI alive while every resource owned by this application
     * is released. The AX safe-exit probe has almost nothing to tear
     * down; this client also owns TCP, H264DEC and SDL/GX2.
     *
     * Every boundary is logged so if the menu transition ever sticks
     * again we know the exact subsystem that did not return.
     */
    WHBLogPrintf("shutdown: final network cleanup");
    net_disconnect();
    net_exit();

    /*
     * Normally these are already gone because RELEASE_FOREGROUND
     * happened before EXITING. Keep the guards for other exit paths.
     */
    if (audio_alive) {
        WHBLogPrintf("shutdown: final audio cleanup");
        audio_exit();
        audio_alive = 0;
    }

    if (video_worker_alive) {
        WHBLogPrintf("shutdown: final video worker cleanup");
        video_worker_stop();
        video_worker_alive = 0;
    }

    if (video_alive) {
        WHBLogPrintf("shutdown: final H264DEC cleanup");
        video_exit();
        video_alive = 0;
    }

    if (input_alive) {
        WHBLogPrintf("shutdown: final input cleanup");
        input_exit();
        input_alive = 0;
    }

    if (ui_alive) {
        WHBLogPrintf("shutdown: final SDL/GX2 cleanup");
        ui_shutdown();
        ui_alive = 0;
    }

    WHBLogPrintf("shutdown: all application resources released");

    WHBLogUdpDeinit();
    proc_shutdown();

    return 0;
}
