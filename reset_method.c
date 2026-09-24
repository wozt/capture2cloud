#define _GNU_SOURCE

#include "reset_method.h"

#include "app_config.h"
#include "video_capture.h"

#include <SDL2/SDL.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESET_BT_RUNNER "/usr/local/libexec/capture2cloud/pcble/run-classic.sh"

#define RESET_BT_BEACON_VERSION 1
#define RESET_BT_BEACON_MAX_ADV 31

static const uint8_t RESET_BT_BEACON_MAGIC[8] = {
    'C', '2', 'C', 'S', '2', 'W', 'A', 'K'
};


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
     * Passive BlueZ discovery.
     *
     * Do not spawn btmgmt or bluetoothctl from the Capture2Cloud
     * process: child processes can inherit live video/audio descriptors
     * and keep hardware busy if the parent exits unexpectedly.
     *
     * This is the same BlueZ ObjectManager path already used by pcble.
     */
    GError *gerror = NULL;

    GDBusConnection *bus =
        g_bus_get_sync(
            G_BUS_TYPE_SYSTEM,
            NULL,
            &gerror);

    if (!bus) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "%s",
                gerror
                    ? gerror->message
                    : "Could not open system D-Bus");
        }

        g_clear_error(&gerror);
        return -1;
    }

    GVariant *reply =
        g_dbus_connection_call_sync(
            bus,
            "org.bluez",
            "/",
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects",
            NULL,
            G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
            G_DBUS_CALL_FLAGS_NONE,
            1500,
            NULL,
            &gerror);

    g_object_unref(bus);

    if (!reply) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "%s",
                gerror
                    ? gerror->message
                    : "BlueZ did not answer");
        }

        g_clear_error(&gerror);
        return -1;
    }

    GVariantIter *objects = NULL;

    g_variant_get(
        reply,
        "(a{oa{sa{sv}}})",
        &objects);

    int count = 0;
    char *path = NULL;
    GVariant *interfaces = NULL;

    while (g_variant_iter_next(
            objects,
            "{o@a{sa{sv}}}",
            &path,
            &interfaces)) {

        GVariant *props =
            g_variant_lookup_value(
                interfaces,
                "org.bluez.Adapter1",
                G_VARIANT_TYPE_VARDICT);

        if (props &&
            count < RESET_METHOD_MAX_BT_ADAPTERS) {

            const char *address = "";

            g_variant_lookup(
                props,
                "Address",
                "&s",
                &address);

            char *id =
                g_path_get_basename(path);

            if (id &&
                strncmp(id, "hci", 3) == 0 &&
                strlen(address) == 17) {

                snprintf(
                    adapters[count].id,
                    sizeof(adapters[count].id),
                    "%s",
                    id);

                snprintf(
                    adapters[count].address,
                    sizeof(adapters[count].address),
                    "%s",
                    address);

                count++;
            }

            g_free(id);
        }

        if (props) {
            g_variant_unref(props);
        }

        g_free(path);
        g_variant_unref(interfaces);

        path = NULL;
        interfaces = NULL;
    }

    g_variant_iter_free(objects);
    g_variant_unref(reply);

    return count;
}


static int reset_bt_hci_id_valid(const char *id)
{
    if (!id ||
        strncmp(id, "hci", 3) != 0 ||
        !isdigit((unsigned char)id[3])) {
        return 0;
    }

    for (const char *p = id + 3; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }

    return 1;
}

int reset_method_test_bluetooth_adapter(
    const char *adapter_id,
    char *message,
    size_t message_size)
{
    if (message && message_size) {
        message[0] = '\0';
    }

    if (!reset_bt_hci_id_valid(adapter_id)) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Invalid Bluetooth adapter id");
        }
        return 0;
    }

    if (access(RESET_BT_RUNNER, X_OK) != 0) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Bluetooth helper is not installed. Run scripts/install_pcble_backend.sh");
        }
        return 0;
    }

    const char *argv[] = {
        "pkexec",
        RESET_BT_RUNNER,
        adapter_id,
        "--wake-probe",
        NULL
    };

    GError *error = NULL;

    GSubprocess *process =
        g_subprocess_newv(
            argv,
            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_MERGE,
            &error);

    if (!process) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "%s",
                error
                    ? error->message
                    : "Could not start Bluetooth compatibility probe");
        }

        g_clear_error(&error);
        return 0;
    }

    gchar *output = NULL;

    gboolean communicated =
        g_subprocess_communicate_utf8(
            process,
            NULL,
            NULL,
            &output,
            NULL,
            &error);

    int ok =
        communicated &&
        g_subprocess_get_successful(process);

    if (ok) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Compatible — LE scan, random-address and advertising commands accepted.");
        }
    } else if (message && message_size) {
        if (output && *output) {
            g_strstrip(output);

            snprintf(
                message,
                message_size,
                "%s",
                output);
        } else {
            snprintf(
                message,
                message_size,
                "%s",
                error
                    ? error->message
                    : "Bluetooth compatibility probe failed");
        }
    }

    g_free(output);
    g_clear_error(&error);
    g_object_unref(process);

    return ok;
}

static int reset_bt_hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return 10 + c - 'a';
    }

    if (c >= 'A' && c <= 'F') {
        return 10 + c - 'A';
    }

    return -1;
}

static int reset_bt_decode_hex(
    const char *hex,
    uint8_t *out,
    size_t out_size,
    size_t *decoded_size)
{
    if (decoded_size) {
        *decoded_size = 0;
    }

    if (!hex || !out) {
        return 0;
    }

    size_t len = strlen(hex);

    if ((len & 1u) != 0 ||
        len / 2 > out_size) {
        return 0;
    }

    for (size_t i = 0; i < len / 2; i++) {
        int high =
            reset_bt_hex_value(
                hex[i * 2]);

        int low =
            reset_bt_hex_value(
                hex[i * 2 + 1]);

        if (high < 0 || low < 0) {
            return 0;
        }

        out[i] =
            (uint8_t)((high << 4) | low);
    }

    if (decoded_size) {
        *decoded_size = len / 2;
    }

    return 1;
}

static int reset_bt_parse_mac(
    const char *text,
    uint8_t out[6])
{
    unsigned values[6];

    if (!text ||
        sscanf(
            text,
            "%2x:%2x:%2x:%2x:%2x:%2x",
            &values[0],
            &values[1],
            &values[2],
            &values[3],
            &values[4],
            &values[5]) != 6) {
        return 0;
    }

    for (int i = 0; i < 6; i++) {
        out[i] =
            (uint8_t)values[i];
    }

    return 1;
}

static int reset_bt_capture_field(
    const char *line,
    const char *field,
    char *out,
    size_t out_size)
{
    if (!line ||
        !field ||
        !out ||
        out_size == 0) {
        return 0;
    }

    out[0] = '\0';

    const char *p =
        strstr(line, field);

    if (!p) {
        return 0;
    }

    p += strlen(field);

    const char *end = p;

    while (*end &&
           *end != ' ' &&
           *end != '\r' &&
           *end != '\n') {
        end++;
    }

    size_t len =
        (size_t)(end - p);

    if (len == 0 ||
        len >= out_size) {
        return 0;
    }

    memcpy(
        out,
        p,
        len);

    out[len] = '\0';
    return 1;
}

static int reset_bt_save_beacon(
    const char *controller_text,
    const char *switch_text,
    const uint8_t *advertisement,
    size_t advertisement_size,
    char *message,
    size_t message_size)
{
    if (!controller_text ||
        !switch_text ||
        !advertisement ||
        advertisement_size == 0 ||
        advertisement_size > RESET_BT_BEACON_MAX_ADV) {
        return 0;
    }

    uint8_t controller[6];
    uint8_t target_switch[6];

    if (!reset_bt_parse_mac(
            controller_text,
            controller) ||
        !reset_bt_parse_mac(
            switch_text,
            target_switch)) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Captured beacon contains an invalid Bluetooth address.");
        }
        return 0;
    }

    const char *data_home =
        g_get_user_data_dir();

    char *directory =
        g_build_filename(
            data_home,
            "capture2cloud",
            "reset",
            NULL);

    if (!directory) {
        return 0;
    }

    if (g_mkdir_with_parents(
            directory,
            0700) != 0) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Could not create %s: %s",
                directory,
                g_strerror(errno));
        }

        g_free(directory);
        return 0;
    }

    g_chmod(
        directory,
        0700);

    char *path =
        g_build_filename(
            directory,
            "switch2-beacon.bin",
            NULL);

    char *temporary =
        g_strdup_printf(
            "%s.tmp.%ld",
            path,
            (long)getpid());

    if (!path || !temporary) {
        g_free(temporary);
        g_free(path);
        g_free(directory);
        return 0;
    }

    FILE *f =
        fopen(
            temporary,
            "wb");

    if (!f) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Could not create beacon file: %s",
                g_strerror(errno));
        }

        g_free(temporary);
        g_free(path);
        g_free(directory);
        return 0;
    }

    uint8_t version =
        RESET_BT_BEACON_VERSION;

    uint8_t adv_size =
        (uint8_t)advertisement_size;

    uint8_t padded_adv[
        RESET_BT_BEACON_MAX_ADV];

    memset(
        padded_adv,
        0,
        sizeof(padded_adv));

    memcpy(
        padded_adv,
        advertisement,
        advertisement_size);

    int ok =
        fwrite(
            RESET_BT_BEACON_MAGIC,
            1,
            sizeof(RESET_BT_BEACON_MAGIC),
            f) ==
            sizeof(RESET_BT_BEACON_MAGIC) &&

        fwrite(
            &version,
            1,
            1,
            f) == 1 &&

        fwrite(
            &adv_size,
            1,
            1,
            f) == 1 &&

        fwrite(
            controller,
            1,
            sizeof(controller),
            f) ==
            sizeof(controller) &&

        fwrite(
            target_switch,
            1,
            sizeof(target_switch),
            f) ==
            sizeof(target_switch) &&

        fwrite(
            padded_adv,
            1,
            sizeof(padded_adv),
            f) ==
            sizeof(padded_adv);

    if (ok &&
        fflush(f) == 0) {
        int fd =
            fileno(f);

        if (fd >= 0 &&
            fsync(fd) != 0) {
            ok = 0;
        }
    }

    if (fclose(f) != 0) {
        ok = 0;
    }

    if (ok &&
        g_chmod(
            temporary,
            0600) != 0) {
        ok = 0;
    }

    if (ok &&
        g_rename(
            temporary,
            path) != 0) {
        ok = 0;
    }

    if (!ok) {
        g_remove(temporary);

        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Could not save captured beacon: %s",
                g_strerror(errno));
        }
    } else if (message && message_size) {
        snprintf(
            message,
            message_size,
            "Captured Switch 2 beacon for %s — saved to %s",
            switch_text,
            path);
    }

    g_free(temporary);
    g_free(path);
    g_free(directory);

    return ok;
}

int reset_method_capture_bluetooth_beacon(
    const char *adapter_id,
    char *message,
    size_t message_size)
{
    if (message && message_size) {
        message[0] = '\0';
    }

    if (!reset_bt_hci_id_valid(
            adapter_id)) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Invalid Bluetooth adapter id");
        }
        return 0;
    }

    if (access(
            RESET_BT_RUNNER,
            X_OK) != 0) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Bluetooth helper is not installed. Run scripts/install_pcble_backend.sh");
        }
        return 0;
    }

    const char *argv[] = {
        "pkexec",
        RESET_BT_RUNNER,
        adapter_id,
        "--wake-capture",
        NULL
    };

    GError *error = NULL;

    GSubprocess *process =
        g_subprocess_newv(
            argv,
            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_MERGE,
            &error);

    if (!process) {
        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "%s",
                error
                    ? error->message
                    : "Could not start Bluetooth wake capture");
        }

        g_clear_error(&error);
        return 0;
    }

    gchar *output = NULL;

    gboolean communicated =
        g_subprocess_communicate_utf8(
            process,
            NULL,
            NULL,
            &output,
            NULL,
            &error);

    int successful =
        communicated &&
        g_subprocess_get_successful(
            process);

    if (!successful) {
        if (message && message_size) {
            if (output && *output) {
                g_strstrip(output);

                snprintf(
                    message,
                    message_size,
                    "%s",
                    output);
            } else {
                snprintf(
                    message,
                    message_size,
                    "%s",
                    error
                        ? error->message
                        : "Wake beacon capture failed");
            }
        }

        g_free(output);
        g_clear_error(&error);
        g_object_unref(process);
        return 0;
    }

    const char *marker =
        output
            ? strstr(
                output,
                "WAKE_CAPTURE_OK ")
            : NULL;

    char controller[32];
    char target_switch[32];
    char data_hex[
        RESET_BT_BEACON_MAX_ADV * 2 + 1];

    if (!marker ||
        !reset_bt_capture_field(
            marker,
            "controller=",
            controller,
            sizeof(controller)) ||
        !reset_bt_capture_field(
            marker,
            "switch=",
            target_switch,
            sizeof(target_switch)) ||
        !reset_bt_capture_field(
            marker,
            "data=",
            data_hex,
            sizeof(data_hex))) {

        if (message && message_size) {
            snprintf(
                message,
                message_size,
                "Bluetooth helper returned an invalid wake-beacon result.");
        }

        g_free(output);
        g_clear_error(&error);
        g_object_unref(process);
        return 0;
    }

    uint8_t advertisement[
        RESET_BT_BEACON_MAX_ADV];

    size_t advertisement_size = 0;

    int decoded =
        reset_bt_decode_hex(
            data_hex,
            advertisement,
            sizeof(advertisement),
            &advertisement_size);

    int ok =
        decoded &&
        reset_bt_save_beacon(
            controller,
            target_switch,
            advertisement,
            advertisement_size,
            message,
            message_size);

    if (!decoded &&
        message &&
        message_size) {
        snprintf(
            message,
            message_size,
            "Bluetooth helper returned invalid advertisement data.");
    }

    g_free(output);
    g_clear_error(&error);
    g_object_unref(process);

    return ok;
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
