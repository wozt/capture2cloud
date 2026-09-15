#ifndef CAPTURE2WIIU_INPUT_H
#define CAPTURE2WIIU_INPUT_H

#include <stdint.h>

#include "c2s_protocol.h"

/*
 * The controller state, in exactly the layout the host already speaks.
 *
 * Only the layout lives here so far: reading the GamePad is step two in
 * SPEC.md, and net.h needs the type before then. The slot count comes
 * from the shared protocol header rather than being repeated, so the two
 * cannot disagree.
 *
 *
 * TWO THINGS THAT ARE WRONG BY DEFAULT, AND WERE WRONG ON THE LAST
 * CLIENT TOO. Read these before writing the mapping in step two.
 *
 * The vertical axes are inverted. The host wants up positive. The
 * previous client's handover document said libdrc already reported it
 * that way and not to negate it -- and on the bench, pushing up moved
 * the character down until it was negated. Whatever VPAD turns out to
 * report, check it against the console before believing a document, and
 * make it a setting so it can be flipped without a rebuild.
 *
 * The face buttons do not mean what they say. This is a Nintendo pad
 * and the adapter on the other end pretends to be an Xbox 360, whose
 * four letters sit in different places: Nintendo's A is on the right
 * where Xbox's B is, and Nintendo's X is on top where Xbox's Y is. Map
 * by POSITION, not by letter, or every on-screen prompt lands on the
 * wrong button. Also a setting.
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

#endif /* CAPTURE2WIIU_INPUT_H */
