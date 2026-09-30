#pragma once

#include "c2s_protocol.h"

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

enum {
    PAD_HOME = 0,
    PAD_MINUS = 1,
    PAD_PLUS = 2,
    PAD_R = 3,
    PAD_ZR = 4,
    PAD_R3 = 5,
    PAD_L = 6,
    PAD_ZL = 7,
    PAD_L3 = 8,
    PAD_RX = 9,
    PAD_RY = 10,
    PAD_LX = 11,
    PAD_LY = 12,
    PAD_UP = 13,
    PAD_DOWN = 14,
    PAD_LEFT = 15,
    PAD_RIGHT = 16,
    PAD_Y = 17,
    PAD_B = 18,
    PAD_A = 19,
    PAD_X = 20
};

typedef struct {
    bool zl;
    bool zr;
    bool l3;
    bool r3;
    bool minus;
    bool plus;
    int8_t rx;
    int8_t ry;
} TouchControls;

void input_build_state(int8_t state[C2S_PAD_SLOTS],
                       const TouchControls *touch,
                       bool controller_mode);

