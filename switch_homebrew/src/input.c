#include "input.h"

#include <stdio.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <switch.h>

#include "net.h"

/*
 * The controller is read on its own thread, not on the frame loop.
 *
 * It used to be sampled once per drawn picture, which tied the hand to
 * the decoder: at 30 fps the stick was read 30 times a second, and every
 * hitch in decoding was also a hitch in steering. A short press falling
 * between two samples was missed entirely, and a held button that
 * happened to be sampled twice looked like a hold that outlived it. None
 * of that is the frame loop's business, so it no longer does it.
 *
 * At 250 Hz the sampling is four times finer than the console's own
 * 60 Hz output and finer than the remote console can act on, so the
 * limit is the network rather than this.
 *
 * libnx directly rather than through SDL: SDL's controller state is
 * refreshed by the event pump, which runs on the frame loop -- reading
 * it from here would sample the same stale value faster.
 */
#define POLL_INTERVAL_MS 4

/* Sends only on change, plus a heartbeat, so an idle pad costs ~10
 * packets a second instead of 250 while a moving stick still gets every
 * sample. */
#define KEEPALIVE_MS 100

/* Start+Select, at two lengths. Both are long enough that the pair
 * pressed in passing during a game does nothing. */
#define HOME_HOLD_MS 1000
#define QUIT_HOLD_MS 5000

/* A tap in the top eighth of the screen opens the menu in handheld mode.
 * Small enough not to be hit while playing, big enough to find without
 * aiming. */
#define MENU_TOUCH_ZONE 0.125f

/* Sticks report -32768..32767; the wire format is -100..100. */
#define STICK_MAX 32767

/* Where each stick's usable travel starts and ends, per stick, as a
 * percentage of the raw range.
 *
 * The outer limit is the one that matters and the one that was missing.
 * A worn or simply ordinary stick does not reach its electrical maximum,
 * least of all on a diagonal, so pushing it all the way sent 70 or 80
 * rather than 100 -- read on the other end as a gentle push. Everything
 * at or beyond the outer limit is now full deflection, and the range
 * between the two limits is stretched across the whole scale, so the
 * middle of the travel stays proportional instead of jumping.
 *
 * Per stick because the two wear differently: the left one takes most of
 * the movement in most games and goes first. */
static int g_deadzone[2] = {2, 2};       /* percent; below this, centred */
static int g_saturation[2] = {100, 100}; /* percent; at this, full, on an axis */
static int g_diagonal[2] = {100, 100};   /* percent; at this, full, at 45 degrees */

static PadState g_hid;
static SDL_Thread *g_thread = NULL;
static SDL_mutex *g_lock = NULL;
static volatile int g_running = 0;
static volatile int g_forward = 0;
static void (*g_merge_hook)(PadState21) = NULL;
static void (*g_poll_hook)(void) = NULL;

static PadState21 g_snapshot;

/* New presses since the frame loop last looked, counted per slot.
 *
 * The menu used to compare this frame's state with the previous frame's,
 * which only ever sees what a button was doing at the two instants the
 * frame loop happened to look. A press and release between two frames is
 * invisible, and the frame loop slows down whenever there is more to
 * decode -- so the menu stopped responding exactly when the picture got
 * busy, which reads as "the confirm button no longer works".
 *
 * Counted by the thread that samples at 250 Hz and drained by whoever
 * asks, so every press is acted on once, whatever the drawing is
 * doing. */
static uint8_t g_presses[PAD_SLOT_COUNT];
static int g_home_pending = 0;
static int g_menu_pending = 0;
static int g_quit_pending = 0;
static int g_touch_menu_pending = 0;

void input_set_stick_limits(int stick, int deadzone_pct, int saturation_pct, int diagonal_pct) {
    if (stick < 0 || stick > 1) {
        return;
    }
    if (deadzone_pct < 0) deadzone_pct = 0;
    if (deadzone_pct > 40) deadzone_pct = 40;
    if (saturation_pct < deadzone_pct + 10) saturation_pct = deadzone_pct + 10;
    if (saturation_pct > 100) saturation_pct = 100;
    if (diagonal_pct < deadzone_pct + 10) diagonal_pct = deadzone_pct + 10;
    if (diagonal_pct > 100) diagonal_pct = 100;
    g_deadzone[stick] = deadzone_pct;
    g_saturation[stick] = saturation_pct;
    g_diagonal[stick] = diagonal_pct;
}

void input_get_stick_limits(int stick, int *deadzone_pct, int *saturation_pct, int *diagonal_pct) {
    if (stick < 0 || stick > 1) {
        return;
    }
    if (deadzone_pct) *deadzone_pct = g_deadzone[stick];
    if (saturation_pct) *saturation_pct = g_saturation[stick];
    if (diagonal_pct) *diagonal_pct = g_diagonal[stick];
}

/* Both axes together, because a stick is round -- and full deflection
 * means both axes at their maximum, wherever it is pointed.
 *
 * Each axis used to be scaled on its own, which is wrong the moment the
 * stick leaves the cardinal directions: the hardware limits the two
 * together, so pushing fully into a corner never gives 1.0 on each axis.
 * Judged per axis that read as three quarters of a push.
 *
 * Two things fix it. The limits apply to the LENGTH of the vector rather
 * than to each axis, so the direction survives; and the result is scaled
 * so the LARGER of the two components reaches 100 -- a corner comes out
 * as 100/100 rather than 71/71, which is what "as far as it goes"
 * should mean for something being handed to a game.
 *
 * A corner also has its own outer limit, because a stick reaches less
 * far diagonally than it does along an axis and the gap differs from
 * stick to stick. The two limits blend by how diagonal the direction is,
 * so there is no seam anywhere in the circle. */
static void stick_to_wire(int stick, int raw_x, int raw_y, int8_t *out_x, int8_t *out_y) {
    *out_x = 0;
    *out_y = 0;

    const float x = (float)raw_x / STICK_MAX;
    const float y = (float)raw_y / STICK_MAX;
    const float mag = sqrtf(x * x + y * y);
    if (mag <= 0.0001f) {
        return;
    }

    const float ax = fabsf(x), ay = fabsf(y);
    const float peak = (ax > ay) ? ax : ay;   /* the larger component */
    const float least = (ax > ay) ? ay : ax;
    /* 0 straight along an axis, 1 at exactly 45 degrees. */
    const float diagonality = (peak > 0.0001f) ? least / peak : 0.0f;

    const float dead = g_deadzone[stick] / 100.0f;
    if (mag <= dead) {
        return;
    }
    const float sat_axis = g_saturation[stick] / 100.0f;
    const float sat_diag = g_diagonal[stick] / 100.0f;
    const float sat = sat_axis + (sat_diag - sat_axis) * diagonality;

    /* How far along the usable travel, 0 at the deadzone and 1 at the
     * outer limit -- so the middle of the range stays proportional
     * rather than jumping. */
    float t = (sat > dead) ? (mag - dead) / (sat - dead) : 1.0f;
    if (t > 1.0f) {
        t = 1.0f;
    }

    /* Scaled by the larger component, not by the length: at the outer
     * limit the dominant axis is exactly 100 whatever the direction. */
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

/* The wire format is an Xbox pad, whose face buttons sit in the mirror
 * positions of this console's. Mapped by POSITION, not by name: the
 * bottom button here is Nintendo's B, and it has to arrive as the
 * bottom button there. Naming them across would swap confirm and cancel
 * on the remote console. */
static void sample(PadState21 out) {
    padUpdate(&g_hid);
    u64 held = padGetButtons(&g_hid);

    memset(out, 0, sizeof(PadState21));

    out[PAD_A] = (held & HidNpadButton_B) ? 100 : 0;  /* bottom */
    out[PAD_B] = (held & HidNpadButton_A) ? 100 : 0;  /* right  */
    out[PAD_X] = (held & HidNpadButton_Y) ? 100 : 0;  /* left   */
    out[PAD_Y] = (held & HidNpadButton_X) ? 100 : 0;  /* top    */

    out[PAD_LB] = (held & HidNpadButton_L) ? 100 : 0;
    out[PAD_RB] = (held & HidNpadButton_R) ? 100 : 0;
    out[PAD_LT] = (held & HidNpadButton_ZL) ? 100 : 0;
    out[PAD_RT] = (held & HidNpadButton_ZR) ? 100 : 0;
    out[PAD_LS] = (held & HidNpadButton_StickL) ? 100 : 0;
    out[PAD_RS] = (held & HidNpadButton_StickR) ? 100 : 0;

    out[PAD_START] = (held & HidNpadButton_Plus) ? 100 : 0;
    out[PAD_BACK]  = (held & HidNpadButton_Minus) ? 100 : 0;

    out[PAD_UP]    = (held & HidNpadButton_Up) ? 100 : 0;
    out[PAD_DOWN]  = (held & HidNpadButton_Down) ? 100 : 0;
    out[PAD_LEFT]  = (held & HidNpadButton_Left) ? 100 : 0;
    out[PAD_RIGHT] = (held & HidNpadButton_Right) ? 100 : 0;

    HidAnalogStickState l = padGetStickPos(&g_hid, 0);
    HidAnalogStickState r = padGetStickPos(&g_hid, 1);
    /* Y is inverted between the two: up is positive here, down is
     * positive on the wire. */
    stick_to_wire(0, l.x, -l.y, &out[PAD_LX], &out[PAD_LY]);
    stick_to_wire(1, r.x, -r.y, &out[PAD_RX], &out[PAD_RY]);
}

/* Both gestures are edge-triggered: each reports once per gesture rather
 * than once per sample while it is held. */
static void gestures(PadState21 out) {
    static Uint32 combo_since = 0;  /* Start+Select */
    static int sticks_were_down = 0;

    Uint32 now = SDL_GetTicks();

    /* L3+R3 opens the menu. Instant, and out of the way of anything a
     * game asks for: a gesture that needs holding is the wrong thing for
     * the door out of the stream. */
    int sticks = out[PAD_LS] && out[PAD_RS];
    if (sticks) {
        if (!sticks_were_down) {
            g_menu_pending = 1;
        }
        /* Not forwarded: reaching for the menu must not also click both
         * sticks in the game. */
        out[PAD_LS] = 0;
        out[PAD_RS] = 0;
    }
    sticks_were_down = sticks;

    /* Start+Select carries HOME and quit. While both are held neither is
     * forwarded, so reaching for either does not also pause the remote
     * game; one alone still goes through.
     *
     * HOME fires on RELEASE, so holding on toward quit does not send one
     * on the way past. Quit fires while held -- there is nothing longer
     * to hold for, and waiting for the release would only feel
     * unresponsive. */
    int combo = out[PAD_START] && out[PAD_BACK];
    if (combo) {
        if (combo_since == 0) {
            combo_since = now;
        } else if (now - combo_since >= QUIT_HOLD_MS) {
            g_quit_pending = 1;
            combo_since = 0; /* one quit per hold */
        }
        out[PAD_START] = 0;
        out[PAD_BACK] = 0;
    } else {
        if (combo_since != 0 && now - combo_since >= HOME_HOLD_MS) {
            g_home_pending = 1;
        }
        combo_since = 0;
    }
}

static int poll_thread(void *unused) {
    (void)unused;
    /* Above the frame loop: a few hundred microseconds every 4 ms, and
     * being late with it is the whole problem this thread exists to
     * solve. Set from inside the thread -- it applies to the caller. */
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    PadState21 state, last;
    memset(last, 0, sizeof(last));
    Uint32 last_sent = 0;

    while (g_running) {
        Uint32 started = SDL_GetTicks();

        sample(state);
        /* Anything else that drives the pad -- the on-screen one -- is
         * read and folded in here rather than in the frame loop, so
         * touch input runs at this thread's rate instead of the
         * drawing's. */
        if (g_poll_hook) {
            g_poll_hook();
        }
        if (g_merge_hook) {
            g_merge_hook(state);
        }

        SDL_LockMutex(g_lock);
        gestures(state);
        for (int i = 0; i < PAD_SLOT_COUNT; i++) {
            /* A press is a slot going from nothing to something. Sticks
             * pass through here too and it is meaningless for them; the
             * caller only asks about buttons. */
            if (state[i] && !g_snapshot[i] && g_presses[i] < 250) {
                g_presses[i]++;
            }
        }
        memcpy(g_snapshot, state, sizeof(g_snapshot));
        SDL_UnlockMutex(g_lock);

        if (g_forward) {
            int changed = memcmp(state, last, sizeof(state)) != 0;
            if (changed || started - last_sent >= KEEPALIVE_MS) {
                net_send_input(state);
                memcpy(last, state, sizeof(last));
                last_sent = started;
            }
        } else {
            memset(last, 0, sizeof(last));
        }

        Uint32 spent = SDL_GetTicks() - started;
        if (spent < POLL_INTERVAL_MS) {
            SDL_Delay(POLL_INTERVAL_MS - spent);
        }
    }
    return 0;
}

int input_init(void) {
    /* Every player number, not just the first, and every style.
     *
     * This was one player and the default pad, which covers the handheld
     * console and whatever happens to be player 1. Detach the Joy-Cons
     * and they come back over Bluetooth as a controller that may not be
     * player 1 at all -- if something else already held that slot, the
     * reconnected pair lands on player 2 and nothing here ever saw it
     * again. That is the "the buttons worked for a while and then did
     * nothing" case, and it needs no unplugging to happen: the console
     * reassigns players on its own when controllers come and go.
     *
     * Reading all of them and folding them together also means it does
     * not matter which one is picked up. */
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&g_hid);
    memset(g_snapshot, 0, sizeof(g_snapshot));

    g_lock = SDL_CreateMutex();
    if (!g_lock) {
        printf("SDL_CreateMutex: %s\n", SDL_GetError());
        return -1;
    }
    g_running = 1;
    g_thread = SDL_CreateThread(poll_thread, "input", NULL);
    if (!g_thread) {
        printf("SDL_CreateThread(input): %s\n", SDL_GetError());
        g_running = 0;
        return -1;
    }
    return 0;
}

void input_exit(void) {
    g_running = 0;
    if (g_thread) {
        SDL_WaitThread(g_thread, NULL);
        g_thread = NULL;
    }
    if (g_lock) {
        SDL_DestroyMutex(g_lock);
        g_lock = NULL;
    }
}

void input_set_merge_hook(void (*hook)(PadState21)) {
    g_merge_hook = hook;
}

void input_set_poll_hook(void (*hook)(void)) {
    g_poll_hook = hook;
}

void input_set_forwarding(int on) {
    g_forward = on ? 1 : 0;
}

int input_take_press(int slot) {
    if (slot < 0 || slot >= PAD_SLOT_COUNT) {
        return 0;
    }
    SDL_LockMutex(g_lock);
    const int n = g_presses[slot];
    g_presses[slot] = 0;
    SDL_UnlockMutex(g_lock);
    return n;
}

void input_clear_presses(void) {
    SDL_LockMutex(g_lock);
    memset(g_presses, 0, sizeof(g_presses));
    SDL_UnlockMutex(g_lock);
}

/* What is attached right now, for the diagnostics: "the controller does
 * nothing" and "the console is not giving this program a controller" are
 * indistinguishable from the outside. */
void input_describe_pads(char *out, size_t out_size) {
    /* What the console is actually giving this program.
     *
     * "The controller does nothing" has three quite different causes --
     * no controller assigned to us, a controller whose input we read but
     * do not forward, and a host that refuses it -- and from the outside
     * they look identical. This names the first one. */
    const u32 style = padGetStyleSet(&g_hid);
    if (!padIsConnected(&g_hid)) {
        snprintf(out, out_size, "none");
        return;
    }
    snprintf(out, out_size, "%s%s%s%s%s",
             (style & HidNpadStyleTag_NpadHandheld) ? "handheld " : "",
             (style & HidNpadStyleTag_NpadFullKey) ? "pro " : "",
             (style & HidNpadStyleTag_NpadJoyDual) ? "joy-dual " : "",
             (style & HidNpadStyleTag_NpadJoyLeft) ? "joy-left " : "",
             (style & HidNpadStyleTag_NpadJoyRight) ? "joy-right " : "");
}

void input_read(PadState21 out) {
    SDL_LockMutex(g_lock);
    memcpy(out, g_snapshot, sizeof(PadState21));
    SDL_UnlockMutex(g_lock);
}

static int take(int *flag) {
    int v;
    SDL_LockMutex(g_lock);
    v = *flag;
    *flag = 0;
    SDL_UnlockMutex(g_lock);
    return v;
}

int input_take_home_gesture(void) { return take(&g_home_pending); }
int input_take_quit_gesture(void) { return take(&g_quit_pending); }

int input_take_menu_gesture(void) {
    if (take(&g_menu_pending)) return 1;
    return take(&g_touch_menu_pending);
}

void input_feed_touch(float normalised_y, int began) {
    if (began && normalised_y <= MENU_TOUCH_ZONE) {
        SDL_LockMutex(g_lock);
        g_touch_menu_pending = 1;
        SDL_UnlockMutex(g_lock);
    }
}
