#include "controller_shaping.h"

#include <math.h>
#include <string.h>

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

void controller_shape_stick(
    float x,
    float y,
    int dead_pct,
    int range_pct,
    int diagonal_pct,
    int8_t *out_x,
    int8_t *out_y)
{
    *out_x = 0;
    *out_y = 0;

    dead_pct = clamp_int(dead_pct, 0, 99);
    range_pct = clamp_int(range_pct, 1, 100);
    diagonal_pct = clamp_int(diagonal_pct, 1, 100);

    const float mag = sqrtf(x * x + y * y);

    if (mag < 0.0001f) {
        return;
    }

    const float ax = fabsf(x);
    const float ay = fabsf(y);
    const float peak = ax > ay ? ax : ay;
    const float least = ax > ay ? ay : ax;

    const float diagonality =
        peak > 0.0001f
            ? least / peak
            : 0.0f;

    const float dead =
        dead_pct / 100.0f;

    if (mag <= dead) {
        return;
    }

    /*
     * A physical stick does not necessarily have the same maximum radius
     * on an axis and in a corner. Interpolate between the two calibration
     * points according to its direction.
     */
    const float sat_axis =
        range_pct / 100.0f;

    const float sat_diag =
        diagonal_pct / 100.0f;

    float sat =
        sat_axis +
        (sat_diag - sat_axis) * diagonality;

    if (sat <= dead) {
        sat = dead + 0.01f;
    }

    float t =
        (mag - dead) /
        (sat - dead);

    if (t > 1.0f) {
        t = 1.0f;
    }

    if (t < 0.0f) {
        t = 0.0f;
    }

    /*
     * Preserve the stick direction while making the largest component
     * reach the requested output scale.
     */
    const float scale =
        peak > 0.0001f
            ? t * 100.0f / peak
            : 0.0f;

    int px =
        (int)(x * scale +
              (x < 0 ? -0.5f : 0.5f));

    int py =
        (int)(y * scale +
              (y < 0 ? -0.5f : 0.5f));

    px = clamp_int(px, -100, 100);
    py = clamp_int(py, -100, 100);

    *out_x = (int8_t)px;
    *out_y = (int8_t)py;
}

static int neutral_stick_shape(
    int deadzone,
    int range,
    int diagonal)
{
    return deadzone == 0 &&
           range == 100 &&
           diagonal == 100;
}

void controller_shape_state(
    const ControllerShaping *shaping,
    const int8_t input[CONTROLLER_STATE_COUNT],
    int8_t output[CONTROLLER_STATE_COUNT])
{
    memcpy(
        output,
        input,
        CONTROLLER_STATE_COUNT);

    if (!shaping) {
        return;
    }

    /*
     * Zero threshold deliberately means pass-through. This keeps output
     * shaping neutral by default and allows a future backend to preserve
     * analogue triggers.
     */
    if (shaping->lt_threshold > 0) {
        output[CONTROLLER_L2] =
            input[CONTROLLER_L2] >
                    shaping->lt_threshold
                ? 100
                : 0;
    }

    if (shaping->rt_threshold > 0) {
        output[CONTROLLER_R2] =
            input[CONTROLLER_R2] >
                    shaping->rt_threshold
                ? 100
                : 0;
    }

    if (!neutral_stick_shape(
            shaping->stick_deadzone[0],
            shaping->stick_range[0],
            shaping->stick_diagonal[0])) {
        controller_shape_stick(
            input[CONTROLLER_LX] / 100.0f,
            input[CONTROLLER_LY] / 100.0f,
            shaping->stick_deadzone[0],
            shaping->stick_range[0],
            shaping->stick_diagonal[0],
            &output[CONTROLLER_LX],
            &output[CONTROLLER_LY]);
    }

    if (!neutral_stick_shape(
            shaping->stick_deadzone[1],
            shaping->stick_range[1],
            shaping->stick_diagonal[1])) {
        controller_shape_stick(
            input[CONTROLLER_RX] / 100.0f,
            input[CONTROLLER_RY] / 100.0f,
            shaping->stick_deadzone[1],
            shaping->stick_range[1],
            shaping->stick_diagonal[1],
            &output[CONTROLLER_RX],
            &output[CONTROLLER_RY]);
    }

    if (shaping->invert_ry) {
        output[CONTROLLER_RY] =
            (int8_t)-output[CONTROLLER_RY];
    }
}
