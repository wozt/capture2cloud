#ifndef OUTPUT_PCBLE_H
#define OUTPUT_PCBLE_H

#include "output_backend.h"

/*
 * Capture2Cloud -> vendored pcble Classic HID backend.
 *
 * This side stays unprivileged.  The privileged helper owns BlueZ and
 * exposes a private Unix socket.  The bridge only sends the final merged
 * controller state to that socket.
 */
const GamepadOutputBackend *output_pcble_backend(void);

#endif
