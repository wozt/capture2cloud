#ifndef OUTPUT_BACKEND_H
#define OUTPUT_BACKEND_H

#include "controller_state.h"

/*
 * Console-output backend contract.
 *
 * gamepad_bridge owns input sources and combines them into one
 * ControllerState.  A backend only has to deliver that final state to
 * the real console.
 *
 * Planned implementations:
 *   - Titan/ConsoleTuner USB (current)
 *   - pcble2joycon2 daemon
 *   - JOCP / JoypadOS-compatible Pico 2 W
 */
typedef struct {
    const char *name;

    int  (*init)(void);
    void (*update)(const int8_t state[CONTROLLER_STATE_COUNT]);
    void (*reset)(void);
    void (*press_home)(void);

    int    (*link_up)(void);
    double (*report_rate)(void);

    void (*shutdown)(void);
} GamepadOutputBackend;

#endif
