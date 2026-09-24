#ifndef GAMEPAD_BRIDGE_H
#define GAMEPAD_BRIDGE_H

#include "controller_state.h"
#include "controller_shaping.h"

/*
 * Stable input-source IDs.
 *
 * Multiple clients may drive the console simultaneously.  The bridge
 * keeps their state separate and combines it before handing one final
 * ControllerState to the selected output backend.
 */
#define GAMEPAD_SOURCE_BROWSER(i) (0x100u + (unsigned)(i))
#define GAMEPAD_SOURCE_NATIVE(i)  (0x200u + (unsigned)(i))
#define GAMEPAD_SOURCE_LOCAL      0x300u

int gamepad_bridge_init(void);

void gamepad_bridge_update(
    unsigned source,
    const int8_t state[GAMEPAD_BRIDGE_STATE_COUNT]);

void gamepad_bridge_forget(unsigned source);

void gamepad_bridge_reset(void);
void gamepad_bridge_press_home(void);

int gamepad_bridge_link_up(void);
double gamepad_bridge_report_rate(void);

/*
 * Applied after all input sources have been merged and immediately before
 * the selected console-output backend. Updating it also re-sends the
 * currently held state, so calibration changes are visible live.
 */
void gamepad_bridge_set_output_shaping(
    const ControllerShaping *shaping);

const char *gamepad_bridge_backend_name(void);

/* Output-backend registry. Planned backends stay visible to the UI
 * but report unavailable until an implementation is registered. */
int gamepad_bridge_backend_count(void);
const char *gamepad_bridge_backend_name_at(int index);
const char *gamepad_bridge_backend_label(int index);
const char *gamepad_bridge_backend_maintenance_label(int index);
const char *gamepad_bridge_backend_maintenance_help(int index);
int gamepad_bridge_backend_available(int index);
int gamepad_bridge_backend_from_name(const char *name);
int gamepad_bridge_backend_configured_index(void);

void gamepad_bridge_shutdown(void);

/*
 * Legacy Titan-specific controls.
 *
 * They remain here so this refactor does not change the GTK/web/native
 * control surfaces.  They are effective only when the Titan backend is
 * active.  They can move into backend capabilities when another output
 * backend is wired into the settings UI.
 */
const char *gamepad_protocol_name(int value);
const char *gamepad_protocol_label(int value);
int gamepad_protocol_from_name(const char *name);
int gamepad_protocol_count(void);

int gamepad_bridge_output_protocol(void);
int gamepad_bridge_console(void);
void gamepad_bridge_request_output_protocol(int value);

#endif
