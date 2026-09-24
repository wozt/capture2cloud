#ifndef CAPTURE2CLOUD_RESET_METHOD_H
#define CAPTURE2CLOUD_RESET_METHOD_H

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

/*
 * Starts the configured console wake operation.
 *
 * The operation is asynchronous. Returns 0 once the work was accepted,
 * -1 if it could not be started.
 */
int reset_method_wake(void);

#endif
