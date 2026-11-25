#include "../../server/wup_touch.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    WupTouch t = {0};
    WupTouchEvent e;
    /* Off: GamePad may keep sending active sticks or touch reports;
     * no mouse packets must be generated. */
    for (int n = 0; n < 100; n++) {
        assert(!wup_touch_step(&t, n % 2, 100 + n, 200, WUP_TOUCH_DISABLED,
                               1000u + (unsigned)n * 16u, &e));
        assert(!e.dx && !e.dy && !e.buttons && !e.active);
    }
    assert(!wup_touch_step(&t, 0, 0, 0, WUP_TOUCH_DISABLED, 2700, &e));

    /* Relative: only physical finger motion, never analog stick movement. */
    assert(!wup_touch_set_mode(&t, WUP_TOUCH_RELATIVE, 0));
    assert(wup_touch_step(&t, 1, 100, 200, WUP_TOUCH_RELATIVE, 3000, &e));
    assert(e.active && !e.dx && !e.dy);
    assert(wup_touch_step(&t, 1, 108, 202, WUP_TOUCH_RELATIVE, 3020, &e));
    assert(e.dx == 8 && !e.buttons);
    assert(wup_touch_step(&t, 0, 108, 202, WUP_TOUCH_RELATIVE, 3040, &e));
    assert(e.buttons == 1);
    assert(wup_touch_step(&t, 0, 108, 202, WUP_TOUCH_RELATIVE, 3280, &e));
    assert(e.buttons == 0 && !e.active);
    assert(!wup_touch_step(&t, 0, 108, 202, WUP_TOUCH_RELATIVE, 3300, &e));

    /* Drag: finger crosses the slop threshold. */
    wup_touch_reset(&t);
    wup_touch_step(&t, 1, 0, 0, WUP_TOUCH_RELATIVE, 1000, &e);
    wup_touch_step(&t, 1, 50, 0, WUP_TOUCH_RELATIVE, 1020, &e);
    assert(e.dx == 50 && e.buttons == 1);
    wup_touch_step(&t, 0, 50, 0, WUP_TOUCH_RELATIVE, 1030, &e);
    assert(!e.buttons);

    /* Long press: right-click held while touching, release on lift. */
    wup_touch_reset(&t);
    wup_touch_step(&t, 1, 100, 100, WUP_TOUCH_RELATIVE, 1000, &e);
    wup_touch_step(&t, 1, 100, 100, WUP_TOUCH_RELATIVE, 1550, &e);
    assert(e.buttons == 2);
    wup_touch_step(&t, 0, 100, 100, WUP_TOUCH_RELATIVE, 1600, &e);
    assert(!e.buttons);

    /* Absolute: virtual coordinate delta, not a raw HID absolute. */
    wup_touch_reset(&t);
    wup_touch_step(&t, 1, 0, 0, WUP_TOUCH_ABSOLUTE, 1000, &e);
    assert(e.dx == -426 && e.dy == -239);
    wup_touch_step(&t, 1, 853, 479, WUP_TOUCH_ABSOLUTE, 1020, &e);
    assert(e.dx == 853 && e.dy == 479 && e.buttons == 1);
    wup_touch_step(&t, 0, 853, 479, WUP_TOUCH_ABSOLUTE, 1040, &e);
    wup_touch_step(&t, 1, 420, 240, WUP_TOUCH_ABSOLUTE, 1500, &e);
    assert(e.dx == -433 && e.dy == -239);

    /* Change while finger is held: release PCBLE now, ignore held finger
     * until it is lifted. The video session never needs a restart. */
    assert(wup_touch_set_mode(&t, WUP_TOUCH_RELATIVE, 1) == 1);
    assert(!wup_touch_step(&t, 1, 421, 240, WUP_TOUCH_RELATIVE, 1520, &e));
    assert(!wup_touch_step(&t, 0, 421, 240, WUP_TOUCH_RELATIVE, 1530, &e));
    assert(wup_touch_step(&t, 1, 421, 240, WUP_TOUCH_RELATIVE, 1550, &e));
    assert(!e.dx && !e.dy && e.active);
    assert(wup_touch_set_mode(&t, WUP_TOUCH_DISABLED, 1) == 1);
    assert(!wup_touch_step(&t, 1, 422, 241, WUP_TOUCH_DISABLED, 1570, &e));
    assert(!e.dx && !e.dy && !e.buttons && !e.active);

    /* Off -> absolute while already touching should wait for lift. */
    assert(!wup_touch_set_mode(&t, WUP_TOUCH_ABSOLUTE, 1));
    assert(!wup_touch_step(&t, 1, 200, 200, WUP_TOUCH_ABSOLUTE, 1580, &e));
    assert(!wup_touch_step(&t, 0, 200, 200, WUP_TOUCH_ABSOLUTE, 1590, &e));
    assert(wup_touch_step(&t, 1, 300, 200, WUP_TOUCH_ABSOLUTE, 1600, &e));
    assert(e.dx == -126 && e.dy == -39);
    puts("WUP touch disabled/relative/absolute and live switching: OK");
    return 0;
}
