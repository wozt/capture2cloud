#ifndef CAPTURE2WIIU_INPUT_H
#define CAPTURE2WIIU_INPUT_H

#include <stddef.h>
#include <stdint.h>

#include "c2s_protocol.h"

/*
 * Exact 21-slot layout expected by capture2cloud/gamepad_bridge.
 *
 * Values:
 *   buttons/triggers: 0 or 100
 *   sticks:          -100 .. +100
 */
#define PAD_SLOT_COUNT C2S_PAD_SLOTS

enum {
    PAD_GUIDE = 0,
    PAD_BACK  = 1,
    PAD_START = 2,
    PAD_RB    = 3,
    PAD_RT    = 4,
    PAD_RS    = 5,
    PAD_LB    = 6,
    PAD_LT    = 7,
    PAD_LS    = 8,
    PAD_RX    = 9,
    PAD_RY    = 10,
    PAD_LX    = 11,
    PAD_LY    = 12,
    PAD_UP    = 13,
    PAD_DOWN  = 14,
    PAD_LEFT  = 15,
    PAD_RIGHT = 16,
    PAD_Y     = 17,
    PAD_B     = 18,
    PAD_A     = 19,
    PAD_X     = 20
};

typedef int8_t PadState21[PAD_SLOT_COUNT];

/*
 * SDL owns VPAD in this client already.
 *
 * We open the raw Wii U GamePad joystick so the mapping is unambiguous:
 * physical Nintendo buttons are then converted by POSITION to the
 * Xbox-style 21-slot wire format.
 */
int  input_init(char *why, size_t why_size);
void input_exit(void);

/*
 * Samples the GamePad once.
 *
 * forward != 0:
 *     forward the sampled state to the host.
 *
 * forward == 0:
 *     keep sampling for diagnostics but send a neutral pad. This is used
 *     while the local menu is open so menu interaction cannot affect the
 *     remote game.
 */
void input_update(int forward);

int  input_available(void);

/* Last physical state sampled, even when forwarding is disabled. */
void input_snapshot(PadState21 out);

#endif
