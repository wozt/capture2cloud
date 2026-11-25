#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../server/app_config.h"

/* server/output_pcble.c references the config reader from init(). The mapping
 * test never calls init, so a tiny stub keeps this test hardware-free. */
const char *config_get_str(
    const char *key,
    char *buffer,
    size_t buffer_size,
    const char *fallback)
{
    (void)key;

    if (buffer && buffer_size) {
        snprintf(buffer, buffer_size, "%s", fallback ? fallback : "");
        return buffer;
    }

    return fallback;
}

long config_get_int(const char *key, long fallback, long min_value, long max_value)
{
    (void)key;
    (void)min_value;
    (void)max_value;
    return fallback;
}

#include "../../server/output_pcble.c"
#include "test_util.h"

static void test_neutral(void)
{
    t_begin("pcble neutral mapping");

    int8_t state[CONTROLLER_STATE_COUNT] = {0};
    uint8_t buttons[3];
    uint16_t sticks[4];

    build_nintendo_state(state, 0, 0, buttons, sticks);

    t_eq_int("buttons 0 neutral", buttons[0], 0);
    t_eq_int("buttons 1 neutral", buttons[1], 0);
    t_eq_int("buttons 2 neutral", buttons[2], 0);

    t_eq_int("left X center", sticks[0], 2159);
    t_eq_int("left Y center", sticks[1], 1916);
    t_eq_int("right X center", sticks[2], 2070);
    t_eq_int("right Y center", sticks[3], 2013);
}

static void test_buttons(void)
{
    t_begin("pcble Nintendo button mapping");

    int8_t state[CONTROLLER_STATE_COUNT] = {0};
    uint8_t buttons[3];
    uint16_t sticks[4];

    state[CONTROLLER_FACE_EAST] = 100;   /* Nintendo A */
    state[CONTROLLER_FACE_SOUTH] = 100;  /* Nintendo B */
    state[CONTROLLER_FACE_NORTH] = 100;  /* Nintendo X */
    state[CONTROLLER_FACE_WEST] = 100;   /* Nintendo Y */
    state[CONTROLLER_R1] = 100;
    state[CONTROLLER_R2] = 100;

    state[CONTROLLER_SELECT] = 100;
    state[CONTROLLER_START] = 100;
    state[CONTROLLER_L3] = 100;
    state[CONTROLLER_R3] = 100;
    state[CONTROLLER_HOME] = 100;

    state[CONTROLLER_UP] = 100;
    state[CONTROLLER_DOWN] = 100;
    state[CONTROLLER_LEFT] = 100;
    state[CONTROLLER_RIGHT] = 100;
    state[CONTROLLER_L1] = 100;
    state[CONTROLLER_L2] = 100;

    build_nintendo_state(state, 0, 0, buttons, sticks);

    t_eq_int("right-side byte", buttons[0], 0xcf);
    t_eq_int("middle byte", buttons[1], 0x1f);
    t_eq_int("left-side byte", buttons[2], 0xcf);
}

static void test_axes(void)
{
    t_begin("pcble stick mapping");

    int8_t state[CONTROLLER_STATE_COUNT] = {0};
    uint8_t buttons[3];
    uint16_t sticks[4];

    state[CONTROLLER_LX] = 100;
    state[CONTROLLER_LY] = -100; /* up */
    state[CONTROLLER_RX] = -100;
    state[CONTROLLER_RY] = 100;  /* down */

    build_nintendo_state(state, 0, 0, buttons, sticks);

    t_eq_int("left X max", sticks[0], 3625);
    t_eq_int("left Y up", sticks[1], 3499);
    t_eq_int("right X min", sticks[2], 548);
    t_eq_int("right Y down", sticks[3], 482);
}

static void test_reconnect_request(void)
{
    t_begin("pcble generic recovery request");

    output_pcble_clear_reconnect_request();

    t_eq_int(
        "no reconnect initially pending",
        output_pcble_reconnect_pending(),
        0);

    output_pcble_reset();

    t_eq_int(
        "backend reset queues reconnect",
        output_pcble_reconnect_pending(),
        1);

    output_pcble_suspend_for_wake();

    t_eq_int(
        "wake suspend cancels reconnect",
        output_pcble_reconnect_pending(),
        0);

    t_eq_int(
        "wake suspend blocks helper auto-recovery",
        SDL_AtomicGet(&g_wake_suspended),
        1);

    output_pcble_reset();

    t_eq_int(
        "post-wake reset resumes pcble",
        SDL_AtomicGet(&g_wake_suspended),
        0);

    t_eq_int(
        "post-wake reset queues reconnect",
        output_pcble_reconnect_pending(),
        1);

    output_pcble_clear_reconnect_request();

    t_eq_int(
        "reconnect request can still be cleared",
        output_pcble_reconnect_pending(),
        0);
}

static void test_home_override(void)
{
    t_begin("pcble HOME pulse");

    int8_t state[CONTROLLER_STATE_COUNT] = {0};
    uint8_t buttons[3];
    uint16_t sticks[4];

    build_nintendo_state(state, 1, 0, buttons, sticks);

    t_ok("HOME override is set", (buttons[1] & 0x10) != 0);
}

static void test_profiles(void)
{
    t_begin("pcble controller profiles");

    int pair = -1;
    int mixed = -1;

    t_ok("pro is valid", parse_profile("pro", &pair, &mixed));
    t_eq_int("pro is not a pair", pair, 0);
    t_ok("joycon-pair is valid", parse_profile("joycon-pair", &pair, &mixed));
    t_eq_int("joycon-pair is a pair", pair, 1);
    t_eq_int("joycon-pair is not mixed", mixed, 0);
    t_ok("joycon-mixed is valid", parse_profile("joycon-mixed", &pair, &mixed));
    t_eq_int("joycon-mixed is a pair", pair, 1);
    t_eq_int("joycon-mixed is mixed", mixed, 1);
    t_ok("unknown profile is rejected", !parse_profile("joycon2", &pair, &mixed));
}

static void test_mouse(void)
{
    t_begin("pcble Joy-Con 2 mouse");

    g_lock = SDL_CreateMutex();

    output_pcble_set_controller("pro");
    g_ipc_up = 1;
    t_eq_int("disabled for Pro", output_pcble_mouse_enabled(), 0);
    output_pcble_mouse(5, 5, 1, 1, 1);
    t_eq_int("ignored for Pro", g_mouse_dx, 0);

    output_pcble_set_controller("joycon-mixed");
    g_ipc_up = 1;
    t_eq_int("enabled for Joy-Con 1 + Joy-Con 2", output_pcble_mouse_enabled(), 1);
    output_pcble_mouse(5, -3, 1, 1, 1);
    output_pcble_mouse(5, -3, 0, 3, 1);
    t_eq_int("dx accumulates", g_mouse_dx, 10);
    t_eq_int("dy accumulates", g_mouse_dy, -6);
    t_eq_int("wheel accumulates", g_mouse_wheel, 1);
    t_eq_int("buttons follow the last event", (int)g_mouse_buttons, 3);
    output_pcble_mouse(40000, 0, 0, 0, 1);
    t_eq_int("dx saturates", g_mouse_dx, 32767);

    output_pcble_set_controller("joycon-pair");
    t_eq_int("profile change clears motion", g_mouse_dx, 0);
    t_eq_int("disabled for Joy-Con 1 pair", output_pcble_mouse_enabled(), 0);

    SDL_DestroyMutex(g_lock);
    g_lock = NULL;
}

static void test_new_ble_profiles(void)
{
    t_begin("pcble complete Switch 2 controller catalog");
    struct { const char *profile; int pair, ble, side; } cases[] = {
        {"pro", 0, 0, 0},
        {"joycon-pair", 1, 0, 0},
        {"joycon-mixed", 1, 1, 2},
        {"joycon2-left", 0, 1, 1},
        {"joycon2-right", 0, 1, 2},
        {"joycon2-pair", 1, 1, 2},
        {"joycon-mixed-reverse", 1, 1, 1}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int pair = -1, ble = -1;
        t_ok(cases[i].profile, parse_profile(cases[i].profile, &pair, &ble));
        t_eq_int("paired sockets", pair, cases[i].pair);
        t_eq_int("BLE transport", ble, cases[i].ble);
        t_eq_int("mouse sensor side", pcble_mouse_side(cases[i].profile), cases[i].side);
    }
    t_ok("invalid profile rejected", !parse_profile("joycon2", &(int){0}, &(int){0}));
}

int main(void)
{
    test_neutral();
    test_buttons();
    test_axes();
    test_reconnect_request();
    test_home_override();
    test_profiles();
    test_mouse();
    test_new_ble_profiles();

    return t_report();
}
