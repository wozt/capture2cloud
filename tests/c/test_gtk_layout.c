/* Does the settings window actually build?
 *
 * GTK reports a missing widget, a control packed into nothing, a combo
 * box set to a row that does not exist and a scale set outside its range
 * as WARNINGS at run time. None of it is a compile error, so a window
 * can be badly broken and the program still links and starts.
 *
 * This opens the real window, fills every line the program fills, loads
 * every control twice with different values, and fails if GLib logged a
 * single warning or criticism while it did.
 *
 * What it does NOT check is what the window LOOKS like. Two labels drawn
 * on top of each other are a perfectly legal layout and GTK says nothing
 * about them -- that has happened here twice and this test would not
 * have caught either. Only a pair of eyes on a screenshot does.
 *
 * It needs a display; run_all.sh supplies one with Xvfb and skips this
 * where Xvfb is not installed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>

/* The .c files are #included the way the other C tests do it, so the
 * window under test is the real one and not a copy. */
#include "../../app_config.c"
#include "../../reset_method.h"

/* Reset-method backend is stubbed here: this test validates GTK layout
 * and wiring, not HCI operations. */
static ResetMethod g_test_reset_method = RESET_METHOD_SCRIPT;

ResetMethod reset_method_current(void)
{
    return g_test_reset_method;
}

const char *reset_method_name(ResetMethod method)
{
    return method == RESET_METHOD_BLUETOOTH
        ? "bluetooth"
        : "script";
}

int reset_method_set(ResetMethod method)
{
    g_test_reset_method = method;
    return 0;
}

void reset_method_get_script(char *out, size_t out_size)
{
    snprintf(out, out_size, "scripts/wake_console.sh");
}

int reset_method_set_script(const char *path)
{
    return path && *path ? 0 : -1;
}

void reset_method_get_bluetooth_target(char *out, size_t out_size)
{
    snprintf(out, out_size, "switch2");
}

int reset_method_set_bluetooth_target(const char *target)
{
    return target && strcmp(target, "switch2") == 0 ? 0 : -1;
}

void reset_method_get_bluetooth_adapter(char *out, size_t out_size)
{
    snprintf(out, out_size, "E0:AD:47:40:70:D9");
}

int reset_method_set_bluetooth_adapter(const char *address)
{
    return address && *address ? 0 : -1;
}

int reset_method_scan_bluetooth_adapters(
    ResetBluetoothAdapter adapters[RESET_METHOD_MAX_BT_ADAPTERS],
    char *error,
    size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }

    snprintf(adapters[0].id, sizeof(adapters[0].id), "hci0");
    snprintf(adapters[0].address, sizeof(adapters[0].address),
             "00:1A:7D:DA:71:13");

    snprintf(adapters[1].id, sizeof(adapters[1].id), "hci1");
    snprintf(adapters[1].address, sizeof(adapters[1].address),
             "E0:AD:47:40:70:D9");

    return 2;
}

int reset_method_test_bluetooth_adapter(
    const char *adapter_id,
    char *message,
    size_t message_size)
{
    (void)adapter_id;

    if (message && message_size) {
        snprintf(
            message,
            message_size,
            "Compatible");
    }

    return 1;
}


#include "../../gtk_shell.c"

#include "test_util.h"

static int g_warnings;
static char g_first[512];

/* Every GLib message, from any thread, including the ones GTK would
 * otherwise print and nobody would read. */
static void count_warnings(const gchar *domain, GLogLevelFlags level, const gchar *message,
                           gpointer user_data) {
    (void)user_data;
    if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR)) {
        /* The icon is looked up beside the binary, and the test binary
         * is not beside the assets. Not a layout problem. */
        if (message && strstr(message, "Error loading icon from file")) {
            return;
        }
        if (g_warnings++ == 0) {
            snprintf(g_first, sizeof(g_first), "%s: %s", domain ? domain : "?",
                     message ? message : "?");
        }
    }
    fprintf(stderr, "[gtk] %s\n", message ? message : "");
}

static void on_settings(void *u, const AppSettings *s) { (void)u; (void)s; }
static void on_action(void *u, GtkShellAction a) { (void)u; (void)a; }
static void on_pair(void *u, const char *pin) { (void)u; (void)pin; }

/* What gtk_shell.c calls into the USB bridge. Linking the real one would
 * drag libusb and an adapter into a test about a window. */
int gamepad_bridge_link_up(void) { return 0; }
double gamepad_bridge_report_rate(void) { return 0.0; }
int gamepad_bridge_console(void) { return 0; }
const char *gamepad_bridge_backend_name(void) { return "titan"; }
int gamepad_bridge_backend_count(void) { return 3; }
const char *gamepad_bridge_backend_name_at(int i) {
    static const char *const N[] = {"titan", "pcble", "jocp"};
    return (i >= 0 && i < 3) ? N[i] : "";
}
const char *gamepad_bridge_backend_label(int i) {
    static const char *const L[] = {
        "Titan / ConsoleTuner USB",
        "pcble2gamepad",
        "JOCP / Pico 2 W",
    };
    return (i >= 0 && i < 3) ? L[i] : "?";
}
const char *gamepad_bridge_backend_maintenance_label(int i) {
    return i == 0 ? "re-enumerate Titan adapter" : "";
}
const char *gamepad_bridge_backend_maintenance_help(int i) {
    return i == 0 ? "test recovery help" : "";
}
int gamepad_bridge_backend_available(int i) { return i == 0 || i == 1; }
int gamepad_bridge_backend_from_name(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "titan") == 0) return 0;
    if (strcmp(name, "pcble") == 0) return 1;
    if (strcmp(name, "jocp") == 0) return 2;
    return -1;
}
int gamepad_bridge_backend_configured_index(void) { return 0; }
const char *gamepad_protocol_label(int i) {
    static const char *const L[] = {"auto", "ps3", "xb360", "wiiu", "ps4", "xb1", "switch"};
    return (i >= 0 && i < 7) ? L[i] : "?";
}
int gamepad_protocol_count(void) { return 7; }

/* pcble session controls are stubbed: this is a GTK layout test, not a
 * Bluetooth integration test. */
int output_pcble_scan_adapters(
    OutputPcbleAdapter adapters[OUTPUT_PCBLE_MAX_ADAPTERS],
    char *error,
    size_t error_size)
{
    (void)adapters;
    if (error && error_size) error[0] = '\0';
    return 0;
}

int output_pcble_set_controller(const char *profile)
{
    (void)profile;
    return 1;
}

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
    size_t error_size)
{
    (void)primary_adapter;
    (void)secondary_adapter;
    (void)profile;
    (void)reconnect_address;
    (void)body_color;
    (void)button_color;
    (void)left_grip_color;
    (void)right_grip_color;
    (void)verbose;
    if (error && error_size) error[0] = '\0';
    return 1;
}

void output_pcble_stop_session(void) {}
int output_pcble_ipc_up(void) { return 0; }
int output_pcble_session_running(void) { return 0; }

void output_pcble_peer(char *out, size_t out_size)
{
    if (out && out_size) out[0] = '\0';
}

int main(void) {
    if (!getenv("DISPLAY")) {
        printf("no display; skipped\n");
        return 0;
    }
    g_log_set_default_handler(count_warnings, NULL);

    AppSettings s = APP_SETTINGS_DEFAULTS;
    GtkShellCallbacks cb = { on_settings, on_action, on_pair, NULL };

    t_begin("the settings window builds and opens");
    GtkShell *sh = gtk_shell_start(&s, &cb);
    t_ok("the shell starts", sh != NULL);
    if (!sh) {
        return t_report();
    }

    /* Every per-client line, set before the window is shown -- which is
     * also the order the program does it in, since the host starts
     * publishing status before anybody opens the window. */
    for (int i = 0; i < GTK_SHELL_CLIENT_COUNT; i++) {
        gtk_shell_set_client_status(sh, i, "2 connected — 1280x720@60, 8000 kbps");
    }
    gtk_shell_set_wiiu_status(sh, "waiting for a pad");
    gtk_shell_set_status(
        sh,
        "running — capture online — browser server listening — native server listening");

    /* And every control, from settings, which is what load_controls
     * walks: a control that was never created shows up here. */
    gtk_shell_update(sh, &s);
    gtk_shell_debug_show_settings(sh);
    sleep(2);

    t_ok(
        "Reset method selector exists",
        g_c.reset_method != NULL);

    t_ok(
        "Reset method page has script controls",
        g_c.reset_script != NULL &&
        g_c.reset_script_panel != NULL);

    t_eq_int(
        "Reset method defaults to script in the layout test",
        gtk_combo_box_get_active(
            GTK_COMBO_BOX(g_c.reset_method)),
        0);

    gtk_combo_box_set_active(
        GTK_COMBO_BOX(g_c.reset_method),
        1);

    sleep(1);

    t_eq_int(
        "Selecting Bluetooth persists through reset_method_set",
        g_test_reset_method,
        RESET_METHOD_BLUETOOTH);

    t_ok(
        "Bluetooth reset panel becomes visible",
        g_c.reset_bt_panel &&
        gtk_widget_get_visible(g_c.reset_bt_panel));

    t_ok(
        "Bluetooth target selector exists",
        g_c.reset_bt_target != NULL);

    t_ok(
        "Bluetooth adapter selector exists",
        g_c.reset_bt_adapter != NULL);

    t_ok(
        "Test dongle button exists",
        g_c.reset_bt_test_adapter != NULL);

    t_ok(
        "Capture wake beacon button exists",
        g_c.reset_bt_capture != NULL);

    t_ok(
        "Test beacon button exists",
        g_c.reset_bt_test_beacon != NULL);

    t_ok(
        "overview receives the live server status",
        g_c.overview_status &&
        strcmp(
            gtk_label_get_text(GTK_LABEL(g_c.overview_status)),
            "starting...") != 0);

    /*
     * Backend-specific controls must really appear when pcble is selected.
     * Merely constructing them is not enough: a previous regression left
     * the whole panel hidden while the backend itself worked correctly.
     */
    s.output_backend = 1;
    gtk_shell_update(sh, &s);
    sleep(1);

    t_ok(
        "pcble panel is visible when pcble2gamepad is selected",
        g_c.pcble_settings &&
        gtk_widget_get_visible(g_c.pcble_settings));

    t_ok(
        "pcble Pair / Sync button is visible",
        g_c.pcble_pair &&
        gtk_widget_get_visible(g_c.pcble_pair));

    t_ok(
        "Titan panel is hidden while pcble is selected",
        g_c.titan_settings &&
        !gtk_widget_get_visible(g_c.titan_settings));

    s.output_backend = 0;
    gtk_shell_update(sh, &s);
    sleep(1);

    t_ok(
        "pcble panel hides again when Titan is selected",
        g_c.pcble_settings &&
        !gtk_widget_get_visible(g_c.pcble_settings));

    /*
     * Pin the two distinct shaping paths to the widgets themselves.
     * This catches the easy regression where a slider is visible but its
     * on_scale() branch was forgotten.
     */
    gtk_range_set_value(
        GTK_RANGE(g_c.range[0]),
        73);

    gtk_range_set_value(
        GTK_RANGE(g_c.output_range[0]),
        84);

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(g_c.output_invert_ry),
        TRUE);

    SDL_LockMutex(sh->lock);

    int input_range =
        sh->settings.stick_range[0];

    int output_range =
        sh->settings.output_stick_range[0];

    int output_invert =
        sh->settings.output_invert_ry;

    SDL_UnlockMutex(sh->lock);

    t_eq_int(
        "Input shaping slider updates local input settings",
        input_range,
        73);

    t_eq_int(
        "Output shaping slider updates final output settings",
        output_range,
        84);

    t_eq_int(
        "Output shaping toggle updates final output settings",
        output_invert,
        1);

    t_ok(
        "Local left stick preview exists",
        g_c.input_stick_preview[0] != NULL);

    t_ok(
        "Local right stick preview exists",
        g_c.input_stick_preview[1] != NULL);

    t_ok(
        "Output left stick preview exists",
        g_c.output_stick_preview[0] != NULL);

    t_ok(
        "Output right stick preview exists",
        g_c.output_stick_preview[1] != NULL);

    /* A second pass with different values, because a combo box with no
     * matching row and a scale outside its range both warn. */
    s.wiiu_console_height = 1080;
    s.wiiu_console_bitrate_mbps = 30;
    s.wiiu_pad_bitrate_mbps = 20;
    s.browser_height = 480;
    s.wiiu_console_enabled = 1;
    s.wiiu_pad_enabled = 1;
    gtk_shell_update(sh, &s);
    sleep(1);

    gtk_shell_stop(sh);

    t_ok("no GTK warning while building it", g_warnings == 0);
    if (g_warnings) {
        printf("  first of %d: %s\n", g_warnings, g_first);
    }
    return t_report();
}
