#ifndef CONTROLLER_SHAPING_H
#define CONTROLLER_SHAPING_H

#include "controller_state.h"

#include <stdint.h>

typedef struct {
    int invert_ry;

    /*
     * Trigger threshold.
     *
     * For an output shaping profile, zero means "preserve the incoming
     * analogue value". A positive value turns the trigger into 0/100.
     *
     * Local SDL input performs its threshold before ControllerState is
     * built, so its normal defaults remain 30%.
     */
    int lt_threshold;
    int rt_threshold;

    int stick_deadzone[2];
    int stick_range[2];
    int stick_diagonal[2];
} ControllerShaping;

#define CONTROLLER_SHAPING_OUTPUT_DEFAULTS \
    {                                      \
        .invert_ry = 0,                    \
        .lt_threshold = 0,                 \
        .rt_threshold = 0,                 \
        .stick_deadzone = {0, 0},          \
        .stick_range = {100, 100},         \
        .stick_diagonal = {100, 100},      \
    }

/*
 * Maps one physical/normalised stick through radial dead-zone and
 * saturation calibration.
 *
 * range_pct is the saturation point on the cardinal axes.
 * diagonal_pct is the saturation point at 45 degrees. Values between
 * those directions interpolate smoothly.
 */
void controller_shape_stick(
    float x,
    float y,
    int dead_pct,
    int range_pct,
    int diagonal_pct,
    int8_t *out_x,
    int8_t *out_y);

/*
 * Applies shaping to a complete already-normalised ControllerState.
 *
 * This is used at the OUTPUT side, after all sources have been merged.
 * The neutral output defaults are deliberately an exact pass-through.
 */
void controller_shape_state(
    const ControllerShaping *shaping,
    const int8_t input[CONTROLLER_STATE_COUNT],
    int8_t output[CONTROLLER_STATE_COUNT]);

#endif
