#include "local_pad.h"

#include "gamepad_bridge.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_CONTROLLERS 8
#define STICK_MAX 32767.0f

static SDL_GameController *g_open = NULL;
static int g_open_index = -1;
static char g_names[MAX_CONTROLLERS][96];

int local_pad_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "local_pad: no controller support (%s)\n", SDL_GetError());
        return -1;
    }
    return 0;
}

void local_pad_shutdown(void) {
    if (g_open) {
        SDL_GameControllerClose(g_open);
        g_open = NULL;
        g_open_index = -1;
    }
    /* Whatever it was holding goes with it. */
    gamepad_bridge_forget(GAMEPAD_SOURCE_LOCAL);
}

int local_pad_list(const char **names, int max_names) {
    int n = 0;
    for (int i = 0; i < SDL_NumJoysticks() && n < max_names && n < MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        const char *name = SDL_GameControllerNameForIndex(i);
        snprintf(g_names[n], sizeof(g_names[n]), "%s", name ? name : "controller");
        names[n] = g_names[n];
        n++;
    }
    return n;
}

/* Both axes together, and the limits applied to the LENGTH of the
 * vector rather than to each axis -- the same reasoning, and the same
 * arithmetic, as the page and the console client. A stick is round:
 * judged per axis, a corner reads as three quarters of a push whatever
 * the limits are. The result is scaled so the larger component reaches
 * 100, so a full corner is 100/100. */
static void stick_to_wire(float x, float y, int dead_pct, int range_pct, int diagonal_pct,
                          int8_t *out_x, int8_t *out_y) {
    *out_x = 0;
    *out_y = 0;

    const float mag = sqrtf(x * x + y * y);
    if (mag < 0.0001f) {
        return;
    }
    const float ax = fabsf(x), ay = fabsf(y);
    const float peak = ax > ay ? ax : ay;
    const float least = ax > ay ? ay : ax;
    const float diagonality = peak > 0.0001f ? least / peak : 0.0f;

    const float dead = dead_pct / 100.0f;
    if (mag <= dead) {
        return;
    }
    const float sat_axis = range_pct / 100.0f;
    const float sat_diag = diagonal_pct / 100.0f;
    float sat = sat_axis + (sat_diag - sat_axis) * diagonality;
    if (sat <= dead) {
        sat = dead + 0.1f;
    }

    float t = (mag - dead) / (sat - dead);
    if (t > 1.0f) {
        t = 1.0f;
    }
    const float scale = t * 100.0f / peak;
    int px = (int)(x * scale + (x < 0 ? -0.5f : 0.5f));
    int py = (int)(y * scale + (y < 0 ? -0.5f : 0.5f));
    if (px > 100) px = 100;
    if (px < -100) px = -100;
    if (py > 100) py = 100;
    if (py < -100) py = -100;
    *out_x = (int8_t)px;
    *out_y = (int8_t)py;
}

static int8_t button(SDL_GameController *c, SDL_GameControllerButton b) {
    return SDL_GameControllerGetButton(c, b) ? 100 : 0;
}

void local_pad_poll(const AppSettings *s) {
    if (!s->gamepad_enabled || s->gamepad_index < 0) {
        if (g_open) {
            SDL_GameControllerClose(g_open);
            g_open = NULL;
            g_open_index = -1;
            gamepad_bridge_forget(GAMEPAD_SOURCE_LOCAL);
        }
        return;
    }

    /* The chosen one, opened when it changes. SDL's joystick index is
     * what the settings window's list is built from, so the two agree by
     * construction. */
    if (g_open_index != s->gamepad_index) {
        if (g_open) {
            SDL_GameControllerClose(g_open);
            g_open = NULL;
            gamepad_bridge_forget(GAMEPAD_SOURCE_LOCAL);
        }
        int seen = -1;
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (!SDL_IsGameController(i)) {
                continue;
            }
            if (++seen == s->gamepad_index) {
                g_open = SDL_GameControllerOpen(i);
                break;
            }
        }
        g_open_index = g_open ? s->gamepad_index : -1;
        if (!g_open) {
            return;
        }
    }

    SDL_GameControllerUpdate();
    if (!SDL_GameControllerGetAttached(g_open)) {
        /* Unplugged mid-session. Releasing what it held matters more
         * than noticing quickly: a button left pressed on the console is
         * worse than a controller that has to be chosen again. */
        SDL_GameControllerClose(g_open);
        g_open = NULL;
        g_open_index = -1;
        gamepad_bridge_forget(GAMEPAD_SOURCE_LOCAL);
        return;
    }

    int8_t pad[GAMEPAD_BRIDGE_STATE_COUNT];
    memset(pad, 0, sizeof(pad));

    pad[GAMEPAD_XB360_A] = button(g_open, SDL_CONTROLLER_BUTTON_A);
    pad[GAMEPAD_XB360_B] = button(g_open, SDL_CONTROLLER_BUTTON_B);
    pad[GAMEPAD_XB360_X] = button(g_open, SDL_CONTROLLER_BUTTON_X);
    pad[GAMEPAD_XB360_Y] = button(g_open, SDL_CONTROLLER_BUTTON_Y);
    pad[GAMEPAD_XB360_LB] = button(g_open, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    pad[GAMEPAD_XB360_RB] = button(g_open, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    pad[GAMEPAD_XB360_LS] = button(g_open, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    pad[GAMEPAD_XB360_RS] = button(g_open, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    pad[GAMEPAD_XB360_START] = button(g_open, SDL_CONTROLLER_BUTTON_START);
    pad[GAMEPAD_XB360_BACK] = button(g_open, SDL_CONTROLLER_BUTTON_BACK);
    pad[GAMEPAD_XB360_GUIDE] = button(g_open, SDL_CONTROLLER_BUTTON_GUIDE);
    pad[GAMEPAD_XB360_UP] = button(g_open, SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad[GAMEPAD_XB360_DOWN] = button(g_open, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad[GAMEPAD_XB360_LEFT] = button(g_open, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad[GAMEPAD_XB360_RIGHT] = button(g_open, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    /* Triggers come back as axes on most pads, thresholded here the way
     * the page thresholds them -- the resting value differs enough
     * between controllers that a fixed one would either stick on or
     * never fire. */
    const float lt = SDL_GameControllerGetAxis(g_open, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / STICK_MAX;
    const float rt = SDL_GameControllerGetAxis(g_open, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / STICK_MAX;
    pad[GAMEPAD_XB360_LT] = lt > s->lt_threshold / 100.0f ? 100 : 0;
    pad[GAMEPAD_XB360_RT] = rt > s->rt_threshold / 100.0f ? 100 : 0;

    stick_to_wire(SDL_GameControllerGetAxis(g_open, SDL_CONTROLLER_AXIS_LEFTX) / STICK_MAX,
                  SDL_GameControllerGetAxis(g_open, SDL_CONTROLLER_AXIS_LEFTY) / STICK_MAX,
                  s->stick_deadzone[0], s->stick_range[0], s->stick_diagonal[0],
                  &pad[GAMEPAD_XB360_LX], &pad[GAMEPAD_XB360_LY]);

    int8_t rx = 0, ry = 0;
    stick_to_wire(SDL_GameControllerGetAxis(g_open, SDL_CONTROLLER_AXIS_RIGHTX) / STICK_MAX,
                  SDL_GameControllerGetAxis(g_open, SDL_CONTROLLER_AXIS_RIGHTY) / STICK_MAX,
                  s->stick_deadzone[1], s->stick_range[1], s->stick_diagonal[1], &rx, &ry);
    pad[GAMEPAD_XB360_RX] = rx;
    pad[GAMEPAD_XB360_RY] = s->invert_ry ? (int8_t)(-ry) : ry;

    gamepad_bridge_update(GAMEPAD_SOURCE_LOCAL, pad);
}
