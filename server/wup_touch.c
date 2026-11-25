#include "wup_touch.h"
#include <stdlib.h>
#include <string.h>

#define WUP_TOUCH_MAX_X 853
#define WUP_TOUCH_MAX_Y 479
#define TOUCH_SLOP 9
#define LONG_PRESS_MS 500u
#define TAP_MAX_MS 500u
#define CLICK_PULSE_MS 220u /* survive the PCBLE 100 ms send cadence */

static int clamp(int x, int lo, int hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

void wup_touch_reset(WupTouch *t) {
    if (t) memset(t, 0, sizeof(*t));
}

int wup_touch_set_mode(WupTouch *t, int mode, int physical_contact) {
    if (!t) return 0;
    if (mode < WUP_TOUCH_DISABLED || mode > WUP_TOUCH_ABSOLUTE)
        mode = WUP_TOUCH_DISABLED;
    if (t->mode_initialized && t->mode == mode) return 0;
    const int was_initialized = t->mode_initialized;
    const int release_mouse = was_initialized &&
                              (t->touching || t->pulse_button);
    /* Reset gesture, virtual pointer and pending click atomically with
     * the mode switch. A held finger must be lifted before starting a
     * fresh gesture in the new mode. No stream reconnect is required. */
    wup_touch_reset(t);
    t->mode_initialized = 1;
    t->mode = mode;
    t->wait_for_release = was_initialized && physical_contact;
    return release_mouse;
}

int wup_touch_step(WupTouch *t, int touched, int x, int y,
                   int mode, uint32_t now, WupTouchEvent *out) {
    if (!t || !out) return 0;
    memset(out, 0, sizeof(*out));
    /* A caller that doesn't explicitly call set_mode can still switch
     * modes, and gets a neutral report to release a held mouse button. */
    if (wup_touch_set_mode(t, mode, touched)) return 1;
    if (t->mode == WUP_TOUCH_DISABLED) return 0;
    if (t->wait_for_release) {
        if (!touched) t->wait_for_release = 0;
        return 0;
    }
    x = clamp(x, 0, WUP_TOUCH_MAX_X);
    y = clamp(y, 0, WUP_TOUCH_MAX_Y);
    if (!t->initialized) {
        t->initialized = 1;
        t->virtual_x = WUP_TOUCH_MAX_X / 2;
        t->virtual_y = WUP_TOUCH_MAX_Y / 2;
    }
    if (touched) {
        if (!t->touching) {
            t->touching = 1;
            t->down_x = t->last_x = x;
            t->down_y = t->last_y = y;
            t->down_at = now;
            t->gesture = 0;
            t->pulse_until = 0;
            t->pulse_button = 0;
            if (t->mode == WUP_TOUCH_ABSOLUTE) {
                out->dx = x - t->virtual_x;
                out->dy = y - t->virtual_y;
                t->virtual_x = x;
                t->virtual_y = y;
            }
        } else {
            if (t->mode == WUP_TOUCH_ABSOLUTE) {
                out->dx = x - t->virtual_x;
                out->dy = y - t->virtual_y;
                t->virtual_x = x;
                t->virtual_y = y;
            } else {
                out->dx = x - t->last_x;
                out->dy = y - t->last_y;
                t->virtual_x = clamp(t->virtual_x + out->dx, 0, WUP_TOUCH_MAX_X);
                t->virtual_y = clamp(t->virtual_y + out->dy, 0, WUP_TOUCH_MAX_Y);
            }
            t->last_x = x;
            t->last_y = y;
        }
        int mx = abs(x - t->down_x), my = abs(y - t->down_y);
        if (!t->gesture && (mx > TOUCH_SLOP || my > TOUCH_SLOP))
            t->gesture = 1;
        if (!t->gesture && (uint32_t)(now - t->down_at) >= LONG_PRESS_MS)
            t->gesture = 2;
        out->buttons = t->gesture == 1 ? 1u : t->gesture == 2 ? 2u : 0u;
        out->active = 1;
        return 1;
    }
    /* Do not overwrite other sources when no finger or click pulse exists. */
    if (!t->touching && !t->pulse_button) return 0;
    if (t->touching) {
        t->touching = 0;
        if (!t->gesture && (uint32_t)(now - t->down_at) < TAP_MAX_MS) {
            t->pulse_button = 1u;
            t->pulse_until = now + CLICK_PULSE_MS;
        } else if (!t->gesture) {
            t->pulse_button = 2u;
            t->pulse_until = now + CLICK_PULSE_MS;
        }
        t->gesture = 0;
    }
    if (t->pulse_button && (int32_t)(t->pulse_until - now) > 0) {
        out->buttons = t->pulse_button;
        out->active = 1;
    } else {
        t->pulse_button = 0;
        t->pulse_until = 0;
    }
    return 1;
}
