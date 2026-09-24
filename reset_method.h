#ifndef CAPTURE2CLOUD_RESET_METHOD_H
#define CAPTURE2CLOUD_RESET_METHOD_H

#include <stddef.h>

/*
 * Console wake/reset mechanism.
 *
 * Remote clients only ask Capture2Cloud to wake the target console.
 * They deliberately do not know whether that means running an external
 * script, transmitting a Bluetooth wake advertisement, or another
 * mechanism added later.
 *
 * The implementation is independent from the controller-output backend.
 * A later Bluetooth implementation may temporarily release an output
 * backend when both features share the same physical adapter, then ask
 * the existing gamepad recovery path to reconnect it.
 */

typedef enum {
    RESET_METHOD_SCRIPT = 0,
    RESET_METHOD_BLUETOOTH = 1,
} ResetMethod;

/* Current persistent selection. Missing/invalid config falls back to script. */
ResetMethod reset_method_current(void);

/* Stable config/UI name for a method, or NULL for an invalid value. */
const char *reset_method_name(ResetMethod method);

/* Saves the selected method to scripts/.env. */
int reset_method_set(ResetMethod method);

#define RESET_METHOD_MAX_BT_ADAPTERS 8

typedef struct {
    char id[16];
    char address[18];
} ResetBluetoothAdapter;

/* Script method configuration. */
void reset_method_get_script(char *out, size_t out_size);
int reset_method_set_script(const char *path);

/* Bluetooth wake configuration. */
void reset_method_get_bluetooth_target(char *out, size_t out_size);
int reset_method_set_bluetooth_target(const char *target);

void reset_method_get_bluetooth_adapter(char *out, size_t out_size);
int reset_method_set_bluetooth_adapter(const char *address);

/*
 * Enumerates BlueZ controllers. The stable address is what gets saved;
 * hciN is only the controller's current runtime name.
 */
int reset_method_scan_bluetooth_adapters(
    ResetBluetoothAdapter adapters[RESET_METHOD_MAX_BT_ADAPTERS],
    char *error,
    size_t error_size);


/*
 * Runs the privileged LE compatibility probe on the selected runtime
 * hciN adapter. The adapter MAC remains the persistent identity; hciN
 * is used only for this immediate operation.
 */
int reset_method_test_bluetooth_adapter(
    const char *adapter_id,
    char *message,
    size_t message_size);

/*
 * Starts the configured console wake operation.
 *
 * The operation is asynchronous. Returns 0 once the work was accepted,
 * -1 if it could not be started.
 */
int reset_method_wake(void);

#endif
