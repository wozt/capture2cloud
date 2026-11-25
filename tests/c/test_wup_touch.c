#include "../../server/wup_touch.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
    WupTouch t = {0};
    WupTouchEvent e;
    wup_touch_step(&t, 1, 100, 200, 0, 1000, &e);
    assert(e.active && e.dx == 0 && e.dy == 0);
    wup_touch_step(&t, 1, 108, 202, 0, 1020, &e);
    assert(e.dx == 8 && !e.buttons);
    wup_touch_step(&t, 0, 108, 202, 0, 1040, &e);
    assert(e.buttons == 1);
    wup_touch_step(&t, 0, 108, 202, 0, 1280, &e);
    assert(e.buttons == 0 && !e.active);
    wup_touch_reset(&t);
    wup_touch_step(&t, 1, 0, 0, 0, 1000, &e);
    wup_touch_step(&t, 1, 50, 0, 0, 1020, &e);
    assert(e.dx == 50 && e.buttons == 1);
    wup_touch_step(&t, 0, 50, 0, 0, 1030, &e);
    assert(!e.buttons);
    wup_touch_reset(&t);
    wup_touch_step(&t, 1, 100, 100, 0, 1000, &e);
    wup_touch_step(&t, 1, 100, 100, 0, 1550, &e);
    assert(e.buttons == 2);
    wup_touch_step(&t, 0, 100, 100, 0, 1600, &e);
    assert(!e.buttons);
    wup_touch_reset(&t);
    wup_touch_step(&t, 1, 0, 0, 1, 1000, &e);
    assert(e.dx == -426 && e.dy == -239);
    wup_touch_step(&t, 1, 853, 479, 1, 1020, &e);
    assert(e.dx == 853 && e.dy == 479 && e.buttons == 1);
    wup_touch_step(&t, 0, 853, 479, 1, 1040, &e);
    wup_touch_step(&t, 1, 420, 240, 1, 1500, &e);
    assert(e.dx == -433 && e.dy == -239);
    puts("wup touch mapping and gestures: OK");
    return 0;
}
