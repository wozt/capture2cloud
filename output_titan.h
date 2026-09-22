#ifndef OUTPUT_TITAN_H
#define OUTPUT_TITAN_H

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

#include "output_backend.h"

/* Connects to the first ConsoleTuner adapter found on USB (Titan One,
 * Titan Two, Cronus, GPP...) and switches its PC port to capture mode.
 * Returns 0 if no adapter is found/accessible (the rest of the app keeps
 * running without gamepad support -- this is not a fatal error), or 1 if
 * connected. */
int output_titan_init(void);

/* Sends a new gamepad state for one source (array of
 * GAMEPAD_BRIDGE_STATE_COUNT values, indexed by the GAMEPAD_XB360_*
 * enum, each in [-100, 100]). What reaches the console is every source
 * combined. Does nothing if output_titan_init() did not succeed. */
void output_titan_update(unsigned source, const int8_t state[GAMEPAD_BRIDGE_STATE_COUNT]);

/* Drops a source from the combination. Called when a client goes away:
 * whatever it was holding down must not stay held. */
void output_titan_forget(unsigned source);

/* Switches the adapter back to normal passthrough mode (a real controller
 * plugged into it works again) and releases the USB device. */
/* Re-enumerates the adapter so it re-handshakes with the console. Takes
 * effect shortly after returning, on the bridge's own thread. Safe to
 * call when nothing is connected -- it does nothing. */
void output_titan_reset(void);

/* Presses and releases GUIDE (the console's HOME) on the remote console.
 * Held briefly so the console registers it as a press rather than a
 * glitch. Applied by the send thread, which is the only thing allowed to
 * touch the outgoing state. */
void output_titan_press_home(void);

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
const char *output_titan_protocol_name(int value);   /* "switch", "" if unknown */
const char *output_titan_protocol_label(int value);  /* "Nintendo Switch" */
int output_titan_protocol_from_name(const char *name); /* -1 if not a known name */
int output_titan_protocol_count(void);

/* What the adapter is set to right now, or -1 before it has been asked. */
int output_titan_output_protocol(void);

/* The console the adapter reports on its output port, or -1 if unknown.
 * A different enumeration from the one above -- this is the older GCAPI
 * one (0 nothing plugged in, 1 ps3, 2 xb360, 3 ps4, 4 xb1, 5 switch) --
 * and it says what the adapter FOUND, not what it is emulating. */
int output_titan_console(void);

/* Whether the USB link to the adapter is up right now. Distinct from
 * "the bridge is running": the adapter can be unplugged and plugged back
 * in without the rest of the program noticing, and watching this go down
 * and up again is how the settings window knows a replug really
 * happened. */
int output_titan_link_up(void);

/* Asks for a different output protocol. Applied by the send thread,
 * which owns the USB handle, and only if it differs from what the device
 * already holds: this writes to the adapter's non-volatile memory. */
void output_titan_request_output_protocol(int value);

/* Reports per second actually sent to the adapter, averaged over the
 * last few seconds. 0 when no adapter is connected.
 *
 * Idle sits at the keepalive rate (10/s) because unchanged state is not
 * resent; anything above that is someone actually pressing something. */
double output_titan_report_rate(void);

void output_titan_shutdown(void);


/* Backend descriptor used by the generic gamepad bridge. */
const GamepadOutputBackend *output_titan_backend(void);

#endif
