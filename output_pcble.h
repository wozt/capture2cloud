#ifndef OUTPUT_PCBLE_H
#define OUTPUT_PCBLE_H

#include "output_backend.h"

#include <stddef.h>

#define OUTPUT_PCBLE_MAX_ADAPTERS 8

typedef struct {
    char id[16];
    char address[18];
} OutputPcbleAdapter;

/*
 * Capture2Cloud -> vendored pcble Classic HID backend.
 *
 * This side stays unprivileged.  The privileged helper owns BlueZ and
 * exposes a private Unix socket.  The bridge only sends the final merged
 * controller state to that socket.
 */
const GamepadOutputBackend *output_pcble_backend(void);

/* Bluetooth-session controls used by the backend-specific GTK panel. */
int output_pcble_scan_adapters(
    OutputPcbleAdapter adapters[OUTPUT_PCBLE_MAX_ADAPTERS],
    char *error,
    size_t error_size);

int output_pcble_set_controller(const char *profile);

int output_pcble_start_session(
    const char *primary_adapter,
    const char *secondary_adapter,
    const char *profile,
    const char *reconnect_address,
    const char *body_color,
    const char *button_color,
    const char *left_grip_color,
    const char *right_grip_color,
    int verbose,
    char *error,
    size_t error_size);

void output_pcble_stop_session(void);

/*
 * Backend recovery state. Recovery itself is owned entirely by the
 * pcble backend and Capture2Cloud main loop; GTK only displays it.
 */
int output_pcble_reconnect_pending(void);
void output_pcble_clear_reconnect_request(void);

int output_pcble_reconnect_active(void);

int output_pcble_ipc_up(void);
int output_pcble_session_running(void);
int output_pcble_session_stopping(void);
void output_pcble_peer(char *out, size_t out_size);

#endif
