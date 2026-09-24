#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../app_config.h"

/* output_pcble.c references the config reader from init(). The mapping
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

#include "../../output_pcble.c"
#include "test_util.h"

static void test_neutral(void)
{
    t_begin("pcble neutral mapping");

    int8_t state[CONTROLLER_STATE_COUNT] = {0};
    uint8_t buttons[3];
    uint16_t sticks[4];

    build_nintendo_state(state, 0, buttons, sticks);

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

    build_nintendo_state(state, 0, buttons, sticks);

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

    build_nintendo_state(state, 0, buttons, sticks);

    t_eq_int("left X max", sticks[0], 3625);
    t_eq_int("left Y up", sticks[1], 3499);
    t_eq_int("right X min", sticks[2], 548);
    t_eq_int("right Y down", sticks[3], 482);
}

static void test_home_override(void)
{
    t_begin("pcble HOME pulse");

    int8_t state[CONTROLLER_STATE_COUNT] = {0};
    uint8_t buttons[3];
    uint16_t sticks[4];

    build_nintendo_state(state, 1, buttons, sticks);

    t_ok("HOME override is set", (buttons[1] & 0x10) != 0);
}

int main(void)
{
    test_neutral();
    test_buttons();
    test_axes();
    test_home_override();

    return t_report();
}
