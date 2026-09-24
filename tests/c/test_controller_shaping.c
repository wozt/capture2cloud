#include <stdint.h>
#include <string.h>

#include "../../controller_shaping.c"

#include "test_util.h"

static void test_stick_calibration(void)
{
    t_begin("stick shaping");

    int8_t x = 0;
    int8_t y = 0;

    controller_shape_stick(
        0.10f,
        0.0f,
        20,
        100,
        100,
        &x,
        &y);

    t_eq_int(
        "deadzone removes small movement",
        x,
        0);

    controller_shape_stick(
        0.80f,
        0.0f,
        0,
        80,
        100,
        &x,
        &y);

    t_eq_int(
        "80 percent cardinal range becomes full output",
        x,
        100);

    controller_shape_stick(
        0.60f,
        0.60f,
        0,
        100,
        80,
        &x,
        &y);

    t_eq_int(
        "diagonal calibration can saturate X independently of axis range",
        x,
        100);

    t_eq_int(
        "diagonal calibration can saturate Y independently of axis range",
        y,
        100);
}

static void test_output_identity(void)
{
    t_begin("neutral output shaping");

    ControllerShaping shaping =
        CONTROLLER_SHAPING_OUTPUT_DEFAULTS;

    int8_t input[CONTROLLER_STATE_COUNT] = {0};
    int8_t output[CONTROLLER_STATE_COUNT] = {0};

    input[CONTROLLER_LX] = 65;
    input[CONTROLLER_LY] = -32;
    input[CONTROLLER_RX] = -44;
    input[CONTROLLER_RY] = 71;
    input[CONTROLLER_L2] = 27;
    input[CONTROLLER_R2] = 83;
    input[CONTROLLER_FACE_EAST] = 100;

    controller_shape_state(
        &shaping,
        input,
        output);

    t_ok(
        "default output shaping is byte-for-byte transparent",
        memcmp(input, output, sizeof(input)) == 0);
}

static void test_live_output_values(void)
{
    t_begin("output calibration values");

    ControllerShaping shaping =
        CONTROLLER_SHAPING_OUTPUT_DEFAULTS;

    int8_t input[CONTROLLER_STATE_COUNT] = {0};
    int8_t output[CONTROLLER_STATE_COUNT] = {0};

    input[CONTROLLER_LX] = 80;
    shaping.stick_range[0] = 80;

    controller_shape_state(
        &shaping,
        input,
        output);

    t_eq_int(
        "output range boosts an 80 percent held stick to 100",
        output[CONTROLLER_LX],
        100);

    memset(input, 0, sizeof(input));
    shaping =
        (ControllerShaping)CONTROLLER_SHAPING_OUTPUT_DEFAULTS;

    input[CONTROLLER_LX] = 10;
    shaping.stick_deadzone[0] = 20;

    controller_shape_state(
        &shaping,
        input,
        output);

    t_eq_int(
        "output deadzone removes small final movement",
        output[CONTROLLER_LX],
        0);

    memset(input, 0, sizeof(input));
    shaping =
        (ControllerShaping)CONTROLLER_SHAPING_OUTPUT_DEFAULTS;

    input[CONTROLLER_RY] = 60;
    shaping.invert_ry = 1;

    controller_shape_state(
        &shaping,
        input,
        output);

    t_eq_int(
        "output right Y inversion is applied",
        output[CONTROLLER_RY],
        -60);

    memset(input, 0, sizeof(input));
    shaping =
        (ControllerShaping)CONTROLLER_SHAPING_OUTPUT_DEFAULTS;

    input[CONTROLLER_L2] = 20;
    input[CONTROLLER_R2] = 50;

    shaping.lt_threshold = 30;
    shaping.rt_threshold = 30;

    controller_shape_state(
        &shaping,
        input,
        output);

    t_eq_int(
        "LT below output threshold is released",
        output[CONTROLLER_L2],
        0);

    t_eq_int(
        "RT above output threshold is fully pressed",
        output[CONTROLLER_R2],
        100);
}

int main(void)
{
    test_stick_calibration();
    test_output_identity();
    test_live_output_values();

    return t_report();
}
