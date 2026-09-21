#include "input.h"
#include "net.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <whb/log.h>

/*
 * The Wii U SDL joystick backend exposes the GamePad raw buttons in
 * this exact order:
 *
 *   0 A       1 B       2 X       3 Y
 *   4 L3      5 R3
 *   6 L       7 R
 *   8 ZL      9 ZR
 *  10 PLUS   11 MINUS
 *  12 LEFT   13 UP     14 RIGHT  15 DOWN
 *
 * Using the raw joystick API deliberately avoids SDL's optional
 * Nintendo-vs-Xbox GameController label remapping.
 */
enum {
    WIIU_BTN_A = 0,
    WIIU_BTN_B,
    WIIU_BTN_X,
    WIIU_BTN_Y,
    WIIU_BTN_L3,
    WIIU_BTN_R3,
    WIIU_BTN_L,
    WIIU_BTN_R,
    WIIU_BTN_ZL,
    WIIU_BTN_ZR,
    WIIU_BTN_PLUS,
    WIIU_BTN_MINUS,
    WIIU_BTN_LEFT,
    WIIU_BTN_UP,
    WIIU_BTN_RIGHT,
    WIIU_BTN_DOWN
};

#define INPUT_KEEPALIVE_MS 100

/*
 * SDL's Wii U backend scales VPAD's -1..+1 stick range by 0x7ff0.
 */
#define STICK_RAW_MAX 32752

static SDL_Joystick *g_pad;

static InputConfig g_config = {
    .deadzone = { 3, 3 },
    .range = { 100, 100 },
    .invert_y = 0,
    .face_by_position = 1
};

static PadState21 g_state;
static PadState21 g_last_sent;

static uint32_t g_last_send_ms;


/*
 * Scale one raw SDL axis to the protocol's -100..+100 range.
 *
 * The deadzone is removed rather than merely zeroed, so once outside it
 * the remaining physical travel still spans the complete output range.
 */
static int8_t axis_to_wire(Sint16 raw, int stick)
{
    int value = (int)raw;
    int sign = 1;

    if (value < 0) {
        sign = -1;
        value = -value;
    }

    int dead =
        STICK_RAW_MAX * g_config.deadzone[stick] / 100;

    int range =
        STICK_RAW_MAX * g_config.range[stick] / 100;

    if (range <= dead) {
        range = dead + 1;
    }

    if (value <= dead) {
        return 0;
    }

    if (value > range) {
        value = range;
    }

    int scaled =
        (value - dead) * 100 /
        (range - dead);

    if (scaled > 100) {
        scaled = 100;
    }

    return (int8_t)(scaled * sign);
}


static int pressed(int index)
{
    if (!g_pad) {
        return 0;
    }

    return
        SDL_JoystickGetButton(
            g_pad,
            index)
        ? 100
        : 0;
}


static void sample_pad(PadState21 out)
{
    memset(
        out,
        0,
        sizeof(PadState21));

    if (!g_pad ||
        !SDL_JoystickGetAttached(g_pad)) {
        return;
    }

    /*
     * FACE BUTTONS BY POSITION, not by Nintendo letter.
     *
     * Wii U:                 Xbox-style wire:
     *
     *        X                       Y
     *     Y     A                 X     B
     *        B                       A
     *
     * Therefore:
     *
     * physical B (bottom) -> PAD_A
     * physical A (right)  -> PAD_B
     * physical Y (left)   -> PAD_X
     * physical X (top)    -> PAD_Y
     */
    if (g_config.face_by_position) {
        out[PAD_A] = pressed(WIIU_BTN_B);
        out[PAD_B] = pressed(WIIU_BTN_A);
        out[PAD_X] = pressed(WIIU_BTN_Y);
        out[PAD_Y] = pressed(WIIU_BTN_X);
    } else {
        out[PAD_A] = pressed(WIIU_BTN_A);
        out[PAD_B] = pressed(WIIU_BTN_B);
        out[PAD_X] = pressed(WIIU_BTN_X);
        out[PAD_Y] = pressed(WIIU_BTN_Y);
    }


    out[PAD_LB] =
        pressed(WIIU_BTN_L);

    out[PAD_RB] =
        pressed(WIIU_BTN_R);

    out[PAD_LT] =
        pressed(WIIU_BTN_ZL);

    out[PAD_RT] =
        pressed(WIIU_BTN_ZR);

    out[PAD_LS] =
        pressed(WIIU_BTN_L3);

    out[PAD_RS] =
        pressed(WIIU_BTN_R3);


    out[PAD_START] =
        pressed(WIIU_BTN_PLUS);

    out[PAD_BACK] =
        pressed(WIIU_BTN_MINUS);


    out[PAD_LEFT] =
        pressed(WIIU_BTN_LEFT);

    out[PAD_UP] =
        pressed(WIIU_BTN_UP);

    out[PAD_RIGHT] =
        pressed(WIIU_BTN_RIGHT);

    out[PAD_DOWN] =
        pressed(WIIU_BTN_DOWN);


    /*
     * Raw SDL Wii U axes:
     *
     *   0 = left X
     *   1 = left Y
     *   2 = right X
     *   3 = right Y
     *
     * SDL's Wii U backend already turns VPAD's "up positive" into the
     * usual SDL convention "up negative".
     *
     * The existing Switch native client currently sends up negative on
     * the wire too, so KEEP the Y sign for this first hardware test.
     * The menu setting for invert-Y comes after we verify it physically.
     */
    out[PAD_LX] =
        axis_to_wire(
            SDL_JoystickGetAxis(
                g_pad,
                0),
            0);

    out[PAD_LY] =
        (g_config.invert_y ? -1 : 1) * axis_to_wire(
            SDL_JoystickGetAxis(
                g_pad,
                1),
            0);

    out[PAD_RX] =
        axis_to_wire(
            SDL_JoystickGetAxis(
                g_pad,
                2),
            1);

    out[PAD_RY] =
        (g_config.invert_y ? -1 : 1) * axis_to_wire(
            SDL_JoystickGetAxis(
                g_pad,
                3),
            1);

    /*
     * PAD_GUIDE remains zero.
     *
     * HOME belongs to ProcUI on the Wii U and opens the local system
     * overlay. Remote HOME can later be exposed as a menu action/combo.
     */
}


int input_init(char *why,
               size_t why_size)
{
    input_exit();

    if (why && why_size) {
        why[0] = '\0';
    }

    /*
     * Make sure SDL has processed its initial Wii U controller
     * detection before asking for the list.
     */
    SDL_PumpEvents();

    const int count =
        SDL_NumJoysticks();

    int gamepad_index = -1;

    for (int i = 0;
         i < count;
         ++i) {

        const char *name =
            SDL_JoystickNameForIndex(i);

        if (!name) {
            continue;
        }

        WHBLogPrintf(
            "input: joystick %d: %s",
            i,
            name);

        /*
         * Different SDL/WUT revisions spell this either WiiU or Wii U.
         */
        if (strstr(name, "WiiU Gamepad") ||
            strstr(name, "Wii U Gamepad")) {

            gamepad_index = i;
            break;
        }
    }

    if (gamepad_index < 0) {
        if (why && why_size) {
            snprintf(
                why,
                why_size,
                "Wii U GamePad not found");
        }

        return -1;
    }

    g_pad =
        SDL_JoystickOpen(
            gamepad_index);

    if (!g_pad) {
        if (why && why_size) {
            snprintf(
                why,
                why_size,
                "GamePad open: %s",
                SDL_GetError());
        }

        return -1;
    }

    memset(
        g_state,
        0,
        sizeof(g_state));

    memset(
        g_last_sent,
        0,
        sizeof(g_last_sent));

    g_last_send_ms = 0;

    WHBLogPrintf(
        "input: Wii U GamePad ready, axes=%d buttons=%d",
        SDL_JoystickNumAxes(g_pad),
        SDL_JoystickNumButtons(g_pad));

    return 0;
}


void input_exit(void)
{
    if (g_pad) {
        SDL_JoystickClose(g_pad);
        g_pad = NULL;
    }

    memset(
        g_state,
        0,
        sizeof(g_state));

    memset(
        g_last_sent,
        0,
        sizeof(g_last_sent));

    g_last_send_ms = 0;
}


int input_available(void)
{
    return
        g_pad &&
        SDL_JoystickGetAttached(g_pad);
}


void input_snapshot(PadState21 out)
{
    if (!out) {
        return;
    }

    memcpy(
        out,
        g_state,
        sizeof(PadState21));
}

void input_set_config(const InputConfig *config)
{
    if (!config) {
        return;
    }

    g_config = *config;

    for (int i = 0; i < 2; ++i) {
        if (g_config.deadzone[i] > 40)
            g_config.deadzone[i] = 40;
        if (g_config.range[i] < 45)
            g_config.range[i] = 45;
        if (g_config.range[i] > 100)
            g_config.range[i] = 100;
        if (g_config.range[i] <= g_config.deadzone[i])
            g_config.range[i] = g_config.deadzone[i] + 1;
    }

    g_config.invert_y = !!g_config.invert_y;
    g_config.face_by_position = !!g_config.face_by_position;
}

void input_get_config(InputConfig *config)
{
    if (config) {
        *config = g_config;
    }
}


void input_update(int forward)
{
    PadState21 physical;
    PadState21 wanted;

    sample_pad(physical);

    memcpy(
        g_state,
        physical,
        sizeof(g_state));

    if (forward) {
        memcpy(
            wanted,
            physical,
            sizeof(wanted));
    } else {
        memset(
            wanted,
            0,
            sizeof(wanted));
    }

    /*
     * Don't advance the local "last sent" clock while the connection
     * cannot actually accept input. This guarantees that the current
     * state is sent immediately when a controllable session comes up.
     */
    const NetInfo *info =
        net_info();

    if (info->state != NET_CONNECTED ||
        !info->may_control) {

        memset(
            g_last_sent,
            0,
            sizeof(g_last_sent));

        g_last_send_ms = 0;

        return;
    }

    const uint32_t now =
        SDL_GetTicks();

    const int changed =
        memcmp(
            wanted,
            g_last_sent,
            sizeof(wanted)) != 0;

    if (changed ||
        g_last_send_ms == 0 ||
        now - g_last_send_ms >=
            INPUT_KEEPALIVE_MS) {

        net_send_input(
            wanted);

        memcpy(
            g_last_sent,
            wanted,
            sizeof(g_last_sent));

        g_last_send_ms =
            now;
    }
}
