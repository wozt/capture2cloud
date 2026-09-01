#ifndef GAMEPAD_BRIDGE_H
#define GAMEPAD_BRIDGE_H

#include <stdint.h>

/*
 * Bridge to a ConsoleTuner adapter (Titan One / Titan Two / Cronus / GPP)
 * connected over USB, to emulate an Xbox 360 controller on a real game
 * console from gamepad states received from the browser (client-side
 * Gamepad API, sent over a WebRTC DataChannel).
 *
 * Protocol ("GCAPI"): these devices expose a generic HID interface that,
 * once switched to "capture mode", accepts an array of -100..100 values
 * (one per Xbox 360 button/axis) and forwards it to the console as if a
 * real controller were plugged in. This protocol is not officially
 * documented by ConsoleTuner; this module relies on the understanding
 * given by the source code of the free/open-source GIMX project
 * (https://github.com/matlo/GIMX, GPLv3, shared/gimxgpp/pcprog.c) without
 * reusing its code -- only the protocol knowledge (USB identifiers, HID
 * report format, command values) was drawn from it.
 */

/* Index of each entry in the state array, in the order expected by the
 * GCAPI protocol for an Xbox 360 controller (same values as GIMX's
 * XB360_* #defines, for compatibility). */
enum {
    GAMEPAD_XB360_GUIDE = 0, /* Xbox / Guide button */
    GAMEPAD_XB360_BACK,
    GAMEPAD_XB360_START,
    GAMEPAD_XB360_RB,
    GAMEPAD_XB360_RT, /* right trigger, analog 0..100 */
    GAMEPAD_XB360_RS, /* right stick click */
    GAMEPAD_XB360_LB,
    GAMEPAD_XB360_LT, /* left trigger, analog 0..100 */
    GAMEPAD_XB360_LS, /* left stick click */
    GAMEPAD_XB360_RX, /* right stick horizontal, -100..100 */
    GAMEPAD_XB360_RY, /* right stick vertical, -100..100 (up = positive) */
    GAMEPAD_XB360_LX, /* left stick horizontal, -100..100 */
    GAMEPAD_XB360_LY, /* left stick vertical, -100..100 (up = positive) */
    GAMEPAD_XB360_UP,
    GAMEPAD_XB360_DOWN,
    GAMEPAD_XB360_LEFT,
    GAMEPAD_XB360_RIGHT,
    GAMEPAD_XB360_Y,
    GAMEPAD_XB360_B,
    GAMEPAD_XB360_A,
    GAMEPAD_XB360_X,
    GAMEPAD_BRIDGE_STATE_COUNT
};

/* Connects to the first ConsoleTuner adapter found on USB (Titan One,
 * Titan Two, Cronus, GPP...) and switches its PC port to capture mode.
 * Returns 0 if no adapter is found/accessible (the rest of the app keeps
 * running without gamepad support -- this is not a fatal error), or 1 if
 * connected. */
int gamepad_bridge_init(void);

/* Identifies who is sending. Two people can be connected at once -- a
 * browser and the Switch client, or two browsers -- and each is tracked
 * separately, so an idle one is not mistaken for the active one letting
 * go. Any stable number works; these only keep the two transports from
 * colliding. */
#define GAMEPAD_SOURCE_BROWSER(i) (0x100u + (unsigned)(i))
#define GAMEPAD_SOURCE_NATIVE(i)  (0x200u + (unsigned)(i))
/* A controller plugged into this machine. One, because there is one
 * person at this keyboard. */
#define GAMEPAD_SOURCE_LOCAL      0x300u

/* Sends a new gamepad state for one source (array of
 * GAMEPAD_BRIDGE_STATE_COUNT values, indexed by the GAMEPAD_XB360_*
 * enum, each in [-100, 100]). What reaches the console is every source
 * combined. Does nothing if gamepad_bridge_init() did not succeed. */
void gamepad_bridge_update(unsigned source, const int8_t state[GAMEPAD_BRIDGE_STATE_COUNT]);

/* Drops a source from the combination. Called when a client goes away:
 * whatever it was holding down must not stay held. */
void gamepad_bridge_forget(unsigned source);

/* Switches the adapter back to normal passthrough mode (a real controller
 * plugged into it works again) and releases the USB device. */
/* Re-enumerates the adapter so it re-handshakes with the console. Takes
 * effect shortly after returning, on the bridge's own thread. Safe to
 * call when nothing is connected -- it does nothing. */
void gamepad_bridge_reset(void);

/* Presses and releases GUIDE (the console's HOME) on the remote console.
 * Held briefly so the console registers it as a press rather than a
 * glitch. Applied by the send thread, which is the only thing allowed to
 * touch the outgoing state. */
void gamepad_bridge_press_home(void);

/*
 * Which controller the adapter pretends to be to the console.
 *
 * The adapter can guess this on its own, and gets it wrong: plugged
 * straight into a Switch dock it guessed Xbox 360, enumerated happily
 * and had every button ignored. So it is settable, and the value is
 * read back from the device rather than remembered.
 *
 * The numbering is GTuner Pro's own "Output Protocol" list, confirmed
 * value by value against a USB capture: 0 auto, 1 ps3, 2 xb360, 3 ps4,
 * 4 xb1, 5 ps4rp, 6 switch, 7 ps5, 8 xbsx.
 */
const char *gamepad_protocol_name(int value);   /* "switch", "" if unknown */
const char *gamepad_protocol_label(int value);  /* "Nintendo Switch" */
int gamepad_protocol_from_name(const char *name); /* -1 if not a known name */
int gamepad_protocol_count(void);

/* What the adapter is set to right now, or -1 before it has been asked. */
int gamepad_bridge_output_protocol(void);

/* The console the adapter reports on its output port, or -1 if unknown.
 * A different enumeration from the one above -- this is the older GCAPI
 * one (0 nothing plugged in, 1 ps3, 2 xb360, 3 ps4, 4 xb1, 5 switch) --
 * and it says what the adapter FOUND, not what it is emulating. */
int gamepad_bridge_console(void);

/* Whether the USB link to the adapter is up right now. Distinct from
 * "the bridge is running": the adapter can be unplugged and plugged back
 * in without the rest of the program noticing, and watching this go down
 * and up again is how the settings window knows a replug really
 * happened. */
int gamepad_bridge_link_up(void);

/* Asks for a different output protocol. Applied by the send thread,
 * which owns the USB handle, and only if it differs from what the device
 * already holds: this writes to the adapter's non-volatile memory. */
void gamepad_bridge_request_output_protocol(int value);

/* Reports per second actually sent to the adapter, averaged over the
 * last few seconds. 0 when no adapter is connected.
 *
 * Idle sits at the keepalive rate (10/s) because unchanged state is not
 * resent; anything above that is someone actually pressing something. */
double gamepad_bridge_report_rate(void);

void gamepad_bridge_shutdown(void);

#endif
