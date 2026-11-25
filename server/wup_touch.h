#ifndef C2C_WUP_TOUCH_H
#define C2C_WUP_TOUCH_H
#include <stdint.h>

enum {
    WUP_TOUCH_DISABLED = 0,
    WUP_TOUCH_RELATIVE = 1,
    WUP_TOUCH_ABSOLUTE = 2
};

typedef struct {
    int touching, last_x, last_y, down_x, down_y;
    int virtual_x, virtual_y, initialized;
    int gesture; /* 0 tap candidate, 1 dragging, 2 right button */
    uint32_t down_at, pulse_until;
    unsigned pulse_button;
    int mode, mode_initialized, wait_for_release;
} WupTouch;

typedef struct {
    int dx, dy, wheel;
    unsigned buttons; /* PCBLE: 1 = left, 2 = right */
    int active;
} WupTouchEvent;

/* Mode changes are immediate, including without a fresh WUP input report.
 * Returns 1 when a previous touch/button may still be held in PCBLE and
 * must be explicitly released. If a finger is down during the switch,
 * ignore it until lifted; otherwise changing modes could cause a click. */
int wup_touch_set_mode(WupTouch *t, int mode, int physical_contact);

/* Only actual touchscreen contact produces motion; stick axes are never
 * inputs to this state machine. Off=0, relative=1, virtual absolute=2.
 * Absolute remains an estimate: PCBLE transports RELATIVE HID motion. */
int wup_touch_step(WupTouch *t, int touched, int x, int y,
                   int mode, uint32_t now, WupTouchEvent *out);
void wup_touch_reset(WupTouch *t);
#endif
