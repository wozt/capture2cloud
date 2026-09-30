#include "input.h"

#include <stdlib.h>
#include <string.h>

static int8_t axis_value(int value)
{
    if (value > 156) value = 156;
    if (value < -156) value = -156;
    int scaled = value * 100 / 156;
    if (abs(scaled) < 5) scaled = 0;
    return (int8_t)scaled;
}

void input_build_state(int8_t state[C2S_PAD_SLOTS],
                       const TouchControls *touch,
                       bool controller_mode)
{
    memset(state, 0, C2S_PAD_SLOTS);
    if (!controller_mode) return;

    u32 held = hidKeysHeld();
    /* The wire format names these four slots after the Xbox layout,
     * while both the 3DS and the remote Nintendo controller label the
     * same physical positions A/B and X/Y the other way around.  Map by
     * Nintendo label here so pressing A on the 3DS produces A remotely. */
    state[PAD_B] = (held & KEY_A) ? 100 : 0;
    state[PAD_A] = (held & KEY_B) ? 100 : 0;
    state[PAD_Y] = (held & KEY_X) ? 100 : 0;
    state[PAD_X] = (held & KEY_Y) ? 100 : 0;
    state[PAD_L] = (held & KEY_L) ? 100 : 0;
    state[PAD_R] = (held & KEY_R) ? 100 : 0;
    state[PAD_PLUS] = ((held & KEY_START) || touch->plus) ? 100 : 0;
    state[PAD_MINUS] = ((held & KEY_SELECT) || touch->minus) ? 100 : 0;
    state[PAD_UP] = (held & KEY_DUP) ? 100 : 0;
    state[PAD_DOWN] = (held & KEY_DDOWN) ? 100 : 0;
    state[PAD_LEFT] = (held & KEY_DLEFT) ? 100 : 0;
    state[PAD_RIGHT] = (held & KEY_DRIGHT) ? 100 : 0;
    state[PAD_ZL] = touch->zl ? 100 : 0;
    state[PAD_ZR] = touch->zr ? 100 : 0;
    state[PAD_L3] = touch->l3 ? 100 : 0;
    state[PAD_R3] = touch->r3 ? 100 : 0;
    state[PAD_RX] = touch->rx;
    state[PAD_RY] = touch->ry;

    circlePosition circle;
    hidCircleRead(&circle);
    state[PAD_LX] = axis_value(circle.dx);
    state[PAD_LY] = axis_value(-circle.dy);
}
