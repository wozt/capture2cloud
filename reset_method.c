#define _GNU_SOURCE

#include "reset_method.h"

#include "app_config.h"
#include "video_capture.h"

#include <SDL2/SDL.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Current legacy reset method: execute scripts/wake_console.sh.
 *
 * The script currently power-cycles the console through Home Assistant.
 * Keeping execution here rather than in web_stream.c is intentional:
 * HTTP, native clients and the local GUI should all ask the same reset
 * layer to wake the console without knowing how that is implemented.
 *
 * Bluetooth wake will become a second reset method behind this API.
 */


const char *reset_method_name(ResetMethod method)
{
    switch (method) {
    case RESET_METHOD_SCRIPT:
        return "script";
    case RESET_METHOD_BLUETOOTH:
        return "bluetooth";
    default:
        return NULL;
    }
}

ResetMethod reset_method_current(void)
{
    char value[32];
    const char *name = config_get_str(
        "RESET_METHOD",
        value,
        sizeof(value),
        "script");

    if (strcmp(name, "bluetooth") == 0) {
        return RESET_METHOD_BLUETOOTH;
    }

    if (strcmp(name, "script") != 0) {
        fprintf(stderr,
                "reset_method: unknown RESET_METHOD '%s', using script\n",
                name);
    }

    return RESET_METHOD_SCRIPT;
}

int reset_method_set(ResetMethod method)
{
    const char *name = reset_method_name(method);

    if (!name) {
        return -1;
    }

    return config_set_str("RESET_METHOD", name);
}


static int safe_path_token(const char *value)
{
    if (!value || !*value) {
        return 0;
    }

    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!isalnum(*p) &&
            *p != '_' &&
            *p != '-' &&
            *p != '.' &&
            *p != '/' &&
            *p != ':') {
            return 0;
        }
    }

    return 1;
}

void reset_method_get_script(char *out, size_t out_size)
{
    if (!out || !out_size) {
        return;
    }

    config_get_str(
        "RESET_SCRIPT",
        out,
        out_size,
        "scripts/wake_console.sh");
}

int reset_method_set_script(const char *path)
{
    if (!safe_path_token(path)) {
        return -1;
    }

    return config_set_str("RESET_SCRIPT", path);
}

void reset_method_get_bluetooth_target(char *out, size_t out_size)
{
    if (!out || !out_size) {
        return;
    }

    config_get_str(
        "RESET_BT_CONSOLE",
        out,
        out_size,
        "switch2");
}

int reset_method_set_bluetooth_target(const char *target)
{
    /*
     * The selector is deliberately extensible, but Switch 2 is the only
     * wake protocol implemented/planned in the first version.
     */
    if (!target || strcmp(target, "switch2") != 0) {
        return -1;
    }

    return config_set_str("RESET_BT_CONSOLE", target);
}

void reset_method_get_bluetooth_adapter(char *out, size_t out_size)
{
    if (!out || !out_size) {
        return;
    }

    config_get_str(
        "RESET_BT_ADAPTER",
        out,
        out_size,
        "");
}

int reset_method_set_bluetooth_adapter(const char *address)
{
    if (!address || strlen(address) != 17) {
        return -1;
    }

    return config_set_str("RESET_BT_ADAPTER", address);
}

int reset_method_scan_bluetooth_adapters(
    ResetBluetoothAdapter adapters[RESET_METHOD_MAX_BT_ADAPTERS],
    char *error,
    size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }

    if (!adapters) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "Invalid adapter output buffer");
        }
        return -1;
    }

    memset(
        adapters,
        0,
        sizeof(ResetBluetoothAdapter) *
            RESET_METHOD_MAX_BT_ADAPTERS);

    /*
     * Bluetooth adapter discovery must remain passive.
     *
     * Do not spawn btmgmt/bluetoothctl here. Capture2Cloud already owns
     * live video/audio file descriptors and a child process can inherit
     * them. If Capture2Cloud then exits unexpectedly, that child can
     * keep the capture device open.
     *
     * Linux already exposes both pieces required by the UI:
     *
     *   /sys/class/bluetooth/hciN/address
     *
     * hciN is the temporary runtime name. The MAC address is the stable
     * identity persisted in RESET_BT_ADAPTER.
     */
    int count = 0;

    /*
     * HCI indexes are small in practice but may contain gaps after
     * unplug/replug cycles. Walk a generous range rather than assuming
     * hci0..hciN are contiguous.
     */
    for (int index = 0;
         index < 256 &&
         count < RESET_METHOD_MAX_BT_ADAPTERS;
         index++) {

        char path[PATH_MAX];

        snprintf(
            path,
            sizeof(path),
            "/sys/class/bluetooth/hci%d/address",
            index);

        FILE *f = fopen(path, "r");

        if (!f) {
            continue;
        }

        char address[32] = {0};

        if (fgets(
                address,
                sizeof(address),
                f)) {
            address[
                strcspn(
                    address,
                    "\r\n")] = '\0';

            if (strlen(address) == 17) {
                snprintf(
                    adapters[count].id,
                    sizeof(adapters[count].id),
                    "hci%d",
                    index);

                snprintf(
                    adapters[count].address,
                    sizeof(adapters[count].address),
                    "%s",
                    address);

                count++;
            }
        }

        fclose(f);
    }

    return count;
}

static int script_wake_thread(void *arg)
{
    char *cmd = arg;
    int rc = system(cmd);

    free(cmd);

    if (rc != 0) {
        fprintf(stderr,
                "reset_method: wake_console.sh exited with %d\n",
                rc);
    }

    /*
     * Do not reset the controller output immediately.
     *
     * The existing script method cuts and restores mains power. The
     * console takes several seconds before it is ready to accept the
     * controller again, so the capture loop waits until the picture
     * changes away from the no-signal pattern and performs recovery at
     * that point.
     *
     * The future Bluetooth method has different timing and will use its
     * own post-wake recovery sequence.
     */
    video_capture_watch_for_change();

    return 0;
}

static int script_wake(void)
{
    char configured[PATH_MAX];
    reset_method_get_script(
        configured,
        sizeof(configured));

    if (!safe_path_token(configured)) {
        fprintf(stderr,
                "reset_method: unsafe RESET_SCRIPT path rejected\n");
        return -1;
    }

    char script[PATH_MAX];

    if (configured[0] == '/') {
        snprintf(
            script,
            sizeof(script),
            "%s",
            configured);
    } else {
        app_path(
            script,
            sizeof(script),
            configured);
    }

    if (!script[0]) {
        return -1;
    }

    char *cmd = malloc(PATH_MAX + 64);
    if (!cmd) {
        return -1;
    }

    snprintf(
        cmd,
        PATH_MAX + 64,
        "/bin/bash '%s' >/dev/null 2>&1",
        script);

    SDL_Thread *thread = SDL_CreateThread(
        script_wake_thread,
        "wake-console",
        cmd);

    if (!thread) {
        free(cmd);
        return -1;
    }

    SDL_DetachThread(thread);
    return 0;
}

int reset_method_wake(void)
{
    switch (reset_method_current()) {
    case RESET_METHOD_SCRIPT:
        return script_wake();

    case RESET_METHOD_BLUETOOTH:
        /*
         * The selector exists before the Bluetooth implementation on
         * purpose: UI/configuration can be built against a stable reset
         * abstraction instead of teaching callers about each backend.
         */
        fprintf(stderr,
                "reset_method: Bluetooth wake is selected but not configured yet\n");
        return -1;

    default:
        return -1;
    }
}
