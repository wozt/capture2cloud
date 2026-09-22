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

static const uint8_t DEFAULT_BUTTON_MAP[INPUT_BUTTON_COUNT] = {
    PAD_B, PAD_A, PAD_Y, PAD_X,
    PAD_LS, PAD_RS, PAD_LB, PAD_RB,
    PAD_LT, PAD_RT, PAD_START, PAD_BACK,
    PAD_LEFT, PAD_UP, PAD_RIGHT, PAD_DOWN
};

static const uint8_t BINDABLE_PAD_SLOTS[INPUT_BUTTON_COUNT] = {
    PAD_A, PAD_B, PAD_X, PAD_Y,
    PAD_LS, PAD_RS, PAD_LB, PAD_RB,
    PAD_LT, PAD_RT, PAD_START, PAD_BACK,
    PAD_LEFT, PAD_UP, PAD_RIGHT, PAD_DOWN
};

static const char *const PHYSICAL_BUTTON_NAMES[INPUT_BUTTON_COUNT] = {
    "A", "B", "X", "Y", "L3", "R3", "L", "R",
    "ZL", "ZR", "+", "-", "D-Left", "D-Up", "D-Right", "D-Down"
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
    .face_by_position = 1,
    .button_map = {
        PAD_B, PAD_A, PAD_Y, PAD_X,
        PAD_LS, PAD_RS, PAD_LB, PAD_RB,
        PAD_LT, PAD_RT, PAD_START, PAD_BACK,
        PAD_LEFT, PAD_UP, PAD_RIGHT, PAD_DOWN
    }
};

static PadState21 g_state;
static PadState21 g_last_sent;

static uint32_t g_last_send_ms;
static int g_remote_home_chord_held;
static uint32_t g_physical_mask;
static uint32_t g_physical_pressed;


static int bindable_slot(int slot)
{
    for (int i = 0; i < INPUT_BUTTON_COUNT; ++i) {
        if (BINDABLE_PAD_SLOTS[i] == slot) {
            return 1;
        }
    }

    return 0;
}


void input_config_reset_bindings(InputConfig *config)
{
    if (!config) {
        return;
    }

    memcpy(
        config->button_map,
        DEFAULT_BUTTON_MAP,
        sizeof(config->button_map));

    config->face_by_position = 1;
}


void input_config_sanitize(InputConfig *config)
{
    if (!config) {
        return;
    }

    for (int i = 0; i < 2; ++i) {
        if (config->deadzone[i] > 40)
            config->deadzone[i] = 40;
        if (config->range[i] < 45)
            config->range[i] = 45;
        if (config->range[i] > 100)
            config->range[i] = 100;
        if (config->range[i] <= config->deadzone[i])
            config->range[i] = config->deadzone[i] + 1;
    }

    config->invert_y = !!config->invert_y;
    config->face_by_position = !!config->face_by_position;

    uint32_t seen = 0;

    for (int i = 0; i < INPUT_BUTTON_COUNT; ++i) {
        const int slot = config->button_map[i];

        if (!bindable_slot(slot) ||
            (seen & (1u << slot)) != 0) {

            input_config_reset_bindings(config);
            return;
        }

        seen |= 1u << slot;
    }
}


int input_config_physical_for_slot(const InputConfig *config,
                                   int pad_slot)
{
    if (!config) {
        return -1;
    }

    for (int i = 0; i < INPUT_BUTTON_COUNT; ++i) {
        if (config->button_map[i] == pad_slot) {
            return i;
        }
    }

    return -1;
}


void input_config_bind(InputConfig *config,
                       int physical_button,
                       int pad_slot)
{
    if (!config ||
        physical_button < 0 ||
        physical_button >= INPUT_BUTTON_COUNT ||
        !bindable_slot(pad_slot)) {

        return;
    }

    input_config_sanitize(config);

    const int displaced =
        input_config_physical_for_slot(
            config,
            pad_slot);

    const uint8_t old_slot =
        config->button_map[physical_button];

    config->button_map[physical_button] =
        (uint8_t)pad_slot;

    if (displaced >= 0 &&
        displaced != physical_button) {

        config->button_map[displaced] =
            old_slot;
    }

    config->face_by_position = 0;
}


const char *input_physical_button_name(int physical_button)
{
    if (physical_button < 0 ||
        physical_button >= INPUT_BUTTON_COUNT) {

        return "?";
    }

    return PHYSICAL_BUTTON_NAMES[physical_button];
}


const char *input_pad_slot_name(int pad_slot)
{
    switch (pad_slot) {
    case PAD_A: return "A";
    case PAD_B: return "B";
    case PAD_X: return "X";
    case PAD_Y: return "Y";
    case PAD_LS: return "L3";
    case PAD_RS: return "R3";
    case PAD_LB: return "L";
    case PAD_RB: return "R";
    case PAD_LT: return "ZL";
    case PAD_RT: return "ZR";
    case PAD_START: return "+";
    case PAD_BACK: return "-";
    case PAD_LEFT: return "D-Left";
    case PAD_UP: return "D-Up";
    case PAD_RIGHT: return "D-Right";
    case PAD_DOWN: return "D-Down";
    default: return "?";
    }
}


int input_take_physical_button(void)
{
    for (int i = 0; i < INPUT_BUTTON_COUNT; ++i) {
        const uint32_t bit = 1u << i;

        if ((g_physical_pressed & bit) != 0) {
            g_physical_pressed &= ~bit;
            return i;
        }
    }

    return -1;
}


void input_clear_physical_buttons(void)
{
    g_physical_pressed = 0;
}


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


static uint32_t sample_pad(PadState21 out)
{
    memset(
        out,
        0,
        sizeof(PadState21));

    if (!g_pad ||
        !SDL_JoystickGetAttached(g_pad)) {
        return 0;
    }

    uint32_t physical_mask = 0;

    for (int physical = 0;
         physical < INPUT_BUTTON_COUNT;
         ++physical) {

        if (!pressed(physical)) {
            continue;
        }

        physical_mask |=
            1u << physical;

        const int slot =
            g_config.button_map[physical];

        if (slot >= 0 && slot < PAD_SLOT_COUNT) {
            out[slot] = 100;
        }
    }


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

    return physical_mask;
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
    g_remote_home_chord_held = 0;
    g_physical_mask = 0;
    g_physical_pressed = 0;

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
    g_remote_home_chord_held = 0;
    g_physical_mask = 0;
    g_physical_pressed = 0;
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
    input_config_sanitize(&g_config);
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

    const uint32_t physical_mask =
        sample_pad(physical);

    g_physical_pressed |=
        physical_mask &
        ~g_physical_mask;

    g_physical_mask =
        physical_mask;

    memcpy(
        g_state,
        physical,
        sizeof(g_state));

    /* HOME itself belongs to the local Wii U and makes ProcUI suspend
     * this application. L3+R3 reaches the remote console while the
     * capture stays visible. Latch it so one hold sends one HOME, and do
     * not leak the two stick clicks into the remote game as well. */
    const int remote_home_chord =
        (physical_mask &
         (1u << WIIU_BTN_L3)) != 0 &&
        (physical_mask &
         (1u << WIIU_BTN_R3)) != 0;

    if (forward &&
        remote_home_chord &&
        !g_remote_home_chord_held) {

        net_send_home();
        WHBLogPrintf("input: L3+R3 -> remote HOME");
    }

    g_remote_home_chord_held =
        remote_home_chord;

    if (remote_home_chord) {
        physical[g_config.button_map[WIIU_BTN_L3]] = 0;
        physical[g_config.button_map[WIIU_BTN_R3]] = 0;
    }

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
