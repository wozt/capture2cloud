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
 * Implementations:
 *   - Titan/ConsoleTuner USB
 *   - vendored pcble Classic Bluetooth HID
 *
 * Planned:
 *   - JOCP / JoypadOS-compatible Pico 2 W
 */
typedef struct {
    const char *name;

    int  (*init)(void);
    void (*update)(const int8_t state[CONTROLLER_STATE_COUNT]);
    void (*reset)(void);
    void (*press_home)(void);

    /*
     * Optional non-blocking maintenance state machine.
     *
     * reset() may only request recovery; service() advances it from the
     * application's main loop without blocking a network/client thread.
     */
    void (*service)(void);

    int    (*link_up)(void);
    double (*report_rate)(void);

    void (*shutdown)(void);
} GamepadOutputBackend;

#endif
