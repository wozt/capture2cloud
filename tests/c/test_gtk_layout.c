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
#include "../../server/app_config.c"
#include "../../server/reset_method.h"

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

int reset_method_capture_bluetooth_beacon(
    const char *adapter_id,
    char *message,
    size_t message_size)
{
    (void)adapter_id;

    if (message && message_size) {
        snprintf(
            message,
            message_size,
            "Captured");
    }

    return 1;
}

int reset_method_test_bluetooth_beacon(
    const char *adapter_id,
    char *message,
    size_t message_size)
{
    (void)adapter_id;

    if (message && message_size) {
        snprintf(
            message,
            message_size,
            "Transmitted");
    }

    return 1;
}




#include "../../server/wozt_ui_standard.c"
#include "../../server/gtk_shell.c"

#include "test_util.h"

static int g_warnings;
static char g_first[512];

/* Every GLib message, from any thread, including the ones GTK would
 * otherwise print and nobody would read. */
static void count_warnings(const gchar *domain, GLogLevelFlags level, const gchar *message,
                           gpointer user_data) {
    (void)user_data;
    if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR)) {
        /* No icon warning is ignored: paths are now provided by
         * C2C_RESOURCE_ROOT exclusively for this test executable. */
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

/* What server/gtk_shell.c calls into the USB bridge. Linking the real one would
 * drag libusb and an adapter into a test about a window. */
int gamepad_bridge_link_up(void) { return 0; }
double gamepad_bridge_report_rate(void) { return 0.0; }
int gamepad_bridge_console(void) { return 0; }
void gamepad_bridge_reset(void) {}
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

/* GTK layout test stub: never accesses Bluetooth hardware. */
void output_pcble_set_mouse_side(int side) { (void)side; }
void output_pcble_stop_session(void) {}
int output_pcble_ipc_up(void) { return 0; }
int output_pcble_session_running(void) { return 0; }
int output_pcble_reconnect_pending(void) { return 0; }
int output_pcble_reconnect_active(void) { return 0; }
int output_pcble_session_stopping(void) { return 0; }

void output_pcble_peer(char *out, size_t out_size)
{
    if (out && out_size) out[0] = '\0';
}

/* All GTK widget calls must execute in the gtk-shell thread. The normal
 * application deliberately owns its GTK main loop on that thread. In the
 * old test, main() performed GTK operations while the GTK thread was doing
 * CSS/layout work, resulting in gtk_css_value_inherit_free / SIGABRT.
 *
 * g_timeout_add() attaches to the default GLib main context, dispatched by
 * gtk_main() in gtk_thread_main(). The test main thread only uses SDL
 * mutex-protected application APIs and waits on the completion condition. */
typedef struct {
    GtkShell *shell;
    AppSettings settings;
    GMutex lock;
    GCond cond;
    gboolean done;
} GtkLayoutProbe;

static gboolean gtk_layout_probe_initial(gpointer user_data);
static gboolean gtk_layout_probe_pcble(gpointer user_data);
static gboolean gtk_layout_probe_titan(gpointer user_data);
static gboolean gtk_layout_probe_complete(gpointer user_data);

static void gtk_layout_probe_finish(GtkLayoutProbe *probe)
{
    g_mutex_lock(&probe->lock);
    probe->done = TRUE;
    g_cond_signal(&probe->cond);
    g_mutex_unlock(&probe->lock);
}

static gboolean gtk_layout_probe_initial(gpointer user_data)
{
    GtkLayoutProbe *probe = user_data;
    GtkShell *sh = probe->shell;
    t_ok("GTK layout probe runs on the GTK main context",
         g_main_context_is_owner(g_main_context_default()));
    t_ok("WOZT standard was created", sh->wozt != NULL);
    if (sh->wozt) {
        t_eq_int("seven WOZT JSON themes loaded", sh->wozt->themes->len, 7);
        t_eq_int("nine WOZT JSON languages loaded", sh->wozt->languages->len, 9);
    }
    t_eq_int("eight WOZT functional sections", g_c.wozt_section_count, 8);
    t_ok("sidebar replaced by dashboard", g_c.wozt_dashboard != NULL);

    /* GtkWindow must contain one and only one content overlay. */
    GtkWidget *child = sh->settings_window
        ? gtk_bin_get_child(GTK_BIN(sh->settings_window)) : NULL;
    t_ok("WOZT overlay is the only direct window child",
         child && GTK_IS_OVERLAY(child));
    t_ok("WOZT overlay owns its root box",
         child && GTK_IS_OVERLAY(child) &&
         GTK_IS_BOX(gtk_bin_get_child(GTK_BIN(child))));
    t_ok("GTK title bar is borderless",
         sh->settings_window &&
         !gtk_window_get_decorated(GTK_WINDOW(sh->settings_window)));
    t_ok("Clients retains its own notebook",
         g_c.wozt_clients_notebook &&
         gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_c.wozt_clients_notebook)) == 5);
    t_ok("Theme and language selectors exist",
         g_c.wozt_theme_combo && g_c.wozt_language_combo);
    /* The native Cairo stick preview reads the same palette as the CSS
     * provider. Validate a loaded color, not a hard-coded blue preview. */
    GdkRGBA preview_accent;
    GdkRGBA preview_background;
    t_ok("Stick preview receives a live WOZT accent color",
         sh->wozt && wozt_ui_standard_get_color(sh->wozt, "accent", &preview_accent));
    t_ok("Stick preview receives a live WOZT background color",
         sh->wozt && wozt_ui_standard_get_color(sh->wozt, "entry", &preview_background));
    char *slider_rules = sh->wozt ? gtk_css_provider_to_string(sh->wozt->css) : NULL;
    t_ok("Slider track, highlight and thumb have palette-driven CSS",
         slider_rules && strstr(slider_rules, "scale trough") &&
         strstr(slider_rules, "scale trough highlight") &&
         strstr(slider_rules, "scale slider") &&
         strstr(slider_rules, "background-image:none"));
    g_free(slider_rules);
    GList *output_children = g_c.wozt_inner_output
        ? gtk_container_get_children(GTK_CONTAINER(g_c.wozt_inner_output)) : NULL;
    GList *input_children = g_c.wozt_inner_input
        ? gtk_container_get_children(GTK_CONTAINER(g_c.wozt_inner_input)) : NULL;
    t_ok("Controller output has three genuine internal columns",
         g_c.wozt_inner_output && GTK_IS_BOX(g_c.wozt_inner_output) &&
         g_list_length(output_children) == 3);
    t_ok("Local input has three genuine internal columns",
         g_c.wozt_inner_input && GTK_IS_BOX(g_c.wozt_inner_input) &&
         g_list_length(input_children) == 3);
    g_list_free(output_children);
    g_list_free(input_children);
    t_ok("Output left and right stick controls occupy different inner grids",
         g_c.output_deadzone[0] && g_c.output_deadzone[1] &&
         gtk_widget_get_parent(g_c.output_deadzone[0]) !=
             gtk_widget_get_parent(g_c.output_deadzone[1]));
    t_ok("Input left and right stick controls occupy different inner grids",
         g_c.deadzone[0] && g_c.deadzone[1] &&
         gtk_widget_get_parent(g_c.deadzone[0]) !=
             gtk_widget_get_parent(g_c.deadzone[1]));
    /* The titlebar already owns the translated explanatory tooltip.
     * It must not appear as a second, space-consuming label. */
    if (g_c.wozt_sections[0]) {
        GtkWidget *outer = gtk_bin_get_child(GTK_BIN(g_c.wozt_sections[0]));
        GList *children = GTK_IS_CONTAINER(outer)
            ? gtk_container_get_children(GTK_CONTAINER(outer)) : NULL;
        t_eq_int("Section description is tooltip-only", g_list_length(children), 1);
        g_list_free(children);
    }
    t_ok("Controller section tooltips are available",
         g_c.wozt_sections[2] &&
         gtk_widget_get_tooltip_text(gtk_frame_get_label_widget(
             GTK_FRAME(g_c.wozt_sections[2]))) != NULL);
    gint default_width = 0, default_height = 0;
    gtk_window_get_default_size(GTK_WINDOW(sh->settings_window),
                                &default_width, &default_height);
    t_ok("Default window is 1144x840 (+10% width, +20% height)",
         default_width == 1144 && default_height == 840);

    t_eq_int("1040px window uses multiple dashboard columns",
             wozt_dashboard_layout_for_width(1040), 1);
    t_eq_int("1650px window uses side-by-side controller sections",
             wozt_dashboard_layout_for_width(1650), 2);
    t_eq_int("2200px window uses three dashboard columns",
             wozt_dashboard_layout_for_width(2200), 3);
    GtkWidget *small_brand = sh->settings_window
        ? g_object_get_data(G_OBJECT(sh->settings_window), "wozt-brand-icon") : NULL;
    GdkPixbuf *brand_pixbuf = small_brand && GTK_IS_IMAGE(small_brand)
        ? gtk_image_get_pixbuf(GTK_IMAGE(small_brand)) : NULL;
    t_ok("Header PNG was really resampled to 20x20",
         brand_pixbuf && gdk_pixbuf_get_width(brand_pixbuf) <= 20 &&
         gdk_pixbuf_get_height(brand_pixbuf) <= 20);
    GtkWidget *browser_page = g_c.wozt_clients_notebook
        ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_c.wozt_clients_notebook), 0) : NULL;
    t_ok("Client tab contents scroll instead of reserving tallest tab height",
         browser_page && GTK_IS_SCROLLED_WINDOW(browser_page));
    gint preview_width = 0, preview_height = 0;
    if (g_c.input_stick_preview[0])
        gtk_widget_get_size_request(g_c.input_stick_preview[0],
                                    &preview_width, &preview_height);
    t_ok("Stick preview is bounded to 116px",
         g_c.input_stick_preview[0] && preview_width == 116 && preview_height == 116);
    t_ok("Stick sliders no longer request entire panel widths",
         g_c.range[0] && !gtk_widget_get_hexpand(g_c.range[0]));
    /* Each calibration group lives in a DISTINCT GtkGrid: both grids have
     * their own label=0/control=1 columns. Checking global column 3 was an
     * obsolete assertion from the earlier single-grid (four-column) UI. */
    GtkWidget *output_right_grid = g_c.output_deadzone[1]
        ? gtk_widget_get_parent(g_c.output_deadzone[1]) : NULL;
    GtkWidget *output_left_grid = g_c.pcble_controller
        ? gtk_widget_get_parent(g_c.pcble_controller) : NULL;
    gint right_axis_col = -1;
    if (output_right_grid && GTK_IS_GRID(output_right_grid))
        gtk_container_child_get(GTK_CONTAINER(output_right_grid),
                                g_c.output_deadzone[1], "left-attach", &right_axis_col, NULL);
    t_eq_int("Right output calibration uses its own grid control column",
             right_axis_col, 1);
    t_ok("Output calibration and backend are in independent inner grids",
         output_right_grid && GTK_IS_GRID(output_right_grid) &&
         output_left_grid && GTK_IS_GRID(output_left_grid) &&
         output_right_grid != output_left_grid);
    GtkWidget *input_right_grid = g_c.deadzone[1]
        ? gtk_widget_get_parent(g_c.deadzone[1]) : NULL;
    gint input_axis_col = -1;
    if (input_right_grid && GTK_IS_GRID(input_right_grid))
        gtk_container_child_get(GTK_CONTAINER(input_right_grid),
                                g_c.deadzone[1], "left-attach", &input_axis_col, NULL);
    t_eq_int("Local input calibration uses its own grid control column",
             input_axis_col, 1);
    t_ok("Input calibration and source controller live in separate grids",
         input_right_grid && GTK_IS_GRID(input_right_grid) &&
         g_c.gamepad_device &&
         input_right_grid != gtk_widget_get_parent(g_c.gamepad_device));
    wozt_dashboard_reflow(1);
    if (g_c.wozt_dashboard && g_c.wozt_sections[1]) {
        gint clients_col = -1, output_span = 0;
        gtk_container_child_get(GTK_CONTAINER(g_c.wozt_dashboard), g_c.wozt_sections[1],
                                "left-attach", &clients_col, NULL);
        gtk_container_child_get(GTK_CONTAINER(g_c.wozt_dashboard), g_c.wozt_sections[2],
                                "width", &output_span, NULL);
        t_eq_int("Clients is in the second dashboard column", clients_col, 1);
        t_eq_int("Controller output spans both columns at 1040px", output_span, 2);
    }
    wozt_dashboard_reflow(wozt_dashboard_layout_for_width(
        gtk_widget_get_allocated_width(sh->settings_window)));

    /* Real live-theme and localization checks, also on the GTK thread.
     * Restore the initial theme/language without touching persistent .env. */
    if (sh->wozt && sh->wozt->themes->len == 7) {
        char *initial = g_strdup(wozt_ui_standard_theme_id(sh->wozt));
        wozt_ui_standard_apply_theme(sh->wozt, "ocean");
        t_eq_str("Ocean theme applies live", wozt_ui_standard_theme_id(sh->wozt), "ocean");
        wozt_ui_standard_apply_theme(sh->wozt, initial);
        t_eq_str("Original theme restored", wozt_ui_standard_theme_id(sh->wozt), initial);
        g_free(initial);
    }
    if (sh->wozt && sh->wozt->languages->len == 9) {
        char *initial = g_strdup(wozt_ui_standard_language_id(sh->wozt));
        sh->loading = 1;
        wozt_ui_standard_apply_language(sh->wozt, "fr", sh->settings_window);
        t_eq_str("French localization applies live", wozt_ui_standard_language_id(sh->wozt), "fr");
        GtkWidget *frame_heading = g_c.wozt_sections[0]
            ? gtk_frame_get_label_widget(GTK_FRAME(g_c.wozt_sections[0])) : NULL;
        GList *heading_labels = GTK_IS_CONTAINER(frame_heading)
            ? gtk_container_get_children(GTK_CONTAINER(frame_heading)) : NULL;
        gboolean title_is_french = FALSE;
        for (GList *it = heading_labels; it; it = it->next) {
            if (GTK_IS_LABEL(it->data) &&
                !g_strcmp0(gtk_label_get_text(GTK_LABEL(it->data)), "Vue d'ensemble"))
                title_is_french = TRUE;
        }
        t_ok("Overview frame heading is translated", title_is_french);
        g_list_free(heading_labels);
        gchar *help_fr = frame_heading ? gtk_widget_get_tooltip_text(frame_heading) : NULL;
        t_ok("Overview frame tooltip is translated", help_fr &&
             strstr(help_fr, "Les informations à voir") != NULL);
        g_free(help_fr);
        wozt_ui_standard_apply_language(sh->wozt, initial, sh->settings_window);
        sh->loading = 0;
        t_eq_str("Original language restored", wozt_ui_standard_language_id(sh->wozt), initial);
        g_free(initial);
    }

    t_ok("Reset method selector exists", g_c.reset_method != NULL);
    t_ok("Reset method page has script controls",
         g_c.reset_script && g_c.reset_script_panel);
    if (g_c.reset_method) {
        t_eq_int("Reset method defaults to script in the layout test",
                 gtk_combo_box_get_active(GTK_COMBO_BOX(g_c.reset_method)), 0);
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.reset_method), 1);
        t_eq_int("Selecting Bluetooth persists through reset_method_set",
                 g_test_reset_method, RESET_METHOD_BLUETOOTH);
    }
    t_ok("Bluetooth reset panel becomes visible",
         g_c.reset_bt_panel && gtk_widget_get_visible(g_c.reset_bt_panel));
    t_ok("Bluetooth target selector exists", g_c.reset_bt_target != NULL);
    t_ok("Bluetooth adapter selector exists", g_c.reset_bt_adapter != NULL);
    t_ok("Test dongle button exists", g_c.reset_bt_test_adapter != NULL);
    t_ok("Capture wake beacon button exists", g_c.reset_bt_capture != NULL);
    t_ok("Test beacon button exists", g_c.reset_bt_test_beacon != NULL);
    t_ok("overview receives the live server status",
         g_c.overview_status &&
         strcmp(gtk_label_get_text(GTK_LABEL(g_c.overview_status)), "starting...") != 0);
    t_ok("all dedicated native-client port controls exist",
         g_c.wiiu_pad_port && g_c.wiiu_console_port && g_c.old3ds_port);
    if (g_c.old3ds_port)
        t_eq_int("Old3DS port is loaded into its own tab",
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_c.old3ds_port)), 5085);

    /* Let the application's normal 200ms on_tick consume this update. */
    probe->settings.output_backend = 1;
    gtk_shell_update(sh, &probe->settings);
    g_timeout_add(600, gtk_layout_probe_pcble, probe);
    return G_SOURCE_REMOVE;
}

static gboolean gtk_layout_probe_pcble(gpointer user_data)
{
    GtkLayoutProbe *probe = user_data;
    t_ok("PCBLE probe remains on GTK main context",
         g_main_context_is_owner(g_main_context_default()));
    t_ok("pcble panel is visible when pcble2gamepad is selected",
         g_c.pcble_settings && gtk_widget_get_visible(g_c.pcble_settings));
    t_ok("pcble Pair / Sync button is visible",
         g_c.pcble_pair && gtk_widget_get_visible(g_c.pcble_pair));
    t_ok("Titan panel is hidden while pcble is selected",
         g_c.titan_settings && !gtk_widget_get_visible(g_c.titan_settings));
    probe->settings.output_backend = 0;
    gtk_shell_update(probe->shell, &probe->settings);
    g_timeout_add(600, gtk_layout_probe_titan, probe);
    return G_SOURCE_REMOVE;
}

static gboolean gtk_layout_probe_titan(gpointer user_data)
{
    GtkLayoutProbe *probe = user_data;
    GtkShell *sh = probe->shell;
    t_ok("Titan probe remains on GTK main context",
         g_main_context_is_owner(g_main_context_default()));
    t_ok("pcble panel hides again when Titan is selected",
         g_c.pcble_settings && !gtk_widget_get_visible(g_c.pcble_settings));

    /* These callbacks drive real settings and must not race GTK redraw. */
    if (g_c.range[0]) gtk_range_set_value(GTK_RANGE(g_c.range[0]), 73);
    if (g_c.output_range[0]) gtk_range_set_value(GTK_RANGE(g_c.output_range[0]), 84);
    if (g_c.output_invert_ry)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.output_invert_ry), TRUE);
    SDL_LockMutex(sh->lock);
    const int input_range = sh->settings.stick_range[0];
    const int output_range = sh->settings.output_stick_range[0];
    const int output_invert = sh->settings.output_invert_ry;
    SDL_UnlockMutex(sh->lock);
    t_eq_int("Input shaping slider updates local input settings", input_range, 73);
    t_eq_int("Output shaping slider updates final output settings", output_range, 84);
    t_eq_int("Output shaping toggle updates final output settings", output_invert, 1);
    t_ok("Local left stick preview exists", g_c.input_stick_preview[0] != NULL);
    t_ok("Local right stick preview exists", g_c.input_stick_preview[1] != NULL);
    t_ok("Output left stick preview exists", g_c.output_stick_preview[0] != NULL);
    t_ok("Output right stick preview exists", g_c.output_stick_preview[1] != NULL);

    probe->settings.wiiu_console_height = 1080;
    probe->settings.wiiu_console_bitrate_mbps = 30;
    probe->settings.wiiu_pad_bitrate_mbps = 20;
    probe->settings.browser_height = 480;
    probe->settings.wiiu_console_enabled = 1;
    probe->settings.wiiu_pad_enabled = 1;
    gtk_shell_update(sh, &probe->settings);
    g_timeout_add(600, gtk_layout_probe_complete, probe);
    return G_SOURCE_REMOVE;
}

static gboolean gtk_layout_probe_complete(gpointer user_data)
{
    GtkLayoutProbe *probe = user_data;
    t_ok("Final GTK probe remains on GTK main context",
         g_main_context_is_owner(g_main_context_default()));
    gtk_layout_probe_finish(probe);
    return G_SOURCE_REMOVE;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("no display; skipped\n");
        return 0;
    }
    g_log_set_default_handler(count_warnings, NULL);
    AppSettings settings = APP_SETTINGS_DEFAULTS;
    GtkShellCallbacks callbacks = {on_settings, on_action, on_pair, NULL};
    t_begin("the settings window builds and opens");
    GtkShell *shell = gtk_shell_start(&settings, &callbacks);
    t_ok("the shell starts", shell != NULL);
    if (!shell) return t_report();

    /* Application-facing APIs are mutex/idle based; no direct GTK calls. */
    for (int i = 0; i < GTK_SHELL_CLIENT_COUNT; i++)
        gtk_shell_set_client_status(shell, i, "2 connected — 1280x720@60, 8000 kbps");
    gtk_shell_set_wiiu_status(shell, "waiting for a pad");
    gtk_shell_set_status(shell,
        "running — capture online — browser server listening — native server listening");
    gtk_shell_update(shell, &settings);
    gtk_shell_debug_show_settings(shell);

    GtkLayoutProbe probe = {.shell = shell, .settings = settings};
    g_mutex_init(&probe.lock);
    g_cond_init(&probe.cond);
    /* The timer callback is dispatched by the GTK main loop (gtk-shell),
     * not by the calling thread. Give the initial UI 500ms to settle. */
    g_timeout_add(500, gtk_layout_probe_initial, &probe);
    g_mutex_lock(&probe.lock);
    const gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!probe.done && g_cond_wait_until(&probe.cond, &probe.lock, deadline)) {
        /* Re-evaluate the condition after each wakeup. */
    }
    const gboolean completed = probe.done;
    g_mutex_unlock(&probe.lock);
    gtk_shell_stop(shell);
    g_cond_clear(&probe.cond);
    g_mutex_clear(&probe.lock);

    t_ok("GTK-thread probe finishes without timeout", completed);
    t_ok("no GTK warning while building it", g_warnings == 0);
    if (g_warnings)
        printf("  first of %d: %s\n", g_warnings, g_first);
    return t_report();
}
