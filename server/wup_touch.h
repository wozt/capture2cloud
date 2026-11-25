#ifndef C2C_WUP_TOUCH_H
#define C2C_WUP_TOUCH_H
#include <stdint.h>

typedef struct {
    int touching, last_x, last_y, down_x, down_y;
    int virtual_x, virtual_y, initialized;
    int gesture; /* 0 tap candidate, 1 dragging, 2 right button */
    uint32_t down_at, pulse_until;
    unsigned pulse_button;
} WupTouch;

typedef struct {
    int dx, dy, wheel;
    unsigned buttons; /* PCBLE: 1 = left, 2 = right */
    int active;
} WupTouchEvent;

/* mode 0 relative touchpad; mode 1 virtual absolute position.
 * Absolute mode is a software-position estimate over Joy-Con 2 RELATIVE HID.
 * It cannot query or force the Switch's actual cursor position. */
int wup_touch_step(WupTouch *t, int touched, int x, int y,
                   int mode, uint32_t now, WupTouchEvent *out);
void wup_touch_reset(WupTouch *t);
#endif
