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
    static const char *const N[] = {"titan", "pcble2joycon2", "jocp"};
    return (i >= 0 && i < 3) ? N[i] : "";
}
const char *gamepad_bridge_backend_label(int i) {
    static const char *const L[] = {
        "Titan / ConsoleTuner USB",
        "Joy-Con 2 Bluetooth (pcble2joycon2)",
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
int gamepad_bridge_backend_available(int i) { return i == 0; }
int gamepad_bridge_backend_from_name(const char *name) {
    return name && strcmp(name, "titan") == 0 ? 0 : -1;
}
int gamepad_bridge_backend_configured_index(void) { return 0; }
const char *gamepad_protocol_label(int i) {
    static const char *const L[] = {"auto", "ps3", "xb360", "wiiu", "ps4", "xb1", "switch"};
    return (i >= 0 && i < 7) ? L[i] : "?";
}
int gamepad_protocol_count(void) { return 7; }

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
        "overview receives the live server status",
        g_c.overview_status &&
        strcmp(
            gtk_label_get_text(GTK_LABEL(g_c.overview_status)),
            "starting...") != 0);

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
