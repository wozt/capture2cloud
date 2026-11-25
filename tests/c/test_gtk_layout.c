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

/* Layout regressions are about GTK's *computed* expansion, not only
 * GtkBox packing flags. Clients has vexpand=TRUE internally, so each
 * dashboard line must explicitly refuse inheriting it. */
static gboolean wozt_dashboard_rows_have_natural_heights(void)
{
    if (!GTK_IS_BOX(g_c.wozt_dashboard)) return FALSE;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(g_c.wozt_dashboard));
    gboolean valid = rows != NULL;
    for (GList *it = rows; it; it = it->next) {
        GtkWidget *line = GTK_WIDGET(it->data);
        valid = valid && GTK_IS_BOX(line) &&
            gtk_widget_get_valign(line) == GTK_ALIGN_START &&
            !gtk_widget_get_vexpand(line) &&
            !gtk_widget_compute_expand(line, GTK_ORIENTATION_VERTICAL);
    }
    g_list_free(rows);
    return valid;
}

/* WOZT_0868: size request AND cell requisition must be reduced; otherwise
 * GTK still demands the original 200px, and breaks the compact two-column
 * layout. Do not silently alter the Bluetooth adapter selector. */
static gboolean wozt_reset_selector_is_half_width(GtkWidget *widget)
{
    if (!GTK_IS_COMBO_BOX_TEXT(widget)) return FALSE;
    gint requested_width = -1;
    gtk_widget_get_size_request(widget, &requested_width, NULL);
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(widget));
    gboolean text_cell_compact = FALSE;
    for (GList *it = cells; it; it = it->next) {
        if (!GTK_IS_CELL_RENDERER_TEXT(it->data)) continue;
        gint cw = -1;
        gtk_cell_renderer_get_fixed_size(GTK_CELL_RENDERER(it->data), &cw, NULL);
        PangoEllipsizeMode ellipsize = PANGO_ELLIPSIZE_NONE;
        g_object_get(it->data, "ellipsize", &ellipsize, NULL);
        text_cell_compact = cw == 76 && ellipsize == PANGO_ELLIPSIZE_END;
    }
    g_list_free(cells);
    return requested_width == 100 && text_cell_compact &&
           !gtk_widget_get_hexpand(widget) &&
           gtk_widget_get_halign(widget) == GTK_ALIGN_START;
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
    t_ok("Clients retains its own seven-page notebook",
         g_c.wozt_clients_notebook &&
         gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_c.wozt_clients_notebook)) == 7);
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
    t_ok("Default Settings window matches the compact 918x844 screenshot",
         default_width == 918 && default_height == 844);
    /* gtk_window_get_geometry_hints() does not exist in GTK3. The setter
     * records the configured minimum as window data for this regression
     * probe; also verify no explicit width request prevents shrinking. */
    gint requested_width = -1;
    gtk_widget_get_size_request(sh->settings_window, &requested_width, NULL);
    const gint configured_minimum = GPOINTER_TO_INT(g_object_get_data(
        G_OBJECT(sh->settings_window), "wozt-minimum-width"));
    t_ok("The window permits shrinking to 600px",
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

    /* WOZT_0855: Overview's natural width, not a stale fixed budget,
     * determines the exact breakpoint with the scrollable Clients panel. */
    const int overview_join_width =
        wozt_section_layout_width(0) + wozt_section_layout_width(1) + 36;
    t_eq_int("Overview/Clients wrap just below their measured breakpoint",
             wozt_dashboard_group_columns(overview_join_width - 1, 0), 1);
    t_eq_int("Overview/Clients join at their measured breakpoint",
             wozt_dashboard_group_columns(overview_join_width, 0), 2);
    /* With Local monitor/Capture on the first row, there are three
     * logical items, not two. The stack remains two distinct frames. */
    t_eq_int("1144px: Overview, Clients and Monitor/Capture share first row",
             wozt_dashboard_group_columns(1144, 0), 3);
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
    gint overview_min = 0, overview_nat = 0;
    gtk_widget_get_preferred_width(g_c.wozt_sections[0],
                                   &overview_min, &overview_nat);
    t_ok("Overview reflow uses its measured natural GTK width",
         overview_nat > 0 &&
         wozt_section_layout_width(0) == MAX(overview_min, overview_nat));
    gint clients_minimum = 0, clients_natural = 0;
    gtk_widget_get_preferred_width(g_c.wozt_sections[1],
                                   &clients_minimum, &clients_natural);
    t_ok("Clients wraps according to its actual minimum, not 435px",
         wozt_section_layout_width(1) == MAX(240, clients_minimum) &&
         wozt_section_target_width[1] == 300);
    t_ok("Reset and Maintenance retain their width budgets",
         wozt_section_layout_width(6) == wozt_section_target_width[6] &&
         wozt_section_layout_width(7) == wozt_section_target_width[7]);
    t_ok("Reset joins the stack only if all three panels fit top-row height",
         !wozt_reset_fits_heights(235, 220, 145, 53, 180) &&
         wozt_reset_fits_heights(420, 400, 145, 53, 180) &&
         !wozt_reset_fits_heights(0, 400, 145, 53, 180));
    const gboolean reset_top = wozt_reset_fits_top();
    const int monitor_capture_width = MAX(
        MAX(wozt_section_layout_width(4), wozt_section_layout_width(5)),
        reset_top ? wozt_section_layout_width(6) : 0);
    const int top_three_width = wozt_section_layout_width(0) +
        wozt_section_layout_width(1) + monitor_capture_width + 42;
    t_eq_int("Monitor/Capture wraps just before the first row fits three items",
             wozt_dashboard_group_columns(top_three_width - 1, 0), 2);
    t_eq_int("Overview, Clients and compact Monitor/Capture fit at the breakpoint",
             wozt_dashboard_group_columns(top_three_width, 0), 3);
    t_eq_int("1650px: the compact stack is on the top row",
             wozt_dashboard_group_columns(1650, 0), 3);
    /* WOZT_0867_RESET_MAINT_TESTS: GTK's homogeneous 3-track minimum,
     * not just the sum of the two old fixed packing budgets. */
    const int tools_two_width = wozt_utility_pair_min_width() + 30;
    if (!reset_top) {
        t_eq_int("Reset and Maintenance wrap before colliding",
                 wozt_dashboard_group_columns(tools_two_width - 1, 2), 1);
        t_eq_int("Reset and Maintenance join when both fit",
                 wozt_dashboard_group_columns(tools_two_width, 2), 2);
    } else {
        t_eq_int("Only Maintenance stays in bottom group when Reset fits above",
                 wozt_dashboard_group_columns(tools_two_width, 2), 1);
    }
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
    t_eq_int("Reset/Maintenance group always has two slots at wide widths",
             wozt_dashboard_group_columns(2200, 2), 2);
    t_ok("Three first-row slots and two bottom-row slots are configured",
         wozt_group_size[0] == 3 && wozt_group_size[2] == 2 &&
         wozt_group_first[2] == 6 &&
         wozt_dashboard_group_columns(1650, 0) == 3);
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
    for (int ci = 0; ci < 7; ++ci) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_c.wozt_clients_notebook), ci);
        GtkWidget *tab = gtk_notebook_get_tab_label(GTK_NOTEBOOK(g_c.wozt_clients_notebook), page);
        GtkWidget *vp = GTK_IS_SCROLLED_WINDOW(page) ? gtk_bin_get_child(GTK_BIN(page)) : NULL;
        GtkWidget *inner = GTK_IS_VIEWPORT(vp) ? gtk_bin_get_child(GTK_BIN(vp)) : vp;
        client_tabs_are_icons = client_tabs_are_icons && GTK_IS_BOX(tab) &&
            gtk_widget_get_tooltip_text(tab) != NULL;
        pages_are_dense = pages_are_dense && GTK_IS_GRID(inner) &&
            gtk_grid_get_row_spacing(GTK_GRID(inner)) == 1 &&
            gtk_grid_get_column_spacing(GTK_GRID(inner)) == 3 &&
            gtk_widget_get_valign(inner) == GTK_ALIGN_START &&
            gtk_container_get_border_width(GTK_CONTAINER(inner)) == 1;
    }
    t_ok("Seven distinct Client tabs show SVG icons with accessible names",
         client_tabs_are_icons &&
         gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_c.wozt_clients_notebook)) == 7);
    t_ok("All seven Client pages use compact top-aligned 1px padding", pages_are_dense);
    GtkWidget *client_grid = g_c.wozt_clients_notebook
        ? gtk_widget_get_parent(g_c.wozt_clients_notebook) : NULL;
    GtkWidget *client_outer = GTK_IS_GRID(client_grid)
        ? gtk_widget_get_parent(client_grid) : NULL;
    gboolean grid_expand = FALSE, grid_fill = FALSE;
    if (GTK_IS_BOX(client_outer)) {
        guint padding = 0;
        GtkPackType pack = GTK_PACK_START;
        gtk_box_query_child_packing(GTK_BOX(client_outer), client_grid,
                                    &grid_expand, &grid_fill, &padding, &pack);
    }
    t_ok("Clients notebook fills its section height with 1px top/bottom margins",
         GTK_IS_BOX(client_outer) && GTK_IS_GRID(client_grid) &&
         gtk_widget_get_margin_top(client_outer) == 1 &&
         gtk_widget_get_margin_bottom(client_outer) == 1 &&
         gtk_widget_get_vexpand(client_outer) &&
         gtk_widget_get_vexpand(client_grid) &&
         gtk_widget_get_vexpand(g_c.wozt_clients_notebook) &&
         grid_expand && grid_fill);
    t_ok("Old 3DS has a real toggle separate from Switch",
         g_c.old3ds_enabled && GTK_IS_CHECK_BUTTON(g_c.old3ds_enabled) &&
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_c.old3ds_enabled)) &&
         g_c.old3ds_enabled != g_c.switch_enabled);
    t_ok("Client ports use one-frame GTK style and compact requisitions",
         g_c.port && g_c.switch_port && g_c.wiiu_pad_port &&
         g_c.wiiu_console_port && g_c.old3ds_port &&
         gtk_style_context_has_class(gtk_widget_get_style_context(g_c.port),
                                     "wozt-client-port") &&
         gtk_style_context_has_class(gtk_widget_get_style_context(g_c.old3ds_port),
                                     "wozt-client-port"));
    /* Switch, Android, radio bridge and WUP each own a separate page.
     * Existing controls/signals remain in their original mode. */
    GtkWidget *radio_page = gtk_notebook_get_nth_page(
        GTK_NOTEBOOK(g_c.wozt_clients_notebook), 3);
    GtkWidget *wup_page = gtk_notebook_get_nth_page(
        GTK_NOTEBOOK(g_c.wozt_clients_notebook), 4);
    GtkWidget *radio_view = GTK_IS_SCROLLED_WINDOW(radio_page)
        ? gtk_bin_get_child(GTK_BIN(radio_page)) : NULL;
    GtkWidget *radio_grid = GTK_IS_VIEWPORT(radio_view)
        ? gtk_bin_get_child(GTK_BIN(radio_view)) : radio_view;
    GtkWidget *wup_view = GTK_IS_SCROLLED_WINDOW(wup_page)
        ? gtk_bin_get_child(GTK_BIN(wup_page)) : NULL;
    GtkWidget *wup_grid = GTK_IS_VIEWPORT(wup_view)
        ? gtk_bin_get_child(GTK_BIN(wup_view)) : wup_view;
    t_ok("Direct radio and WUP have separate GtkNotebook pages",
         GTK_IS_GRID(radio_grid) && GTK_IS_GRID(wup_grid) && radio_grid != wup_grid &&
         gtk_widget_get_parent(g_c.wiiu_pad_port) == radio_grid &&
         gtk_widget_get_parent(g_c.wup_touch_mode) == wup_grid &&
         gtk_widget_get_parent(g_c.wup_metrics_label) == wup_grid);
    t_ok("Android has its own status and Overview line",
         g_c.client_status[GTK_SHELL_CLIENT_ANDROID] &&
         g_c.overview_client_status[GTK_SHELL_CLIENT_ANDROID] &&
         g_c.client_status[GTK_SHELL_CLIENT_ANDROID] !=
             g_c.client_status[GTK_SHELL_CLIENT_NATIVE]);
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
    t_ok("Wide dashboard rows never inherit Clients vertical expansion",
         wozt_dashboard_rows_have_natural_heights());
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
    /* WOZT_0862_MONITOR_CAPTURE_STACK: two framed panels, one column. */
    GtkWidget *monitor_stack = gtk_widget_get_parent(g_c.wozt_sections[5]);
    GtkWidget *tool_line = monitor_stack ? gtk_widget_get_parent(monitor_stack) : NULL;
    GList *monitor_stack_children = GTK_IS_BOX(monitor_stack)
        ? gtk_container_get_children(GTK_CONTAINER(monitor_stack)) : NULL;
    t_ok("Local monitor is ABOVE Capture in two separate vertical frames",
         GTK_IS_BOX(monitor_stack) &&
         gtk_orientable_get_orientation(GTK_ORIENTABLE(monitor_stack)) ==
             GTK_ORIENTATION_VERTICAL &&
         gtk_box_get_spacing(GTK_BOX(monitor_stack)) == 5 &&
         gtk_widget_get_parent(g_c.wozt_sections[4]) == monitor_stack &&
         g_list_length(monitor_stack_children) == 2 &&
         monitor_stack_children->data == g_c.wozt_sections[5] &&
         monitor_stack_children->next->data == g_c.wozt_sections[4]);
    g_list_free(monitor_stack_children);
    gboolean monitor_expand = FALSE, monitor_fill = FALSE;
    gboolean capture_expand = FALSE, capture_fill = FALSE;
    if (GTK_IS_BOX(monitor_stack)) {
        gtk_box_query_child_packing(GTK_BOX(monitor_stack), g_c.wozt_sections[5],
                                    &monitor_expand, &monitor_fill, NULL, NULL);
        gtk_box_query_child_packing(GTK_BOX(monitor_stack), g_c.wozt_sections[4],
                                    &capture_expand, &capture_fill, NULL, NULL);
    }
    t_ok("Monitor fills only available first-row height; Capture remains compact",
         GTK_IS_BOX(monitor_stack) &&
         gtk_widget_get_valign(monitor_stack) == GTK_ALIGN_FILL &&
         !gtk_widget_get_vexpand(monitor_stack) &&
         gtk_widget_get_vexpand(g_c.wozt_sections[5]) &&
         !gtk_widget_get_vexpand(g_c.wozt_sections[4]) &&
         monitor_expand && monitor_fill && !capture_expand && capture_fill);
    t_ok("Compact stack sits beside Clients on first row at wide widths",
         GTK_IS_BOX(tool_line) &&
         gtk_orientable_get_orientation(GTK_ORIENTABLE(tool_line)) ==
             GTK_ORIENTATION_HORIZONTAL &&
         gtk_widget_get_parent(g_c.wozt_sections[0]) == tool_line &&
         gtk_widget_get_parent(g_c.wozt_sections[1]) == tool_line &&
         gtk_widget_get_hexpand(g_c.wozt_sections[1]) &&
         !gtk_widget_get_hexpand(monitor_stack));
    GtkWidget *reset_parent = gtk_widget_get_parent(g_c.wozt_sections[6]);
    GtkWidget *maintenance_parent = gtk_widget_get_parent(g_c.wozt_sections[7]);
    GtkWidget *bottom_row = GTK_IS_GRID(reset_parent)
        ? gtk_widget_get_parent(reset_parent) : maintenance_parent;
    gint reset_x = -1, reset_span = -1, maint_x = -1, maint_span = -1;
    if (GTK_IS_GRID(reset_parent) && reset_parent == maintenance_parent) {
        gtk_container_child_get(GTK_CONTAINER(reset_parent), g_c.wozt_sections[6],
                                "left-attach", &reset_x, "width", &reset_span, NULL);
        gtk_container_child_get(GTK_CONTAINER(reset_parent), g_c.wozt_sections[7],
                                "left-attach", &maint_x, "width", &maint_span, NULL);
    }
    t_ok("Reset spans two thirds and Maintenance one third when sharing a row",
         reset_top
         ? (reset_parent == monitor_stack && maintenance_parent != tool_line)
         : (GTK_IS_GRID(reset_parent) && reset_parent == maintenance_parent &&
            GTK_IS_BOX(bottom_row) && bottom_row != tool_line &&
            gtk_grid_get_column_homogeneous(GTK_GRID(reset_parent)) &&
            gtk_grid_get_column_spacing(GTK_GRID(reset_parent)) == 6 &&
            reset_x == 0 && reset_span == 2 && maint_x == 2 && maint_span == 1));
    gint status_left = -1, status_width = -1;
    GtkWidget *status_grid = g_c.reset_bt_status
        ? gtk_widget_get_parent(g_c.reset_bt_status) : NULL;
    if (GTK_IS_GRID(status_grid)) {
        gtk_container_child_get(GTK_CONTAINER(status_grid),
                                g_c.reset_bt_status,
                                "left-attach", &status_left,
                                "width", &status_width, NULL);
    }
    t_ok("Bluetooth wake status is a single full-width, tooltip-backed line",
         GTK_IS_GRID(status_grid) && GTK_IS_LABEL(g_c.reset_bt_status) &&
         status_left == 0 &&
         status_width == 2 &&
         !gtk_label_get_line_wrap(GTK_LABEL(g_c.reset_bt_status)) &&
         gtk_label_get_single_line_mode(GTK_LABEL(g_c.reset_bt_status)) &&
         gtk_label_get_ellipsize(GTK_LABEL(g_c.reset_bt_status)) == PANGO_ELLIPSIZE_END &&
         gtk_widget_get_tooltip_text(g_c.reset_bt_status) != NULL);
    GtkWidget *method_bar = g_c.reset_method
        ? gtk_widget_get_parent(g_c.reset_method) : NULL;
    GList *method_children = GTK_IS_BOX(method_bar)
        ? gtk_container_get_children(GTK_CONTAINER(method_bar)) : NULL;
    t_ok("Wake method title and selector share a single compact line",
         GTK_IS_BOX(method_bar) && g_list_length(method_children) == 2 &&
         GTK_IS_LABEL(method_children->data) &&
         method_children->next->data == g_c.reset_method &&
         GTK_IS_GRID(gtk_widget_get_parent(method_bar)));
    g_list_free(method_children);
    t_ok("Wake method combo is half the previous 200px without GTK cell overflow",
         wozt_reset_selector_is_half_width(g_c.reset_method));
    t_ok("Bluetooth target combo is half width without affecting adapter",
         wozt_reset_selector_is_half_width(g_c.reset_bt_target));
    GtkWidget *reset_flow = GTK_IS_GRID(g_c.reset_bt_panel)
        ? gtk_grid_get_child_at(GTK_GRID(g_c.reset_bt_panel), 0, 0) : NULL;
    GList *flow_children = GTK_IS_FLOW_BOX(reset_flow)
        ? gtk_container_get_children(GTK_CONTAINER(reset_flow)) : NULL;
    t_ok("Bluetooth reset layout has two independently flowing columns",
         GTK_IS_FLOW_BOX(reset_flow) && g_list_length(flow_children) == 2 &&
         gtk_flow_box_get_min_children_per_line(GTK_FLOW_BOX(reset_flow)) == 1 &&
         gtk_flow_box_get_max_children_per_line(GTK_FLOW_BOX(reset_flow)) == 2 &&
         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g_c.reset_bt_panel),
                                          "wozt-reset-two-columns")) == 1);
    GtkWidget *left_flow_grid = flow_children && GTK_IS_BIN(flow_children->data)
        ? gtk_bin_get_child(GTK_BIN(flow_children->data)) : NULL;
    GtkWidget *right_flow_grid = flow_children && flow_children->next &&
                                 GTK_IS_BIN(flow_children->next->data)
        ? gtk_bin_get_child(GTK_BIN(flow_children->next->data)) : NULL;
    t_ok("Target console stays left; Bluetooth adapter starts the right column",
         GTK_IS_GRID(left_flow_grid) && GTK_IS_GRID(right_flow_grid) &&
         gtk_widget_get_parent(g_c.reset_bt_target) == left_flow_grid &&
         gtk_widget_get_parent(g_c.reset_bt_adapter) == right_flow_grid &&
         gtk_widget_get_parent(g_c.reset_bt_status) == right_flow_grid);
    g_list_free(flow_children);
    GtkWidget *maintenance_grid = g_c.backend_recovery
        ? gtk_widget_get_parent(g_c.backend_recovery) : NULL;
    t_ok("Maintenance rows have compact consistent spacing",
         GTK_IS_GRID(maintenance_grid) &&
         gtk_grid_get_row_spacing(GTK_GRID(maintenance_grid)) == 1 &&
         gtk_grid_get_column_spacing(GTK_GRID(maintenance_grid)) == 4);
    gint monitor_scale_width = -1, monitor_scale_height = -1;
    gtk_widget_get_size_request(g_c.volume, &monitor_scale_width, &monitor_scale_height);
    t_ok("Monitor sliders use the thin compact 116x15 calibration style",
         monitor_scale_width == 116 && monitor_scale_height == 15 &&
         gtk_style_context_has_class(gtk_widget_get_style_context(g_c.volume),
                                     "wozt-calibration-scale") &&
         gtk_style_context_has_class(gtk_widget_get_style_context(g_c.brightness),
                                     "wozt-calibration-scale") &&
         gtk_style_context_has_class(gtk_widget_get_style_context(g_c.contrast),
                                     "wozt-calibration-scale"));
    t_ok("The three functional groups remain independent",
         gtk_widget_get_parent(g_c.wozt_sections[0]) !=
             gtk_widget_get_parent(g_c.wozt_sections[2]) &&
         gtk_widget_get_parent(g_c.wozt_sections[2]) !=
             gtk_widget_get_parent(g_c.wozt_sections[6]));
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
    wozt_dashboard_reflow(MAX(820, overview_join_width));
    t_ok("Overview and Clients share their row at the measured breakpoint",
         gtk_widget_get_parent(g_c.wozt_sections[0]) ==
             gtk_widget_get_parent(g_c.wozt_sections[1]));
    wozt_dashboard_reflow(720);
    t_ok("Narrow stacked dashboard rows never expand vertically",
         wozt_dashboard_rows_have_natural_heights());
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
    t_ok("Default dashboard rows have natural, top-aligned heights",
         wozt_dashboard_rows_have_natural_heights());
    t_ok("Default width keeps Overview and Clients together",
         gtk_widget_get_parent(g_c.wozt_sections[0]) ==
             gtk_widget_get_parent(g_c.wozt_sections[1]));
    GtkWidget *monitor_stack_default =
        gtk_widget_get_parent(g_c.wozt_sections[5]);
    GtkWidget *default_top_line = monitor_stack_default
        ? gtk_widget_get_parent(monitor_stack_default) : NULL;
    t_ok("Monitor and Capture stay stacked on first row at default width",
         wozt_dashboard_group_columns(1144, 0) == 3 &&
         GTK_IS_BOX(monitor_stack_default) &&
         gtk_orientable_get_orientation(GTK_ORIENTABLE(monitor_stack_default)) ==
             GTK_ORIENTATION_VERTICAL &&
         gtk_widget_get_parent(g_c.wozt_sections[4]) == monitor_stack_default &&
         default_top_line == gtk_widget_get_parent(g_c.wozt_sections[0]) &&
         default_top_line == gtk_widget_get_parent(g_c.wozt_sections[1]) &&
         (wozt_reset_fits_top()
          ? gtk_widget_get_parent(g_c.wozt_sections[6]) == monitor_stack_default
          : gtk_widget_get_parent(g_c.wozt_sections[6]) != monitor_stack_default));
    wozt_dashboard_reflow(top_three_width - 1);
    GtkWidget *narrow_stack = gtk_widget_get_parent(g_c.wozt_sections[5]);
    t_ok("Monitor/Capture remains one vertical block when top row wraps",
         GTK_IS_BOX(narrow_stack) &&
         gtk_widget_get_parent(g_c.wozt_sections[4]) == narrow_stack &&
         gtk_widget_get_parent(narrow_stack) !=
             gtk_widget_get_parent(g_c.wozt_sections[1]) &&
         gtk_box_get_spacing(GTK_BOX(narrow_stack)) == 5 &&
         (wozt_reset_fits_top()
          ? gtk_widget_get_parent(g_c.wozt_sections[6]) == narrow_stack
          : gtk_widget_get_parent(g_c.wozt_sections[6]) != narrow_stack));
    wozt_dashboard_reflow(1144);
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
    t_ok("Overview client statuses stay single-line, with shortened idle text",
         g_c.overview_client_status[GTK_SHELL_CLIENT_BROWSER] &&
         gtk_label_get_max_width_chars(GTK_LABEL(
             g_c.overview_client_status[GTK_SHELL_CLIENT_BROWSER])) == 34 &&
         !gtk_label_get_line_wrap(GTK_LABEL(
             g_c.overview_client_status[GTK_SHELL_CLIENT_BROWSER])) &&
         gtk_label_get_ellipsize(GTK_LABEL(
             g_c.overview_client_status[GTK_SHELL_CLIENT_BROWSER])) == PANGO_ELLIPSIZE_END &&
         g_strcmp0(wozt_overview_client_summary(
             "nothing connected — not encoding"), "idle — no-enc") == 0 &&
         g_strcmp0(wozt_overview_client_summary("2 connected — 60 FPS"),
                   "2 connected — 60 FPS") == 0);

    /* WOZT_0859_SERVER_ICONS: two legacy full-text labels remain accessible
     * to the host but are HIDDEN; the only visible Server row holds exactly
     * four images and three one-character dash separators. */
    GtkWidget *overview_grid = g_c.overview_status
        ? gtk_widget_get_parent(g_c.overview_status) : NULL;
    gint indicators_x = -1, indicators_span = -1;
    if (GTK_IS_GRID(overview_grid) && g_c.overview_indicator_bar)
        gtk_container_child_get(GTK_CONTAINER(overview_grid),
            g_c.overview_indicator_bar, "left-attach", &indicators_x,
            "width", &indicators_span, NULL);
    GList *indicator_children = GTK_IS_BOX(g_c.overview_indicator_bar)
        ? gtk_container_get_children(GTK_CONTAINER(g_c.overview_indicator_bar)) : NULL;
    gboolean four_indicators = g_list_length(indicator_children) == 7;
    for (int i = 0; i < 4 && four_indicators; ++i) {
        GtkWidget *item = GTK_WIDGET(g_list_nth_data(indicator_children, 2 * i));
        gchar *tip = GTK_IS_IMAGE(item) ? gtk_widget_get_tooltip_text(item) : NULL;
        four_indicators = GTK_IS_IMAGE(item) &&
                          item == g_c.overview_indicators[i] &&
                          tip && *tip;
        g_free(tip);
        if (i < 3) {
            GtkWidget *dash = GTK_WIDGET(g_list_nth_data(indicator_children, 2 * i + 1));
            four_indicators = four_indicators && GTK_IS_LABEL(dash) &&
                              g_strcmp0(gtk_label_get_text(GTK_LABEL(dash)), "-") == 0;
        }
    }
    g_list_free(indicator_children);
    t_ok("Overview Server is a single line of four live icons and three dashes",
         GTK_IS_GRID(overview_grid) && four_indicators && indicators_x == 0 &&
         indicators_span == 2 &&
         gtk_widget_get_no_show_all(g_c.overview_status) &&
         gtk_widget_get_no_show_all(g_c.backend_status) &&
         !gtk_widget_get_visible(g_c.overview_status) &&
         !gtk_widget_get_visible(g_c.backend_status));

    /* Each line is its OWN GtkBox, so no spanning Server label or Quick
     * actions can create a 100px blank column between icon and status. */
    gboolean compact_icons = GTK_IS_GRID(overview_grid);
    GtkWidget *client_list = NULL;
    for (int client = 0; client < GTK_SHELL_CLIENT_COUNT && compact_icons; ++client) {
        GtkWidget *status = g_c.overview_client_status[client];
        GtkWidget *line = status ? gtk_widget_get_parent(status) : NULL;
        GtkWidget *list = line ? gtk_widget_get_parent(line) : NULL;
        GList *children = GTK_IS_BOX(line)
            ? gtk_container_get_children(GTK_CONTAINER(line)) : NULL;
        GtkWidget *icon = children ? GTK_WIDGET(children->data) : NULL;
        GtkWidget *text = children && children->next
            ? GTK_WIDGET(children->next->data) : NULL;
        compact_icons = GTK_IS_BOX(line) && GTK_IS_BOX(list) &&
            gtk_box_get_spacing(GTK_BOX(line)) == 3 &&
            g_list_length(children) == 2 && GTK_IS_BOX(icon) && text == status &&
            (!client_list || list == client_list);
        g_list_free(children);
        if (!client_list) client_list = list;
    }
    t_ok("Overview SVG icons are followed immediately by client statuses",
         compact_icons && GTK_IS_BOX(client_list) &&
         gtk_widget_get_parent(client_list) == overview_grid &&
         gtk_box_get_spacing(GTK_BOX(client_list)) == 0);

    /* WOZT_0857_OVERVIEW_DYNAMIC_ACTIONS: Client rows increased from 5 to 7.
     * Resolve the actual Quick actions row by its two real button widgets,
     * rather than assuming the row number stays 10 after client changes. */
    GtkWidget *overview_actions = NULL;
    GList *action_buttons = NULL;
    gint actions_left = -1, actions_top = -1, actions_span = -1;
    if (GTK_IS_GRID(overview_grid)) {
        GList *overview_children = gtk_container_get_children(GTK_CONTAINER(overview_grid));
        for (GList *it = overview_children; it; it = it->next) {
            GtkWidget *candidate = GTK_WIDGET(it->data);
            if (!GTK_IS_BOX(candidate)) continue;
            GList *buttons = gtk_container_get_children(GTK_CONTAINER(candidate));
            GtkWidget *first = buttons ? GTK_WIDGET(buttons->data) : NULL;
            GtkWidget *second = buttons && buttons->next
                ? GTK_WIDGET(buttons->next->data) : NULL;
            const gboolean quick_actions = g_list_length(buttons) == 2 &&
                GTK_IS_BUTTON(first) && GTK_IS_BUTTON(second) &&
                gtk_style_context_has_class(gtk_widget_get_style_context(first),
                                            "wozt-overview-action-fulltext") &&
                gtk_style_context_has_class(gtk_widget_get_style_context(second),
                                            "wozt-overview-action-fulltext");
            if (quick_actions) {
                overview_actions = candidate;
                action_buttons = buttons;
                gtk_container_child_get(GTK_CONTAINER(overview_grid), candidate,
                                        "left-attach", &actions_left,
                                        "top-attach", &actions_top,
                                        "width", &actions_span, NULL);
                break;
            }
            g_list_free(buttons);
        }
        g_list_free(overview_children);
    }
    GtkWidget *capture_button = action_buttons ? GTK_WIDGET(action_buttons->data) : NULL;
    GtkWidget *wake_button = action_buttons && action_buttons->next
        ? GTK_WIDGET(action_buttons->next->data) : NULL;
    GtkWidget *capture_text = GTK_IS_BIN(capture_button)
        ? gtk_bin_get_child(GTK_BIN(capture_button)) : NULL;
    t_ok("Overview Quick actions are full-width-grid children aligned left",
         GTK_IS_BOX(overview_actions) && g_list_length(action_buttons) == 2 &&
         gtk_widget_get_halign(overview_actions) == GTK_ALIGN_START &&
         gtk_box_get_spacing(GTK_BOX(overview_actions)) == 4 &&
         actions_left == 0 && actions_span == 2 && actions_top >= 0 &&
         gtk_grid_get_child_at(GTK_GRID(overview_grid), 1, actions_top) == overview_actions &&
         GTK_IS_BUTTON(capture_button) && GTK_IS_BUTTON(wake_button));
    t_ok("Show capture window cannot be ellipsized",
         GTK_IS_LABEL(capture_text) &&
         gtk_label_get_ellipsize(GTK_LABEL(capture_text)) == PANGO_ELLIPSIZE_NONE &&
         gtk_label_get_max_width_chars(GTK_LABEL(capture_text)) == -1);
    g_list_free(action_buttons);
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
    if (g_c.old3ds_enabled) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.old3ds_enabled), FALSE);
        SDL_LockMutex(sh->lock);
        const int old3ds_off = sh->settings.old3ds_enabled;
        SDL_UnlockMutex(sh->lock);
        t_eq_int("Old3DS toggle updates server settings", old3ds_off, 0);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.old3ds_enabled), TRUE);
    }

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
