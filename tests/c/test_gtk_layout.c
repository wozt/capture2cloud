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
/* The compact CSS tokens omit whitespace. GTK3 expands CSS shorthands
 * and serializes selectors/declarations in its own canonical order. Ensure
 * each declaration belongs to the requested rule rather than another scale. */
static gboolean wozt_css_rule_has_declaration(const char *css,
                                              const char *selector,
                                              const char *declaration)
{
    const char *rule = css ? strstr(css, selector) : NULL;
    const char *end = rule ? strchr(rule, '}') : NULL;
    const char *found = rule ? strstr(rule, declaration) : NULL;
    return end && found && found < end;
}

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
    /* GtkCssProvider serializes declarations as e.g. "background-image: none"
     * even if the generated source had "background-image:none". Assert on
     * whitespace-independent CSS tokens, not one serialization format. */
    GString *slider_tokens = slider_rules ? g_string_sized_new(strlen(slider_rules)) : NULL;
    if (slider_tokens) {
        for (const unsigned char *p = (const unsigned char *)slider_rules; *p; ++p)
            if (!g_ascii_isspace(*p)) g_string_append_c(slider_tokens, (char)*p);
    }
    const char *slider_css = slider_tokens ? slider_tokens->str : NULL;
    t_ok("Slider track, highlight and thumb have palette-driven CSS",
         slider_css && strstr(slider_css, "scaletrough{") &&
         strstr(slider_css, "scaletroughhighlight") &&
         strstr(slider_css, "scaleslider") &&
         strstr(slider_css, "background-image:none") &&
         strstr(slider_css, "background-color:"));
    /* GTK3 serializes border-width:0 as four longhand border widths.
     * Check the actual calibration CSS rules, not a raw shorthand string
     * or declarations belonging to unrelated scales. */
    t_ok("Controller slider CSS defines a 3px track and 12px thumb core",
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaletrough{", "min-height:3px;") &&
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaletrough{", "border-top-width:0;") &&
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaletrough{", "border-bottom-width:0;") &&
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaleslider{", "min-height:12px;") &&
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaleslider{", "min-width:12px;") &&
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaleslider{", "border-top-width:1px;") &&
         /* Both the empty track and the filled highlight paint a fixed
          * 2px stripe, irrespective of their allocated GTK node height. */
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaletrough{", "background-size:100%2px;") &&
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaletroughhighlight{", "background-size:100%2px;") &&
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaletroughhighlight{", "background-image:") &&
         wozt_css_rule_has_declaration(
             slider_css, "scale.wozt-calibration-scaletrough{", "background-image:"));
    if (slider_tokens) g_string_free(slider_tokens, TRUE);
    g_free(slider_rules);
    GList *output_children = g_c.wozt_inner_output
        ? gtk_container_get_children(GTK_CONTAINER(g_c.wozt_inner_output)) : NULL;
    GtkWidget *reconnect_text = g_c.pcble_reconnect && GTK_IS_BIN(g_c.pcble_reconnect)
        ? gtk_bin_get_child(GTK_BIN(g_c.pcble_reconnect)) : NULL;
    t_ok("PCBLE buttons show the complete text without ellipses or wrapping",
         reconnect_text && GTK_IS_LABEL(reconnect_text) &&
         !gtk_label_get_line_wrap(GTK_LABEL(reconnect_text)) &&
         gtk_label_get_ellipsize(GTK_LABEL(reconnect_text)) == PANGO_ELLIPSIZE_NONE);
    t_ok("Controller output has two permanent horizontal columns",
         g_c.wozt_inner_output && GTK_IS_BOX(g_c.wozt_inner_output) &&
         g_list_length(output_children) == 2 &&
         gtk_orientable_get_orientation(GTK_ORIENTABLE(g_c.wozt_inner_output)) ==
             GTK_ORIENTATION_HORIZONTAL);
    t_ok("Local input uses one shared vertical grid",
         g_c.wozt_inner_input && GTK_IS_GRID(g_c.wozt_inner_input) &&
         gtk_widget_get_parent(g_c.gamepad_enabled) == g_c.wozt_inner_input &&
         gtk_widget_get_parent(g_c.lt_threshold) == g_c.wozt_inner_input);
    g_list_free(output_children);
    t_ok("Output left and right stick controls share one compact grid",
         g_c.output_deadzone[0] && g_c.output_deadzone[1] &&
         gtk_widget_get_parent(g_c.output_deadzone[0]) ==
             gtk_widget_get_parent(g_c.output_deadzone[1]));
    t_ok("Input left and right stick controls share one compact grid",
         g_c.deadzone[0] && g_c.deadzone[1] &&
         gtk_widget_get_parent(g_c.deadzone[0]) ==
             gtk_widget_get_parent(g_c.deadzone[1]));
    GtkWidget *session_grid = g_c.pcble_reconnect
        ? gtk_widget_get_parent(g_c.pcble_reconnect) : NULL;
    gint pair_column = -1, stop_row = -1, refresh_row = -1;
    if (GTK_IS_GRID(session_grid)) {
        gtk_container_child_get(GTK_CONTAINER(session_grid), g_c.pcble_pair,
                                "left-attach", &pair_column, NULL);
        gtk_container_child_get(GTK_CONTAINER(session_grid), g_c.pcble_stop,
                                "top-attach", &stop_row, NULL);
        gtk_container_child_get(GTK_CONTAINER(session_grid), g_c.pcble_refresh,
                                "top-attach", &refresh_row, NULL);
    }
    t_ok("PCBLE session controls form two buttons per row",
         GTK_IS_GRID(session_grid) && pair_column == 1 &&
         stop_row == 1 && refresh_row == 1);
    gint session_column = -1;
    if (GTK_IS_GRID(session_grid) && GTK_IS_GRID(g_c.pcble_settings))
        gtk_container_child_get(GTK_CONTAINER(g_c.pcble_settings), session_grid,
                                "left-attach", &session_column, NULL);
    t_eq_int("Bluetooth action buttons begin in leftmost column, no label",
             session_column, 0);
    gint combo_width = -1;
    if (g_c.pcble_controller)
        gtk_widget_get_size_request(g_c.pcble_controller, &combo_width, NULL);
    t_eq_int("Controller profile selector aligns to Backend right edge",
             combo_width, 170);
    gint primary_combo_width = -1, secondary_combo_width = -1;
    if (g_c.pcble_primary)
        gtk_widget_get_size_request(g_c.pcble_primary, &primary_combo_width, NULL);
    if (g_c.pcble_secondary)
        gtk_widget_get_size_request(g_c.pcble_secondary, &secondary_combo_width, NULL);
    t_ok("PCBLE selectors use one common compact width",
         primary_combo_width == 170 && secondary_combo_width == 170);
    GList *profile_renderers = g_c.pcble_controller
        ? gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(g_c.pcble_controller)) : NULL;
    gboolean profile_single_line = FALSE;
    for (GList *it = profile_renderers; it; it = it->next) {
        if (GTK_IS_CELL_RENDERER_TEXT(it->data)) {
            gboolean single_paragraph = FALSE;
            gint fixed_height = -1;
            g_object_get(it->data, "single-paragraph-mode", &single_paragraph, NULL);
            gtk_cell_renderer_get_fixed_size(GTK_CELL_RENDERER(it->data),
                                             NULL, &fixed_height);
            if (single_paragraph && fixed_height == 18)
                profile_single_line = TRUE;
        }
    }
    g_list_free(profile_renderers);
    t_ok("Emulated controller combo uses one text line", profile_single_line);

    GtkWidget *sliders_grid = g_c.output_lt_threshold
        ? gtk_widget_get_parent(g_c.output_lt_threshold) : NULL;
    gint lt_row = -1, left_final_row = -1, rt_row = -1, right_final_row = -1;
    gint lt_col = -1, rt_col = -1;
    if (GTK_IS_GRID(sliders_grid) && g_c.output_rt_threshold &&
        g_c.output_diagonal[0] && g_c.output_diagonal[1] &&
        gtk_widget_get_parent(g_c.output_rt_threshold) == sliders_grid) {
        gtk_container_child_get(GTK_CONTAINER(sliders_grid), g_c.output_lt_threshold,
                                "top-attach", &lt_row, "left-attach", &lt_col, NULL);
        gtk_container_child_get(GTK_CONTAINER(sliders_grid), g_c.output_diagonal[0],
                                "top-attach", &left_final_row, NULL);
        gtk_container_child_get(GTK_CONTAINER(sliders_grid), g_c.output_rt_threshold,
                                "top-attach", &rt_row, "left-attach", &rt_col, NULL);
        gtk_container_child_get(GTK_CONTAINER(sliders_grid), g_c.output_diagonal[1],
                                "top-attach", &right_final_row, NULL);
    }
    t_ok("Output sliders use one column, left group above right group",
         GTK_IS_GRID(sliders_grid) && lt_col == 1 && rt_col == 1 &&
         lt_row >= 0 && left_final_row == lt_row + 3 &&
         rt_row == lt_row + 4 && right_final_row == lt_row + 7);
    t_ok("Output frame expands to the available row width",
         g_c.wozt_sections[2] && gtk_widget_get_hexpand(g_c.wozt_sections[2]));
    gint output_preview_w[2] = {-1, -1};
    gint output_preview_h[2] = {-1, -1};
    gint output_slider_width = -1;
    for (int side = 0; side < 2; ++side)
        if (g_c.output_stick_preview[side])
            gtk_widget_get_size_request(g_c.output_stick_preview[side],
                                        &output_preview_w[side], &output_preview_h[side]);
    if (g_c.output_deadzone[0])
        gtk_widget_get_size_request(g_c.output_deadzone[0],
                                    &output_slider_width, NULL);
    t_ok("Output stick previews are 70 by 70px",
         output_preview_w[0] == 70 && output_preview_w[1] == 70 &&
         output_preview_h[0] == 70 && output_preview_h[1] == 70);
    GtkWidget *logging_row = g_c.pcble_verbose
        ? gtk_widget_get_parent(g_c.pcble_verbose) : NULL;
    GtkWidget *diagnostics_grid = GTK_IS_BOX(logging_row)
        ? gtk_widget_get_parent(logging_row) : NULL;
    t_ok("PCBLE Diagnostics starts the second Output column",
         GTK_IS_GRID(diagnostics_grid) &&
         gtk_widget_get_parent(diagnostics_grid) == sliders_grid &&
         gtk_grid_get_child_at(GTK_GRID(sliders_grid), 0, 0) == diagnostics_grid &&
         gtk_widget_get_parent(g_c.pcble_verbose) == logging_row);
    GtkWidget *diagnostics_heading = GTK_IS_GRID(diagnostics_grid)
        ? gtk_grid_get_child_at(GTK_GRID(diagnostics_grid), 0, 0) : NULL;
    t_ok("Diagnostics title is created and explicitly shown",
         GTK_IS_LABEL(diagnostics_heading) &&
         g_strcmp0(gtk_label_get_text(GTK_LABEL(diagnostics_heading)),
                   "Diagnostics") == 0 &&
         gtk_widget_get_visible(diagnostics_heading));
    t_ok("Output logging toggle sits immediately after its label",
         GTK_IS_BOX(logging_row) && GTK_IS_GRID(diagnostics_grid));
    GtkWidget *preview_frame = g_c.output_stick_preview[0]
        ? gtk_widget_get_parent(g_c.output_stick_preview[0]) : NULL;
    GtkWidget *preview_pair = preview_frame
        ? gtk_widget_get_parent(preview_frame) : NULL;
    GtkWidget *preview_inline = preview_pair
        ? gtk_widget_get_parent(preview_pair) : NULL;
    GList *output_preview_children = GTK_IS_BOX(preview_inline)
        ? gtk_container_get_children(GTK_CONTAINER(preview_inline)) : NULL;
    t_ok("Output preview remains inline without its redundant caption",
         GTK_IS_BOX(preview_inline) &&
         gtk_widget_get_parent(preview_inline) == sliders_grid &&
         g_list_length(output_preview_children) == 1);
    g_list_free(output_preview_children);
    GList *packed_output = g_c.wozt_inner_output
        ? gtk_container_get_children(GTK_CONTAINER(g_c.wozt_inner_output)) : NULL;
    gboolean output_children_natural = g_list_length(packed_output) == 2;
    for (GList *it = packed_output; it; it = it->next) {
        gboolean expand = TRUE, fill = TRUE;
        guint padding = 0;
        GtkPackType pack_type = GTK_PACK_START;
        gtk_box_query_child_packing(GTK_BOX(g_c.wozt_inner_output),
                                    GTK_WIDGET(it->data), &expand, &fill,
                                    &padding, &pack_type);
        if (expand || fill) output_children_natural = FALSE;
    }
    g_list_free(packed_output);
    t_ok("Controller output columns no longer spread into unused width",
         output_children_natural);
    t_ok("Output sliders request a compact 116px width",
         output_slider_width == 116);
    /* The main window may narrow, but the TWO internal output grids
     * must never flip orientation as dashboard rows reflow. */
    wozt_dashboard_reflow(720);
    GdkEventConfigure narrow = {0};
    narrow.width = 720;
    wozt_window_configured(sh->settings_window, &narrow, NULL);
    t_ok("Output remains horizontal below the former 665px cutoff",
         GTK_IS_BOX(g_c.wozt_inner_output) &&
         gtk_orientable_get_orientation(GTK_ORIENTABLE(g_c.wozt_inner_output)) ==
             GTK_ORIENTATION_HORIZONTAL);
    wozt_dashboard_reflow(1144);
    t_ok("Output calibration rows have zero spacing",
         GTK_IS_GRID(sliders_grid) &&
         gtk_grid_get_row_spacing(GTK_GRID(sliders_grid)) == 0);
    GtkWidget *color_grid = g_c.pcble_body_color
        ? gtk_widget_get_parent(g_c.pcble_body_color) : NULL;
    t_ok("PCBLE appearance swatches are in a compact nested grid",
         GTK_IS_GRID(color_grid) && color_grid != g_c.pcble_settings &&
         gtk_widget_get_parent(g_c.pcble_button_color) == color_grid);
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
    /* gtk_window_get_geometry_hints() does not exist in GTK3. The setter
     * records the configured minimum as window data for this regression
     * probe; also verify no explicit width request prevents shrinking. */
    gint requested_width = -1;
    gtk_widget_get_size_request(sh->settings_window, &requested_width, NULL);
    const gint configured_minimum = GPOINTER_TO_INT(g_object_get_data(
        G_OBJECT(sh->settings_window), "wozt-minimum-width"));
    t_ok("The window config permits shrinking to 600px",
         gtk_window_get_resizable(GTK_WINDOW(sh->settings_window)) &&
         configured_minimum == 600 &&
         (requested_width < 0 || requested_width <= configured_minimum));
    GtkWidget *dashboard_scroller = gtk_widget_get_parent(g_c.wozt_dashboard);
    if (GTK_IS_VIEWPORT(dashboard_scroller))
        dashboard_scroller = gtk_widget_get_parent(dashboard_scroller);
    GtkPolicyType horizontal = GTK_POLICY_NEVER, vertical = GTK_POLICY_NEVER;
    if (GTK_IS_SCROLLED_WINDOW(dashboard_scroller))
        gtk_scrolled_window_get_policy(GTK_SCROLLED_WINDOW(dashboard_scroller),
                                       &horizontal, &vertical);
    t_ok("Scrolled dashboard permits shrinking and safe overflow",
         GTK_IS_SCROLLED_WINDOW(dashboard_scroller) &&
         horizontal == GTK_POLICY_AUTOMATIC && vertical == GTK_POLICY_AUTOMATIC);

    /* Each functional row picks its own capacity; no shared grid columns. */
    t_eq_int("720px: overview and clients stack",
             wozt_dashboard_group_columns(720, 0), 1);
    /* WOZT_0840_RESPONSIVE_TESTS */
    t_eq_int("820px: compact Overview and Clients share a line",
             wozt_dashboard_group_columns(820, 0), 2);
    t_eq_int("780px: Overview and Clients still stack when narrow",
             wozt_dashboard_group_columns(780, 0), 1);
    t_eq_int("1144px: overview and clients fit on one line",
             wozt_dashboard_group_columns(1144, 0), 2);
    t_eq_int("1144px: compact input fits beside two-column Output",
             wozt_dashboard_group_columns(1144, 1), 2);
    t_eq_int("Unrealized Local input retains its 350px fallback",
             wozt_section_target_width[3], 350);
    /* Real GTK preferred widths, not the obsolete 620/350 estimates,
     * determine when the controller pair must wrap. */
    gint output_min = 0, output_nat = 0;
    gint input_min = 0, input_nat = 0;
    gtk_widget_get_preferred_width(g_c.wozt_sections[2],
                                   &output_min, &output_nat);
    gtk_widget_get_preferred_width(g_c.wozt_sections[3],
                                   &input_min, &input_nat);
    t_ok("Controller widths are measured from GTK content",
         output_nat > 0 && input_nat > 0 &&
         wozt_section_layout_width(2) == MAX(output_min, output_nat) &&
         wozt_section_layout_width(3) == MAX(input_min, input_nat));
    t_ok("Other dashboard rows preserve their independent width budgets",
         wozt_section_layout_width(0) == wozt_section_target_width[0] &&
         wozt_section_layout_width(1) == wozt_section_target_width[1] &&
         wozt_section_layout_width(4) == wozt_section_target_width[4] &&
         wozt_section_layout_width(7) == wozt_section_target_width[7]);
    /* Controller budgets now match their compact two-column contents.
     * Derive the joining boundary from their targets and the 36px gutter
     * allowance, rather than relying on a stale fixed-width test. */
    t_eq_int("1650px: both compact controller sections share a row",
             wozt_dashboard_group_columns(1650, 1), 2);
    const int controller_join_width =
        wozt_section_layout_width(2) + wozt_section_layout_width(3) + 36;
    t_eq_int("Controller sections separate just below their width budget",
             wozt_dashboard_group_columns(controller_join_width - 1, 1), 1);
    t_eq_int("Controller sections join at their width budget",
             wozt_dashboard_group_columns(controller_join_width, 1), 2);
    t_eq_int("1800px: controller sections share one line",
             wozt_dashboard_group_columns(1800, 1), 2);
    t_eq_int("1650px: all four small sections share one line",
             wozt_dashboard_group_columns(1650, 2), 4);
    /* Reflow thresholds must exceed the minimum widths of packed GTK
     * children: otherwise the window gets stuck before configure-event. */
    t_eq_int("1360px: four small sections reflow to three before blocking resize",
             wozt_dashboard_group_columns(1360, 2), 3);
    t_eq_int("1100px: small sections reflow to pairs",
             wozt_dashboard_group_columns(1100, 2), 2);
    t_eq_int("780px: small sections stack before their minimum width",
             wozt_dashboard_group_columns(780, 2), 1);
    t_eq_int("2200px: four small sections remain in one line",
             wozt_dashboard_group_columns(2200, 2), 4);
    t_ok("Dashboard is a vertical container of independent rows",
         GTK_IS_BOX(g_c.wozt_dashboard) &&
         gtk_orientable_get_orientation(GTK_ORIENTABLE(g_c.wozt_dashboard)) ==
             GTK_ORIENTATION_VERTICAL);
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
    gboolean client_tabs_are_icons = TRUE;
    gboolean pages_are_dense = TRUE;
    for (int ci = 0; ci < 5; ++ci) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_c.wozt_clients_notebook), ci);
        GtkWidget *tab = gtk_notebook_get_tab_label(GTK_NOTEBOOK(g_c.wozt_clients_notebook), page);
        GtkWidget *vp = GTK_IS_SCROLLED_WINDOW(page) ? gtk_bin_get_child(GTK_BIN(page)) : NULL;
        GtkWidget *inner = GTK_IS_VIEWPORT(vp) ? gtk_bin_get_child(GTK_BIN(vp)) : vp;
        client_tabs_are_icons = client_tabs_are_icons && GTK_IS_BOX(tab) &&
            gtk_widget_get_tooltip_text(tab) != NULL;
        pages_are_dense = pages_are_dense && GTK_IS_GRID(inner) &&
            gtk_grid_get_row_spacing(GTK_GRID(inner)) == 1 &&
            gtk_grid_get_column_spacing(GTK_GRID(inner)) == 3;
    }
    t_ok("Five Client tabs show small SVG icons with accessible names", client_tabs_are_icons);
    t_ok("All Client pages use compact one-pixel row spacing", pages_are_dense);
    t_ok("Client ports use one-frame GTK style and compact requisitions",
         g_c.port && g_c.switch_port && g_c.wiiu_pad_port &&
         g_c.wiiu_console_port && g_c.old3ds_port &&
         gtk_style_context_has_class(gtk_widget_get_style_context(g_c.port),
                                     "wozt-client-port") &&
         gtk_style_context_has_class(gtk_widget_get_style_context(g_c.old3ds_port),
                                     "wozt-client-port"));
    GtkWidget *wiiu_page = gtk_notebook_get_nth_page(
        GTK_NOTEBOOK(g_c.wozt_clients_notebook), 2);
    GtkWidget *wiiu_viewport = GTK_IS_SCROLLED_WINDOW(wiiu_page)
        ? gtk_bin_get_child(GTK_BIN(wiiu_page)) : NULL;
    GtkWidget *wiiu_root = GTK_IS_VIEWPORT(wiiu_viewport)
        ? gtk_bin_get_child(GTK_BIN(wiiu_viewport)) : wiiu_viewport;
    GtkWidget *wiiu_modes = GTK_IS_GRID(wiiu_root)
        ? gtk_grid_get_child_at(GTK_GRID(wiiu_root), 0, 0) : NULL;
    GList *wiiu_columns = GTK_IS_FLOW_BOX(wiiu_modes)
        ? gtk_container_get_children(GTK_CONTAINER(wiiu_modes)) : NULL;
    GtkWidget *direct = wiiu_columns && GTK_IS_BIN(wiiu_columns->data)
        ? gtk_bin_get_child(GTK_BIN(wiiu_columns->data)) : NULL;
    GtkWidget *wup = wiiu_columns && wiiu_columns->next &&
                     GTK_IS_BIN(wiiu_columns->next->data)
        ? gtk_bin_get_child(GTK_BIN(wiiu_columns->next->data)) : NULL;
    t_ok("GamePad page automatically flows between two mode columns",
         GTK_IS_FLOW_BOX(wiiu_modes) && g_list_length(wiiu_columns) == 2 &&
         GTK_IS_GRID(direct) && GTK_IS_GRID(wup) &&
         gtk_widget_get_parent(g_c.wiiu_pad_port) == direct &&
         gtk_widget_get_parent(g_c.wup_touch_mode) == wup &&
         gtk_flow_box_get_max_children_per_line(GTK_FLOW_BOX(wiiu_modes)) == 2);
    g_list_free(wiiu_columns);
    gint preview_width = 0, preview_height = 0;
    if (g_c.input_stick_preview[0])
        gtk_widget_get_size_request(g_c.input_stick_preview[0],
                                    &preview_width, &preview_height);
    gint preview_right_width = -1, preview_right_height = -1;
    if (g_c.input_stick_preview[1])
        gtk_widget_get_size_request(g_c.input_stick_preview[1],
                                    &preview_right_width, &preview_right_height);
    t_ok("Input stick previews are 70 by 70px",
         g_c.input_stick_preview[0] && preview_width == 70 && preview_height == 70 &&
         g_c.input_stick_preview[1] &&
         preview_right_width == 70 && preview_right_height == 70);
    t_ok("Stick sliders no longer request entire panel widths",
         g_c.range[0] && !gtk_widget_get_hexpand(g_c.range[0]));
    gint input_slider_width = -1;
    if (g_c.deadzone[0])
        gtk_widget_get_size_request(g_c.deadzone[0], &input_slider_width, NULL);
    t_eq_int("Input sliders match Output's 116px width", input_slider_width, 116);
    /* WOZT_0852: real GtkGrid labels, not a fake frame width-request.
     * The 116px scales must remain unchanged; the two width-dominating
     * labels have slightly tighter Pango glyph spacing. */
    gboolean input_label_spacing_ok = GTK_IS_GRID(g_c.wozt_inner_input);
    for (int side = 0; side < 2; ++side) {
        GtkWidget *scale = g_c.diagonal[side];
        gint diagonal_row = -1;
        if (!input_label_spacing_ok || !GTK_IS_SCALE(scale)) {
            input_label_spacing_ok = FALSE;
            continue;
        }
        gtk_container_child_get(GTK_CONTAINER(g_c.wozt_inner_input), scale,
                                "top-attach", &diagonal_row, NULL);
        GtkWidget *label = diagonal_row >= 0
            ? gtk_grid_get_child_at(GTK_GRID(g_c.wozt_inner_input),
                                    0, diagonal_row) : NULL;
        PangoAttrList *attrs = GTK_IS_LABEL(label)
            ? gtk_label_get_attributes(GTK_LABEL(label)) : NULL;
        if (!attrs) {
            input_label_spacing_ok = FALSE;
            continue;
        }
        PangoAttrIterator *iterator = pango_attr_list_get_iterator(attrs);
        PangoAttribute *attr = pango_attr_iterator_get(
            iterator, PANGO_ATTR_LETTER_SPACING);
        input_label_spacing_ok = input_label_spacing_ok && attr &&
            ((PangoAttrInt *)attr)->value == -660;
        pango_attr_iterator_destroy(iterator);
    }
    t_ok("Input grid regains 18px without shortening sliders or labels",
         input_label_spacing_ok && input_slider_width == 116);
    /* WOZT_0841_GTK_REGRESSIONS */
    GtkWidget *device_inline = g_c.gamepad_device
        ? gtk_widget_get_parent(g_c.gamepad_device) : NULL;
    GList *device_children = GTK_IS_BOX(device_inline)
        ? gtk_container_get_children(GTK_CONTAINER(device_inline)) : NULL;
    t_ok("Input device selector is inline with its label",
         GTK_IS_BOX(device_inline) && g_list_length(device_children) == 2 &&
         GTK_IS_LABEL(device_children->data));
    gint local_device_width = -1;
    if (g_c.gamepad_device)
        gtk_widget_get_size_request(g_c.gamepad_device, &local_device_width, NULL);
    t_ok("Local device row is compact without touching input slider width",
         GTK_IS_BOX(device_inline) && gtk_box_get_spacing(GTK_BOX(device_inline)) == 1 &&
         local_device_width == 190 && input_slider_width == 116);
    g_list_free(device_children);
    GtkWidget *input_preview_frame = g_c.input_stick_preview[0]
        ? gtk_widget_get_parent(g_c.input_stick_preview[0]) : NULL;
    GtkWidget *input_preview_pair = input_preview_frame
        ? gtk_widget_get_parent(input_preview_frame) : NULL;
    GtkWidget *input_preview_inline = input_preview_pair
        ? gtk_widget_get_parent(input_preview_pair) : NULL;
    GList *input_preview_children = GTK_IS_BOX(input_preview_inline)
        ? gtk_container_get_children(GTK_CONTAINER(input_preview_inline)) : NULL;
    t_ok("Input preview has no redundant caption",
         GTK_IS_BOX(input_preview_inline) &&
         g_list_length(input_preview_children) == 1);
    g_list_free(input_preview_children);
    GtkWidget *input_scale_grid = g_c.deadzone[0]
        ? gtk_widget_get_parent(g_c.deadzone[0]) : NULL;
    t_ok("Input slider rows have zero extra grid spacing",
         GTK_IS_GRID(input_scale_grid) &&
         gtk_grid_get_row_spacing(GTK_GRID(input_scale_grid)) == 0);
    GtkStyleContext *input_scale_style = g_c.deadzone[0]
        ? gtk_widget_get_style_context(g_c.deadzone[0]) : NULL;
    GtkWidget *input_scales[8] = {
        g_c.lt_threshold, g_c.deadzone[0], g_c.range[0], g_c.diagonal[0],
        g_c.rt_threshold, g_c.deadzone[1], g_c.range[1], g_c.diagonal[1]
    };
    GtkWidget *output_scales[8] = {
        g_c.output_lt_threshold, g_c.output_deadzone[0],
        g_c.output_range[0], g_c.output_diagonal[0],
        g_c.output_rt_threshold, g_c.output_deadzone[1],
        g_c.output_range[1], g_c.output_diagonal[1]
    };
    gboolean compact_scales = TRUE;
    for (int side = 0; side < 2; ++side) {
        GtkWidget **scales = side ? output_scales : input_scales;
        for (int i = 0; i < 8; ++i) {
            gint sw = -1, sh = -1;
            if (!GTK_IS_SCALE(scales[i])) { compact_scales = FALSE; continue; }
            gtk_widget_get_size_request(scales[i], &sw, &sh);
            compact_scales = compact_scales && sw == 116 && sh == 15 &&
                gtk_style_context_has_class(gtk_widget_get_style_context(scales[i]),
                                            "wozt-calibration-scale") &&
                gtk_widget_get_margin_top(scales[i]) == 0 &&
                gtk_widget_get_margin_bottom(scales[i]) == 0;
        }
    }
    t_ok("All sixteen sliders have compact height and scoped thin-track CSS",
         compact_scales && input_scale_style &&
         gtk_style_context_has_class(input_scale_style, "wozt-calibration-scale"));

    GtkWidget *input_sliders_grid = g_c.lt_threshold
        ? gtk_widget_get_parent(g_c.lt_threshold) : NULL;
    gint input_lt_row = -1, input_last_left = -1;
    gint input_rt_row = -1, input_last_right = -1;
    gint input_lt_col = -1, input_rt_col = -1;
    if (GTK_IS_GRID(input_sliders_grid) && g_c.rt_threshold &&
        g_c.diagonal[0] && g_c.diagonal[1]) {
        gtk_container_child_get(GTK_CONTAINER(input_sliders_grid), g_c.lt_threshold,
                                "top-attach", &input_lt_row,
                                "left-attach", &input_lt_col, NULL);
        gtk_container_child_get(GTK_CONTAINER(input_sliders_grid), g_c.diagonal[0],
                                "top-attach", &input_last_left, NULL);
        gtk_container_child_get(GTK_CONTAINER(input_sliders_grid), g_c.rt_threshold,
                                "top-attach", &input_rt_row,
                                "left-attach", &input_rt_col, NULL);
        gtk_container_child_get(GTK_CONTAINER(input_sliders_grid), g_c.diagonal[1],
                                "top-attach", &input_last_right, NULL);
    }
    t_ok("Input calibration has one slider column, left then right",
         GTK_IS_GRID(input_sliders_grid) && input_lt_col == 1 &&
         input_rt_col == 1 && input_lt_row >= 0 &&
         input_last_left == input_lt_row + 3 &&
         input_rt_row == input_lt_row + 4 &&
         input_last_right == input_lt_row + 7);
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
    t_eq_int("Right output calibration shares the same control column",
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
    gint input_preview_row = -1;
    if (GTK_IS_GRID(input_right_grid) && input_preview_inline)
        gtk_container_child_get(GTK_CONTAINER(input_right_grid), input_preview_inline,
                                "top-attach", &input_preview_row, NULL);
    t_ok("Input previews and calibration form one uninterrupted column",
         GTK_IS_GRID(input_right_grid) &&
         input_right_grid == g_c.wozt_inner_input &&
         gtk_widget_get_parent(device_inline) == input_right_grid &&
         gtk_widget_get_parent(input_preview_inline) == input_right_grid &&
         input_preview_row >= 0 && input_lt_row > input_preview_row);
    gint toggle_column = -1, toggle_span = -1;
    if (GTK_IS_GRID(input_right_grid) && g_c.gamepad_enabled)
        gtk_container_child_get(GTK_CONTAINER(input_right_grid),
                                g_c.gamepad_enabled, "left-attach", &toggle_column,
                                "width", &toggle_span, NULL);
    t_ok("Local input toggle starts at x=0 without an orphan label",
         g_c.gamepad_enabled && toggle_column == 0 && toggle_span == 2 &&
         gtk_widget_get_halign(g_c.gamepad_enabled) == GTK_ALIGN_START);
    /* WOZT_0851: Both headings stay readable while the input panel
     * gives back 5px. Keep the responsive row logic unchanged. */
    GtkWidget *input_source_heading = GTK_IS_GRID(input_right_grid)
        ? gtk_grid_get_child_at(GTK_GRID(input_right_grid), 0, 0) : NULL;
    gint invert_row = -1;
    if (GTK_IS_GRID(input_right_grid) && g_c.invert_ry)
        gtk_container_child_get(GTK_CONTAINER(input_right_grid),
                                g_c.invert_ry, "top-attach", &invert_row, NULL);
    GtkWidget *input_calibration_heading =
        GTK_IS_GRID(input_right_grid) && invert_row > 0
        ? gtk_grid_get_child_at(GTK_GRID(input_right_grid), 0, invert_row - 1)
        : NULL;
    t_ok("Local input uses the new source and calibration headings",
         GTK_IS_LABEL(input_source_heading) &&
         g_strcmp0(gtk_label_get_text(GTK_LABEL(input_source_heading)),
                   "Source controller") == 0 &&
         GTK_IS_LABEL(input_calibration_heading) &&
         g_strcmp0(gtk_label_get_text(GTK_LABEL(input_calibration_heading)),
                   "Input calibration and Input shaping") == 0);
    GtkWidget *input_outer = g_c.wozt_sections[3]
        ? gtk_bin_get_child(GTK_BIN(g_c.wozt_sections[3])) : NULL;
    t_ok("Local input trims its final right margin without changing sliders",
         GTK_IS_BOX(input_outer) &&
         gtk_widget_get_margin_end(input_outer) == 0 &&
         GTK_IS_GRID(input_right_grid) &&
         gtk_grid_get_column_spacing(GTK_GRID(input_right_grid)) == 0 &&
         input_slider_width == 116);
    GtkWidget *invert_text = GTK_IS_BIN(g_c.invert_ry)
        ? gtk_bin_get_child(GTK_BIN(g_c.invert_ry)) : NULL;
    PangoAttrList *invert_attrs = GTK_IS_LABEL(invert_text)
        ? gtk_label_get_attributes(GTK_LABEL(invert_text)) : NULL;
    gboolean input_invert_compact = FALSE;
    if (invert_attrs) {
        PangoAttrIterator *iter = pango_attr_list_get_iterator(invert_attrs);
        PangoAttribute *attr = pango_attr_iterator_get(iter, PANGO_ATTR_LETTER_SPACING);
        input_invert_compact = attr && ((PangoAttrInt *)attr)->value == -750;
        pango_attr_iterator_destroy(iter);
    }
    t_ok("Input invert control does not widen the calibration slider column",
         input_invert_compact && input_slider_width == 116);
    double px = -1.0, py = -1.0, radius = -1.0;
    wozt_stick_preview_geometry(70, 70, &px, &py, &radius);
    t_ok("70px preview has a centered visible 68px circle",
         px == 35.0 && py == 35.0 &&
         fabs(2.0 * (radius + 0.5) - 68.0) < 0.0001);
    t_ok("Preview frames add no shadow around the 70px drawing surface",
         input_preview_frame && GTK_IS_FRAME(input_preview_frame) &&
         gtk_frame_get_shadow_type(GTK_FRAME(input_preview_frame)) == GTK_SHADOW_NONE);
    wozt_dashboard_reflow(1650);
    /* The final frame expands, but the preceding one stays compact.
     * Both frames remain adjacent in their shared horizontal row. */
    t_ok("Rightmost Local input absorbs surplus in the shared controller row",
         g_c.wozt_sections[2] && !gtk_widget_get_hexpand(g_c.wozt_sections[2]) &&
         gtk_widget_get_halign(g_c.wozt_sections[2]) == GTK_ALIGN_START &&
         g_c.wozt_sections[3] && gtk_widget_get_hexpand(g_c.wozt_sections[3]) &&
         gtk_widget_get_halign(g_c.wozt_sections[3]) == GTK_ALIGN_FILL &&
         gtk_widget_get_parent(g_c.wozt_sections[2]) ==
             gtk_widget_get_parent(g_c.wozt_sections[3]));
    t_ok("Overview and Clients share an independent row at 1650px",
         gtk_widget_get_parent(g_c.wozt_sections[0]) ==
             gtk_widget_get_parent(g_c.wozt_sections[1]));
    t_ok("Controller panels share their responsive row at 1650px",
         gtk_widget_get_parent(g_c.wozt_sections[2]) ==
             gtk_widget_get_parent(g_c.wozt_sections[3]));
    t_ok("Four small sections occupy the same row at 1650px",
         gtk_widget_get_parent(g_c.wozt_sections[4]) ==
             gtk_widget_get_parent(g_c.wozt_sections[5]) &&
         gtk_widget_get_parent(g_c.wozt_sections[5]) ==
             gtk_widget_get_parent(g_c.wozt_sections[6]) &&
         gtk_widget_get_parent(g_c.wozt_sections[6]) ==
             gtk_widget_get_parent(g_c.wozt_sections[7]));
    t_ok("Maintenance alone expands at the end of the four-tool row",
         !gtk_widget_get_hexpand(g_c.wozt_sections[4]) &&
         !gtk_widget_get_hexpand(g_c.wozt_sections[5]) &&
         !gtk_widget_get_hexpand(g_c.wozt_sections[6]) &&
         gtk_widget_get_hexpand(g_c.wozt_sections[7]) &&
         gtk_widget_get_halign(g_c.wozt_sections[7]) == GTK_ALIGN_FILL);
    t_ok("The three functional rows have distinct parents",
         gtk_widget_get_parent(g_c.wozt_sections[0]) !=
             gtk_widget_get_parent(g_c.wozt_sections[2]) &&
         gtk_widget_get_parent(g_c.wozt_sections[2]) !=
             gtk_widget_get_parent(g_c.wozt_sections[4]));
    /* Verify actual GTK reparenting both below and at the packing
     * threshold; keep all widgets, callbacks and settings intact. */
    wozt_dashboard_reflow(controller_join_width - 1);
    t_ok("Controller frames have separate parents below the width budget",
         gtk_widget_get_parent(g_c.wozt_sections[2]) !=
             gtk_widget_get_parent(g_c.wozt_sections[3]));
    wozt_dashboard_reflow(controller_join_width);
    t_ok("Controller frames share a parent at the width budget",
         gtk_widget_get_parent(g_c.wozt_sections[2]) ==
             gtk_widget_get_parent(g_c.wozt_sections[3]));
    wozt_dashboard_reflow(820);
    t_ok("Overview and Clients share the first row at 820px",
         gtk_widget_get_parent(g_c.wozt_sections[0]) ==
             gtk_widget_get_parent(g_c.wozt_sections[1]));
    wozt_dashboard_reflow(720);
    t_ok("A wrapped Output frame fills its own line",
         g_c.wozt_sections[2] && gtk_widget_get_hexpand(g_c.wozt_sections[2]) &&
         gtk_widget_get_halign(g_c.wozt_sections[2]) == GTK_ALIGN_FILL);
    t_ok("A wrapped Local input frame fills its own line",
         g_c.wozt_sections[3] && gtk_widget_get_hexpand(g_c.wozt_sections[3]) &&
         gtk_widget_get_halign(g_c.wozt_sections[3]) == GTK_ALIGN_FILL);
    t_ok("A narrow window wraps the controller panels into separate rows",
         gtk_widget_get_parent(g_c.wozt_sections[2]) !=
             gtk_widget_get_parent(g_c.wozt_sections[3]));
    wozt_dashboard_reflow(1144);
    t_ok("Default width keeps Overview and Clients together",
         gtk_widget_get_parent(g_c.wozt_sections[0]) ==
             gtk_widget_get_parent(g_c.wozt_sections[1]));
    t_ok("At 1144px the last widget of each lower tool pair expands",
         wozt_dashboard_group_columns(1144, 2) == 2 &&
         !gtk_widget_get_hexpand(g_c.wozt_sections[4]) &&
         gtk_widget_get_hexpand(g_c.wozt_sections[5]) &&
         !gtk_widget_get_hexpand(g_c.wozt_sections[6]) &&
         gtk_widget_get_hexpand(g_c.wozt_sections[7]) &&
         gtk_widget_get_parent(g_c.wozt_sections[4]) ==
             gtk_widget_get_parent(g_c.wozt_sections[5]) &&
         gtk_widget_get_parent(g_c.wozt_sections[6]) ==
             gtk_widget_get_parent(g_c.wozt_sections[7]) &&
         gtk_widget_get_parent(g_c.wozt_sections[5]) !=
             gtk_widget_get_parent(g_c.wozt_sections[7]));
    /* Unlike the previous TRUE/TRUE GtkBox packing, Overview must not
     * take half of the available width. Clients absorbs all spare width. */
    GtkWidget *overview_line = gtk_widget_get_parent(g_c.wozt_sections[0]);
    gboolean overview_expand = TRUE, overview_fill = TRUE;
    if (GTK_IS_BOX(overview_line)) {
        guint overview_padding = 0;
        GtkPackType overview_pack_type = GTK_PACK_START;
        gtk_box_query_child_packing(GTK_BOX(overview_line), g_c.wozt_sections[0],
                                    &overview_expand, &overview_fill,
                                    &overview_padding, &overview_pack_type);
    }
    t_ok("Overview is packed at its natural width, without expansion",
         GTK_IS_BOX(overview_line) && !overview_expand && !overview_fill &&
         !gtk_widget_get_hexpand(g_c.wozt_sections[0]));
    wozt_dashboard_reflow(controller_join_width);
    GtkWidget *input_line = gtk_widget_get_parent(g_c.wozt_sections[3]);
    gboolean input_expand = TRUE, input_fill = TRUE;
    GtkPackType input_pack_type = GTK_PACK_END;
    if (GTK_IS_BOX(input_line)) {
        guint input_padding = 0;
        gtk_box_query_child_packing(GTK_BOX(input_line), g_c.wozt_sections[3],
                                    &input_expand, &input_fill,
                                    &input_padding, &input_pack_type);
    }
    t_ok("Local input is packed immediately after Output and fills the right edge",
         GTK_IS_BOX(input_line) && input_expand && input_fill &&
         input_pack_type == GTK_PACK_START &&
         gtk_widget_get_hexpand(g_c.wozt_sections[3]) &&
         gtk_widget_get_halign(g_c.wozt_sections[3]) == GTK_ALIGN_FILL &&
         gtk_box_get_spacing(GTK_BOX(input_line)) == 6);
    gboolean output_expand = TRUE, output_fill = TRUE;
    GtkPackType output_pack_type = GTK_PACK_END;
    if (GTK_IS_BOX(input_line)) {
        guint output_padding = 0;
        gtk_box_query_child_packing(GTK_BOX(input_line), g_c.wozt_sections[2],
                                    &output_expand, &output_fill,
                                    &output_padding, &output_pack_type);
    }
    t_ok("Output packs directly before Local input without expanding",
         GTK_IS_BOX(input_line) && !output_expand && !output_fill &&
         output_pack_type == GTK_PACK_START &&
         !gtk_widget_get_hexpand(g_c.wozt_sections[2]));
    t_ok("Overview server status wraps instead of widening the section",
         g_c.overview_status &&
         gtk_label_get_line_wrap(GTK_LABEL(g_c.overview_status)) &&
         gtk_label_get_max_width_chars(GTK_LABEL(g_c.overview_status)) == 34);
    t_ok("Overview controller output status keeps its compact width",
         g_c.backend_status &&
         gtk_label_get_line_wrap(GTK_LABEL(g_c.backend_status)) &&
         gtk_label_get_max_width_chars(GTK_LABEL(g_c.backend_status)) == 34);
    t_ok("Overview client statuses use compact natural widths",
         g_c.overview_client_status[GTK_SHELL_CLIENT_BROWSER] &&
         gtk_label_get_max_width_chars(GTK_LABEL(
             g_c.overview_client_status[GTK_SHELL_CLIENT_BROWSER])) == 34);
    GtkWidget *clients_line = gtk_widget_get_parent(g_c.wozt_sections[1]);
    gboolean clients_expand = FALSE, clients_fill = FALSE;
    if (GTK_IS_BOX(clients_line)) {
        guint clients_padding = 0;
        GtkPackType clients_pack_type = GTK_PACK_START;
        gtk_box_query_child_packing(GTK_BOX(clients_line), g_c.wozt_sections[1],
                                    &clients_expand, &clients_fill,
                                    &clients_padding, &clients_pack_type);
    }
    t_ok("Clients consumes the spare width in the first row",
         GTK_IS_BOX(clients_line) && clients_expand && clients_fill &&
         gtk_widget_get_hexpand(g_c.wozt_sections[1]));
    wozt_dashboard_reflow(gtk_widget_get_allocated_width(sh->settings_window));

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
    GtkWidget *logging_inline = g_c.pcble_verbose
        ? gtk_widget_get_parent(g_c.pcble_verbose) : NULL;
    GtkWidget *diagnostics_panel = logging_inline
        ? gtk_widget_get_parent(logging_inline) : NULL;
    t_ok("Right-column PCBLE diagnostics follows selected backend",
         GTK_IS_GRID(diagnostics_panel) && gtk_widget_get_visible(diagnostics_panel));
    GtkWidget *diagnostics_title = GTK_IS_GRID(diagnostics_panel)
        ? gtk_grid_get_child_at(GTK_GRID(diagnostics_panel), 0, 0) : NULL;
    t_ok("PCBLE Diagnostics heading is visible with its settings",
         GTK_IS_LABEL(diagnostics_title) &&
         gtk_widget_get_visible(diagnostics_title));
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
    GtkWidget *logging_inline = g_c.pcble_verbose
        ? gtk_widget_get_parent(g_c.pcble_verbose) : NULL;
    GtkWidget *diagnostics_panel = logging_inline
        ? gtk_widget_get_parent(logging_inline) : NULL;
    t_ok("PCBLE Diagnostics hides with the PCBLE backend",
         GTK_IS_GRID(diagnostics_panel) && !gtk_widget_get_visible(diagnostics_panel));

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
