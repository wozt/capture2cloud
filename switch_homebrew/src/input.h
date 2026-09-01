#ifndef CAPTURE2SWITCH_INPUT_H
#define CAPTURE2SWITCH_INPUT_H

#include <stddef.h>
#include <stdint.h>

#include "c2s_protocol.h"

/*
 * The controller state, in exactly the layout the host already speaks --
 * the slot count comes from the shared protocol header rather than being
 * repeated here, so the two cannot disagree.
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

/* Opens the controller and starts the polling thread. Returns 0 on
 * success. */
int input_init(void);
void input_exit(void);

/* Where a stick's usable travel starts and ends, as a percentage of the
 * raw range, for stick 0 (left) or 1 (right).
 *
 * The outer limit is the important one: a stick that no longer reaches
 * its electrical maximum sends 70 or 80 when pushed all the way, which
 * reads on the other end as a gentle push. Setting it to what the stick
 * actually reaches makes the end of the travel mean full deflection
 * again. Per stick, because the two wear at different rates. */
/* `saturation_pct` is the outer limit along an axis and `diagonal_pct`
 * the one at 45 degrees; the two blend by how diagonal the direction is.
 * A stick reaches less far into a corner than along an axis, and by how
 * much differs from one to the next, so a single number could not
 * describe both. */
void input_set_stick_limits(int stick, int deadzone_pct, int saturation_pct, int diagonal_pct);
void input_get_stick_limits(int stick, int *deadzone_pct, int *saturation_pct, int *diagonal_pct);

/* Another source of pad state, folded into every sample.
 *
 * The on-screen pad uses this. Called from the polling thread, so
 * whatever it reads has to be safe to read from there -- and doing it
 * here rather than in the frame loop is what gives touch input the same
 * rate as the sticks instead of the drawing's. */
void input_set_merge_hook(void (*hook)(PadState21));

/* Run just before each sample, for a source that has to read something
 * of its own first -- the touchscreen. */
void input_set_poll_hook(void (*hook)(void));

/* Whether the polling thread forwards what it reads to the host. Off
 * while a local screen is up, so navigating the menu never reaches the
 * remote console. */
void input_set_forwarding(int on);

/* How many times this slot has been pressed since the last call, and
 * zero afterwards.
 *
 * Menus use this rather than comparing one frame with the next: a press
 * and release between two frames is invisible to that comparison, and
 * the frame loop slows down whenever there is more to decode -- so the
 * menu stopped answering exactly when the picture got busy. */
int  input_take_press(int slot);
void input_clear_presses(void);

/* What the console is giving this program: "none", or the styles that
 * are attached ("handheld", "joy-dual", "pro"...). */
void input_describe_pads(char *out, size_t out_size);

/* Fills `out` with the most recent sample, for drawing. The stream
 * itself is fed by the polling thread, not from here: sending on the
 * frame loop meant the hand moved at the speed of the decoder. */
void input_read(PadState21 out);

/* --- gestures -------------------------------------------------------
 *
 * Both are edge-triggered: they report once per gesture, not once per
 * frame while it is held, so a caller cannot accidentally act on the
 * same press repeatedly.
 */

/* Start+Select, one gesture measured at two lengths:
 *
 *   ~1 s  HOME, forwarded to the remote Switch 2
 *   ~5 s  quit
 *
 * HOME fires on RELEASE, so holding on toward quit does not send one on
 * the way past. Each reports once per hold. */
int input_take_home_gesture(void);
int input_take_quit_gesture(void);

/* L3+R3, or a tap near the top of the screen. Pressed rather than held:
 * the way back out of the stream should not itself need a knack. */
int input_take_menu_gesture(void);

/* A tap near the top of the screen -- the handheld-mode way in, since
 * holding two buttons is awkward with the console in your hands. Fed by
 * SDL touch events, so the caller passes them in. */
void input_feed_touch(float normalised_y, int began);

#endif
