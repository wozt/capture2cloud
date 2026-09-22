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
#define INPUT_BUTTON_COUNT 16

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

typedef struct {
    uint8_t deadzone[2];       /* percent, left/right */
    uint8_t range[2];          /* percent that counts as fully pushed */
    uint8_t invert_y;
    uint8_t face_by_position;  /* legacy config migration only */
    uint8_t button_map[INPUT_BUTTON_COUNT]; /* physical index -> PAD_* */
} InputConfig;

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

void input_set_config(const InputConfig *config);
void input_get_config(InputConfig *config);

/* Full digital-button binding support. The default uses Xbox positions
 * for A/B/X/Y. Binding swaps the displaced physical button so the map
 * always remains a permutation with no duplicate output. */
void input_config_reset_bindings(InputConfig *config);
void input_config_sanitize(InputConfig *config);
void input_config_bind(InputConfig *config, int physical_button, int pad_slot);
int  input_config_physical_for_slot(const InputConfig *config, int pad_slot);
const char *input_physical_button_name(int physical_button);
const char *input_pad_slot_name(int pad_slot);

/* Rising edge from the raw Wii U buttons, independent of their mapping. */
int  input_take_physical_button(void);
void input_clear_physical_buttons(void);

#endif
