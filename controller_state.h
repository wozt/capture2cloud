#ifndef CONTROLLER_STATE_H
#define CONTROLLER_STATE_H

#include <stdint.h>

/*
 * Backend-independent controller state.
 *
 * The wire protocol still carries exactly the same 21 signed bytes as
 * before.  Generic positional names are provided so future output
 * backends do not need to know that the original representation came
 * from the Titan/GCAPI Xbox 360 layout.
 *
 * Values are -100..100 for axes and 0..100 for buttons/triggers.
 */
enum {
    CONTROLLER_HOME = 0,
    CONTROLLER_SELECT,
    CONTROLLER_START,
    CONTROLLER_R1,
    CONTROLLER_R2,
    CONTROLLER_R3,
    CONTROLLER_L1,
    CONTROLLER_L2,
    CONTROLLER_L3,
    CONTROLLER_RX,
    CONTROLLER_RY,
    CONTROLLER_LX,
    CONTROLLER_LY,
    CONTROLLER_UP,
    CONTROLLER_DOWN,
    CONTROLLER_LEFT,
    CONTROLLER_RIGHT,
    CONTROLLER_FACE_NORTH,
    CONTROLLER_FACE_EAST,
    CONTROLLER_FACE_SOUTH,
    CONTROLLER_FACE_WEST,
    CONTROLLER_STATE_COUNT
};

typedef int8_t ControllerState[CONTROLLER_STATE_COUNT];

/*
 * Compatibility names.
 *
 * Keep the existing clients and protocol completely unchanged while
 * the host gains a backend-independent representation.
 */
#define GAMEPAD_XB360_GUIDE CONTROLLER_HOME
#define GAMEPAD_XB360_BACK  CONTROLLER_SELECT
#define GAMEPAD_XB360_START CONTROLLER_START
#define GAMEPAD_XB360_RB    CONTROLLER_R1
#define GAMEPAD_XB360_RT    CONTROLLER_R2
#define GAMEPAD_XB360_RS    CONTROLLER_R3
#define GAMEPAD_XB360_LB    CONTROLLER_L1
#define GAMEPAD_XB360_LT    CONTROLLER_L2
#define GAMEPAD_XB360_LS    CONTROLLER_L3
#define GAMEPAD_XB360_RX    CONTROLLER_RX
#define GAMEPAD_XB360_RY    CONTROLLER_RY
#define GAMEPAD_XB360_LX    CONTROLLER_LX
#define GAMEPAD_XB360_LY    CONTROLLER_LY
#define GAMEPAD_XB360_UP    CONTROLLER_UP
#define GAMEPAD_XB360_DOWN  CONTROLLER_DOWN
#define GAMEPAD_XB360_LEFT  CONTROLLER_LEFT
#define GAMEPAD_XB360_RIGHT CONTROLLER_RIGHT
#define GAMEPAD_XB360_Y     CONTROLLER_FACE_NORTH
#define GAMEPAD_XB360_B     CONTROLLER_FACE_EAST
#define GAMEPAD_XB360_A     CONTROLLER_FACE_SOUTH
#define GAMEPAD_XB360_X     CONTROLLER_FACE_WEST

#define GAMEPAD_BRIDGE_STATE_COUNT CONTROLLER_STATE_COUNT

#endif
