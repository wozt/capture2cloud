#include "gtk_shell.h"

#include "app_config.h"
#include "gamepad_bridge.h" /* the output-protocol names shown in the combo */
#include "output_pcble.h"
#include "reset_method.h"
#include "version.h"
#include "wozt_ui_standard.h"

#include <SDL2/SDL.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
 * One GTK thread with its own loop, as before. What it puts on screen is
 * different: a status icon rather than a window that had to be kept
 * glued to another one.
 *
 * The tray icon is a GtkStatusIcon, which GTK has deprecated. It is
 * still the right choice here: the modern replacement is an
 * AppIndicator library that is not installed on this machine, and KDE
 * bridges XEmbed icons into its own tray through xembedsniproxy, which
 * is running. A deprecated call that works beats a dependency that is
 * absent.
 *
 * The deprecation warnings are silenced for this file alone, and only
 * around the calls themselves: turning them off project-wide would hide
 * the next deprecation, which might be one worth reading.
 */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

struct GtkShell {
    SDL_Thread *thread;
    GtkStatusIcon *icon;
    GtkWidget *settings_window;
    WoztUiStandard *wozt;
    GtkShellCallbacks callbacks;

    /* The settings, and the lock around them. The GTK thread writes them
     * when a control moves; the program writes them when it refuses
     * something or corrects it. */
    SDL_mutex *lock;
    AppSettings settings;
    char status[160];
    char wiiu_status_text[160];
    volatile int wiiu_status_dirty;
    char wup_metrics_text[240];
    volatile int wup_metrics_dirty;
    char client_status_text[GTK_SHELL_CLIENT_COUNT][160];
    volatile int client_status_dirty;

    /* Set while the code is filling the controls in from `settings`, so
     * the "value changed" handlers do not report those as the user
     * moving something -- which would echo straight back and, with two
     * threads, could ping-pong. */
    int loading;

    /* The tray menu, kept so the previous one can be destroyed. It is
     * rebuilt on every right click because it shows a checkbox whose
     * state may have changed since the last one. */
    GtkWidget *menu;

    /* The controllers the program can see. Copied in under the lock and
     * rebuilt into the list on the GTK thread. */
    char controllers[8][96];
    int controller_count;

    volatile int running;
    volatile int settings_dirty; /* the program changed something */
    volatile int status_dirty;
    volatile int controllers_dirty;
};

/* Every control that shows a value, so refreshing is a loop rather than
 * a list of assignments repeated in three places. */
typedef struct {
    GtkWidget *stream_enabled, *port, *switch_enabled, *switch_port, *resolution, *bitrate, *capture_format;
    GtkWidget *wiiu_pad_enabled, *wiiu_pad_port, *wiiu_pad_bitrate;
    GtkWidget *wup_stream_enabled, *wup_touch_mode, *wup_metrics_label;
    GtkWidget *wiiu_console_enabled, *wiiu_console_port;
    GtkWidget *wiiu_console_resolution, *wiiu_console_bitrate;
    GtkWidget *old3ds_enabled, *old3ds_port; /* WOZT_0860_OLD3DS_ENABLED */
    /* One line per client family, so "nothing is watching" can be told
     * apart from "something is watching and the picture is wrong"
     * without reading a terminal. Indexed by GtkShellClient. */
    GtkWidget *client_status[GTK_SHELL_CLIENT_COUNT];
    GtkWidget *overview_client_status[GTK_SHELL_CLIENT_COUNT];

    GtkWidget *gamepad_enabled, *gamepad_device, *invert_ry, *output_protocol;
    GtkWidget *output_backend, *backend_status, *titan_settings;
    GtkWidget *adapter_sees;
    GtkWidget *backend_recovery;

    GtkWidget *pcble_settings;
    GtkWidget *pcble_controller;
    GtkWidget *pcble_mouse_side;
    GtkWidget *pcble_primary;
    GtkWidget *pcble_secondary;
    GtkWidget *pcble_paired;
    GtkWidget *pcble_status;
    GtkWidget *pcble_pair;
    GtkWidget *pcble_reconnect;
    GtkWidget *pcble_stop;
    GtkWidget *pcble_refresh;
    GtkWidget *pcble_body_color;
    GtkWidget *pcble_button_color;
    GtkWidget *pcble_left_grip_color;
    GtkWidget *pcble_right_grip_color;
    GtkWidget *pcble_verbose;

    OutputPcbleAdapter pcble_adapters[OUTPUT_PCBLE_MAX_ADAPTERS];
    int pcble_adapter_count;
    char pcble_paired_switch[18];
    char pcble_paired_adapter[18];

    GtkWidget *overview_status;
    /* Exactly four small live server indicators, independent of the hidden
     * full-length labels kept for the footer, diagnostics and tests. */
    GtkWidget *overview_indicators[4];
    GtkWidget *overview_indicator_bar;

    /* Console wake/reset method. */
    GtkWidget *reset_method;
    GtkWidget *reset_script_panel;
    GtkWidget *reset_script;
    GtkWidget *reset_bt_panel;
    GtkWidget *reset_bt_target;
    GtkWidget *reset_bt_adapter;
    GtkWidget *reset_bt_refresh;
    GtkWidget *reset_bt_test_adapter;
    GtkWidget *reset_bt_capture;
    GtkWidget *reset_bt_test_beacon;
    GtkWidget *reset_bt_status;

    ResetBluetoothAdapter
        reset_bt_adapters[RESET_METHOD_MAX_BT_ADAPTERS];
    int reset_bt_adapter_count;

    /* Local SDL input calibration. */
    GtkWidget *input_stick_preview[2];
    GtkWidget *lt_threshold, *rt_threshold;
    GtkWidget *deadzone[2], *range[2], *diagonal[2];

    /* Final merged-state calibration. */
    GtkWidget *output_stick_preview[2];
    GtkWidget *output_invert_ry;
    GtkWidget *output_lt_threshold, *output_rt_threshold;
    GtkWidget *output_deadzone[2], *output_range[2], *output_diagonal[2];
    GtkWidget *muted, *volume, *direct_sink, *brightness, *contrast, *vsync;
    GtkWidget *status_label;
    GtkWidget *wiiu_status;
    GtkWidget *wozt_dashboard;
    GtkWidget *wozt_sections[8];
    GtkWidget *wozt_theme_combo, *wozt_language_combo;
    GtkWidget *wozt_clients_notebook;
    int wozt_section_count, wozt_layout_mode;
    GtkWidget *wozt_inner_output, *wozt_inner_input;

    /* The replug dialog. Changing what the adapter emulates is not done
     * when the write returns: the adapter has to be pulled out of the
     * console and out of this machine and put back, and until it is, the
     * pad is dead with nothing to show for it. So the window says so and
     * then watches, rather than leaving someone to guess whether it took. */
    GtkWidget *replug_dialog, *replug_label;
    GtkWidget *replug_cancel, *replug_apply, *replug_close;
    int replug_stage;      /* ReplugStage */
    int replug_down_ticks; /* consecutive ticks with the link down */
    int replug_pending;    /* the protocol being confirmed, -1 when none */
} Controls;

/* Where the replug is up to. The interesting one is WAIT_UNPLUG: the
 * program drops the USB link itself right after the write, so seeing it
 * down for an instant means nothing -- only seeing it STAY down means a
 * hand actually pulled the cable. */
typedef enum {
    REPLUG_IDLE = 0,
    REPLUG_CONFIRM,
    REPLUG_WAIT_UNPLUG,
    REPLUG_WAIT_BACK,
    REPLUG_WAIT_CONSOLE,
    REPLUG_DONE,
} ReplugStage;

/* At 200 ms a tick, a second and a half. The program's own reconnection
 * after the write takes a fraction of that, so it cannot be mistaken for
 * someone unplugging the adapter. */
#define REPLUG_DOWN_TICKS 8

static Controls g_c;

/* WOZT GTK Standard: responsive sections, live themes/languages and custom
 * window chrome. Only this GTK thread touches the widget hierarchy. */
/* WOZT_0864_TOP_ROW_MONITOR_CAPTURE:
 * Three responsive groups: Overview, Clients and a compact two-frame
 * Local monitor/Capture stack; both controller frames; Reset/Maintenance.
 * The stack is one LOGICAL item, but retains two independent GtkFrames.
 * Only the current group's last row item gets surplus width (except that
 * Clients absorbs it when the compact stack is the top row's third item).
 * Rows explicitly refuse excess vertical expansion. */
static const int wozt_group_first[3] = {0, 2, 6};
static const int wozt_group_size[3] = {3, 2, 2};
/* WOZT_0840_COMPACT_CONTROLLER_ROWS */
static const int wozt_section_target_width[8] = {
    260, 300,   /* compact Overview; Clients notebook scrolls horizontally */
    620, 350,   /* two output columns; compact one-column local input */
    /* Budgets MUST exceed the GTK minimum of each packed row. Otherwise
     * GTK blocks a resize before configure-event can reflow the row. */
    290, 310, 410, 340 /* Capture, Monitor, Reset, Maintenance */
};

/* WOZT_0855_OVERVIEW_REFLOW
 * The Overview no longer carries two fixed-width labels: its old 315px
 * packing budget is therefore obsolete. Like the controller panels, its
 * natural GTK width determines when it fits beside Clients. Clients and
 * the four utility panels keep their independent, stable width budgets. */
static int wozt_section_layout_width(int index)
{
    const int fallback = wozt_section_target_width[index];
    if (index != 0 && index != 1 && index != 2 && index != 3 &&
        index != 4 && index != 5) return fallback;
    GtkWidget *frame = g_c.wozt_sections[index];
    if (!GTK_IS_WIDGET(frame)) return fallback;
    gint minimum = 0, natural = 0;
    gtk_widget_get_preferred_width(frame, &minimum, &natural);
    /* WOZT_0860_CLIENTS_CONTENT_MIN: all seven notebook pages scroll.
     * Their NATURAL width must not trigger an early wrap or keep an
     * unused strip beside the selected, narrower page. The GTK
     * minimum still protects the tabs/controls from clipping. */
    if (index == 1) return minimum > 0 ? MAX(240, minimum) : fallback;
    return natural > 0 ? MAX(minimum, natural) : fallback;
}

/* WOZT_0866_RESET_FITS_TOP:
 * Reuse the small top-right vertical gutter ONLY if the complete Reset
 * frame fits without increasing the natural height of Overview/Clients.
 * A taller Reset would otherwise create a much larger unused region under
 * those two panels. GTK measures the actual current content (also localized).
 */
static gboolean wozt_reset_fits_heights(int overview, int clients,
                                        int monitor, int capture, int reset)
{
    if (overview <= 0 || clients <= 0 || monitor <= 0 ||
        capture <= 0 || reset <= 0) return FALSE;
    return MAX(overview, clients) >= monitor + capture + reset + 10;
}

static gboolean wozt_reset_fits_top(void)
{
    const int indices[] = {0, 1, 5, 4, 6};
    int h[G_N_ELEMENTS(indices)] = {0};
    for (guint i = 0; i < G_N_ELEMENTS(indices); ++i) {
        GtkWidget *w = g_c.wozt_sections[indices[i]];
        if (!GTK_IS_WIDGET(w)) return FALSE;
        gint minimum = 0, natural = 0;
        gtk_widget_get_preferred_height(w, &minimum, &natural);
        h[i] = MAX(minimum, natural);
    }
    return wozt_reset_fits_heights(h[0], h[1], h[2], h[3], h[4]);
}

/* WOZT_0867_RESET_MAINT_GRID: a homogeneous three-track utility grid
 * allocates two tracks to Reset and one to Maintenance. Only put the panels
 * beside one another when their actual minimum widths fit that ratio. */
static int wozt_utility_pair_min_width(void)
{
    GtkWidget *reset = g_c.wozt_sections[6];
    GtkWidget *maintenance = g_c.wozt_sections[7];
    const int fallback = wozt_section_layout_width(6) +
                         wozt_section_layout_width(7) + 6;
    if (!GTK_IS_WIDGET(reset) || !GTK_IS_WIDGET(maintenance)) return fallback;
    gint reset_min = 0, reset_nat = 0, maint_min = 0, maint_nat = 0;
    gtk_widget_get_preferred_width(reset, &reset_min, &reset_nat);
    gtk_widget_get_preferred_width(maintenance, &maint_min, &maint_nat);
    const int track = MAX((MAX(1, reset_min) + 1) / 2, MAX(1, maint_min));
    return MAX(fallback, 3 * track + 12); /* two 6px grid gaps */
}

static int wozt_dashboard_group_columns(int width, int group)
{
    if (group < 0 || group >= 3) return 1;
    const int space = MAX(180, width - 30);
    const int first = wozt_group_first[group];
    const gboolean reset_top = wozt_reset_fits_top();
    const int count = group == 2 && reset_top ? 1 : wozt_group_size[group];
    int total = 0, packed = 0;
    for (int i = 0; i < count; ++i) {
        /* One responsive slot for the entire top-right stack. The optional
         * Reset is considered in its width only when it fits by HEIGHT. */
        const int need_width = (group == 0 && i == 2)
            ? MAX(MAX(wozt_section_layout_width(5), wozt_section_layout_width(4)),
                  reset_top ? wozt_section_layout_width(6) : 0)
            : wozt_section_layout_width(group == 2
                  ? (reset_top ? 7 : 6 + i) : first + i);
        const int need = need_width + (i ? 6 : 0);
        if (group == 2 && !reset_top && i == 1 &&
            space < wozt_utility_pair_min_width()) break;
        if (packed && total + need > space) break;
        total += need;
        ++packed;
    }
    return MAX(1, packed);
}

static int wozt_dashboard_signature(int width)
{
    /* Reparent only when the number of sections in a logical row changes.
     * Continuous resize between those points stays smooth and cheap. */
    return wozt_dashboard_group_columns(width, 0)
         + 10 * wozt_dashboard_group_columns(width, 1)
         + 100 * wozt_dashboard_group_columns(width, 2)
         + 1000 * (wozt_reset_fits_top() ? 1 : 0);
}

static void wozt_dashboard_reflow(int width)
{
    if (!GTK_IS_BOX(g_c.wozt_dashboard) || g_c.wozt_section_count != 8) return;
    const int signature = wozt_dashboard_signature(width);
    if (signature == g_c.wozt_layout_mode) return;
    GtkWidget *dashboard = g_c.wozt_dashboard;
    const gboolean reset_top = wozt_reset_fits_top();

    /* Take one reference per section before detaching. No callbacks, controls,
     * settings or views are recreated during a responsive transition. */
    for (int i = 0; i < 8; ++i) {
        g_object_ref(g_c.wozt_sections[i]);
        GtkWidget *parent = gtk_widget_get_parent(g_c.wozt_sections[i]);
        if (GTK_IS_CONTAINER(parent))
            gtk_container_remove(GTK_CONTAINER(parent), g_c.wozt_sections[i]);
    }
    /* Old independent line containers are now empty. When the first layout
     * is applied, the dashboard instead contains direct child frames. */
    GList *old_rows = gtk_container_get_children(GTK_CONTAINER(dashboard));
    for (GList *it = old_rows; it; it = it->next)
        gtk_container_remove(GTK_CONTAINER(dashboard), GTK_WIDGET(it->data));
    g_list_free(old_rows);

    for (int group = 0; group < 3; ++group) {
        const int first = wozt_group_first[group];
        const int count = group == 2 && reset_top ? 1 : wozt_group_size[group];
        const int capacity = wozt_dashboard_group_columns(width, group);
        for (int offset = 0; offset < count; offset += capacity) {
            const int n = MIN(capacity, count - offset);
            GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_set_hexpand(line, TRUE);
            /* WOZT_0863_VERTICAL_REFLOW: GtkWidget::vexpand is inherited
             * from descendants (notably the Clients notebook). GtkBox's
             * FALSE/FALSE child packing does NOT override that inherited
             * request. Explicitly refuse extra vertical space on each row. */
            gtk_widget_set_vexpand(line, FALSE);
            gtk_widget_set_valign(line, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(dashboard), line, FALSE, FALSE, 0);
            if (group == 2 && !reset_top && n == 2) {
                /* 2/3 Reset, 1/3 Maintenance. Never use a fixed pixel split:
                 * the three homogeneous columns share the current row width. */
                GtkWidget *thirds = gtk_grid_new();
                gtk_grid_set_column_homogeneous(GTK_GRID(thirds), TRUE);
                gtk_grid_set_column_spacing(GTK_GRID(thirds), 6);
                gtk_widget_set_hexpand(thirds, TRUE);
                gtk_widget_set_vexpand(thirds, FALSE);
                gtk_box_pack_start(GTK_BOX(line), thirds, TRUE, TRUE, 0);
                GtkWidget *reset = g_c.wozt_sections[6];
                GtkWidget *maintenance = g_c.wozt_sections[7];
                gtk_widget_set_halign(reset, GTK_ALIGN_FILL);
                gtk_widget_set_halign(maintenance, GTK_ALIGN_FILL);
                gtk_widget_set_hexpand(reset, TRUE);
                gtk_widget_set_hexpand(maintenance, TRUE);
                gtk_widget_set_vexpand(reset, FALSE);
                gtk_widget_set_vexpand(maintenance, FALSE);
                gtk_grid_attach(GTK_GRID(thirds), reset, 0, 0, 2, 1);
                gtk_grid_attach(GTK_GRID(thirds), maintenance, 2, 0, 1, 1);
                gtk_widget_show(thirds);
                gtk_widget_show(reset);
                gtk_widget_show(maintenance);
                gtk_widget_show(line);
                continue;
            }
            for (int j = 0; j < n; ++j) {
                const int slot = offset + j;
                const gboolean last = j == n - 1;
                if (group == 0 && slot == 2) {
                    /* Keep the pair compact at the RIGHT of Clients. Clients
                     * takes spare horizontal width; the stack never becomes
                     * a very wide monitor panel or a tall empty column.
                     * Alone on a wrapped row, it still fills that row. */
                    const gboolean alone = n == 1;
                    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
                    gtk_widget_set_halign(column,
                                         alone ? GTK_ALIGN_FILL : GTK_ALIGN_START);
                    gtk_widget_set_hexpand(column, alone);
                    /* Fill only the height ALREADY requested by Overview or
                     * Clients. No extra height is requested on the outer row. */
                    gtk_widget_set_valign(column, GTK_ALIGN_FILL);
                    gtk_widget_set_vexpand(column, FALSE);
                    gtk_box_pack_start(GTK_BOX(line), column, alone, alone, 0);
                    GtkWidget *monitor = g_c.wozt_sections[5];
                    GtkWidget *capture = g_c.wozt_sections[4];
                    gtk_widget_set_halign(monitor, GTK_ALIGN_FILL);
                    gtk_widget_set_halign(capture, GTK_ALIGN_FILL);
                    gtk_widget_set_hexpand(monitor, TRUE);
                    gtk_widget_set_hexpand(capture, TRUE);
                    gtk_widget_set_valign(monitor, GTK_ALIGN_FILL);
                    gtk_widget_set_valign(capture, GTK_ALIGN_START);
                    gtk_widget_set_vexpand(monitor, TRUE);
                    gtk_widget_set_vexpand(capture, FALSE);
                    gtk_box_pack_start(GTK_BOX(column), monitor, TRUE, TRUE, 0);
                    gtk_box_pack_start(GTK_BOX(column), capture, FALSE, TRUE, 0);
                    if (reset_top) {
                        /* Only possible when no other top-row panel must
                         * become taller to accommodate this entire frame. */
                        GtkWidget *reset = g_c.wozt_sections[6];
                        gtk_widget_set_valign(reset, GTK_ALIGN_START);
                        gtk_widget_set_vexpand(reset, FALSE);
                        gtk_widget_set_halign(reset, GTK_ALIGN_FILL);
                        gtk_widget_set_hexpand(reset, TRUE);
                        gtk_box_pack_start(GTK_BOX(column), reset, FALSE, TRUE, 0);
                    }
                    gtk_widget_show_all(column);
                    continue;
                }
                GtkWidget *frame = g_c.wozt_sections[
                    group == 2 ? (reset_top ? 7 : 6 + slot) : first + slot];
                /* In the three-item top row, Clients expands while the last
                 * stack stays compact. Else the last item of each row fills
                 * surplus width. There are no artificial middle spacers. */
                const gboolean grow = last ||
                    (group == 0 && slot == 1 && n == 3);
                gtk_widget_set_halign(frame,
                                      grow ? GTK_ALIGN_FILL : GTK_ALIGN_START);
                gtk_widget_set_hexpand(frame, grow);
                gtk_box_pack_start(GTK_BOX(line), frame, grow, grow, 0);
                gtk_widget_show(frame);
            }
            gtk_widget_show(line);
        }
    }
    for (int i = 0; i < 8; ++i) g_object_unref(g_c.wozt_sections[i]);
    g_c.wozt_layout_mode = signature;
}

static gboolean wozt_window_configured(GtkWidget *widget, GdkEventConfigure *event,
                                        gpointer userdata)
{
    (void)widget;
    (void)userdata;
    wozt_dashboard_reflow(event->width);
    /* The DASHBOARD reflows controller frames, but Output's two genuine
     * internal columns must NEVER stack, including below 665px. The main
     * scroller handles overflow if a very narrow window cannot fit them. */
    if (GTK_IS_BOX(g_c.wozt_inner_output))
        gtk_orientable_set_orientation(GTK_ORIENTABLE(g_c.wozt_inner_output),
                                      GTK_ORIENTATION_HORIZONTAL);
    return FALSE;
}

/* Cairo previews do not restyle automatically when a CSS provider changes.
 * Queue a redraw for all four currently created views on live theme events. */
static void wozt_redraw_stick_previews(void)
{
    for (int side = 0; side < 2; ++side) {
        if (g_c.input_stick_preview[side])
            gtk_widget_queue_draw(g_c.input_stick_preview[side]);
        if (g_c.output_stick_preview[side])
            gtk_widget_queue_draw(g_c.output_stick_preview[side]);
    }
}

static void wozt_theme_changed(GtkComboBox *combo, gpointer user_data)
{
    GtkShell *shell = user_data;
    if (!shell->wozt || shell->loading) return;
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    wozt_ui_standard_apply_theme(shell->wozt, id);
    wozt_redraw_stick_previews();
    config_set_str("WOZT_UI_THEME", wozt_ui_standard_theme_id(shell->wozt));
}

static void wozt_language_changed(GtkComboBox *combo, gpointer user_data)
{
    GtkShell *shell = user_data;
    if (!shell->wozt || shell->loading) return;
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    shell->loading = 1;
    wozt_ui_standard_apply_language(shell->wozt, id, shell->settings_window);
    shell->loading = 0;
    config_set_str("WOZT_UI_LANGUAGE", wozt_ui_standard_language_id(shell->wozt));
}

static void wozt_reload_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkShell *shell = user_data;
    if (!shell->wozt) return;
    shell->loading = 1;
    wozt_ui_standard_reload(shell->wozt, shell->settings_window);
    wozt_redraw_stick_previews();
    shell->loading = 0;
}

static void wozt_window_minimize(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkShell *shell = user_data;
    gtk_window_iconify(GTK_WINDOW(shell->settings_window));
}

static void wozt_window_maximize(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkShell *shell = user_data;
    if (gtk_window_is_maximized(GTK_WINDOW(shell->settings_window)))
        gtk_window_unmaximize(GTK_WINDOW(shell->settings_window));
    else gtk_window_maximize(GTK_WINDOW(shell->settings_window));
}

static void wozt_window_hide(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkShell *shell = user_data;
    /* Capture2Cloud lives in the tray; never kill the stream here. */
    gtk_widget_hide(shell->settings_window);
}

static gboolean wozt_title_drag(GtkWidget *widget, GdkEventButton *event,
                                 gpointer user_data)
{
    (void)widget;
    GtkShell *shell = user_data;
    if (event->button != 1) return FALSE;
    if (event->type == GDK_2BUTTON_PRESS) {
        if (gtk_window_is_maximized(GTK_WINDOW(shell->settings_window)))
            gtk_window_unmaximize(GTK_WINDOW(shell->settings_window));
        else gtk_window_maximize(GTK_WINDOW(shell->settings_window));
        return TRUE;
    }
    if (event->type == GDK_BUTTON_PRESS) {
        gtk_window_begin_move_drag(GTK_WINDOW(shell->settings_window),
                                   (gint)event->button,
                                   (gint)event->x_root, (gint)event->y_root,
                                   event->time);
        return TRUE;
    }
    return FALSE;
}

static gboolean wozt_resize_drag(GtkWidget *widget, GdkEventButton *event,
                                  gpointer user_data)
{
    (void)widget;
    if (event->button != 1 || event->type != GDK_BUTTON_PRESS) return FALSE;
    GtkWidget *win = g_object_get_data(G_OBJECT(widget), "wozt-window");
    if (!win || gtk_window_is_maximized(GTK_WINDOW(win))) return FALSE;
    gtk_window_begin_resize_drag(GTK_WINDOW(win), (GdkWindowEdge)GPOINTER_TO_INT(user_data),
                                 (gint)event->button,
                                 (gint)event->x_root, (gint)event->y_root,
                                 event->time);
    return TRUE;
}

static void wozt_resize_realized(GtkWidget *widget, gpointer user_data)
{
    static const char *const CURSORS[8] = {
        "nw-resize", "n-resize", "ne-resize", "w-resize",
        "e-resize", "sw-resize", "s-resize", "se-resize"
    };
    int index = GPOINTER_TO_INT(user_data);
    if (index < 0 || index >= 8) return;
    GdkDisplay *display = gtk_widget_get_display(widget);
    GdkCursor *cursor = gdk_cursor_new_from_name(display, CURSORS[index]);
    if (cursor) {
        GdkWindow *window = gtk_widget_get_window(widget);
        if (window) gdk_window_set_cursor(window, cursor);
        g_object_unref(cursor);
    }
}

static void wozt_add_resize_zones(GtkWidget *overlay, GtkWidget *window)
{
    static const GdkWindowEdge EDGES[8] = {
        GDK_WINDOW_EDGE_NORTH_WEST, GDK_WINDOW_EDGE_NORTH,
        GDK_WINDOW_EDGE_NORTH_EAST, GDK_WINDOW_EDGE_WEST,
        GDK_WINDOW_EDGE_EAST, GDK_WINDOW_EDGE_SOUTH_WEST,
        GDK_WINDOW_EDGE_SOUTH, GDK_WINDOW_EDGE_SOUTH_EAST
    };
    for (int i = 0; i < 8; ++i) {
        GtkWidget *zone = gtk_event_box_new();
        gtk_event_box_set_visible_window(GTK_EVENT_BOX(zone), TRUE);
        gtk_widget_add_events(zone, GDK_BUTTON_PRESS_MASK);
        gtk_style_context_add_class(gtk_widget_get_style_context(zone), "wozt-resize");
        const gboolean left = i == 0 || i == 3 || i == 5;
        const gboolean right = i == 2 || i == 4 || i == 7;
        const gboolean top = i <= 2;
        const gboolean bottom = i >= 5;
        const gboolean corner = i == 0 || i == 2 || i == 5 || i == 7;
        gtk_widget_set_halign(zone, left ? GTK_ALIGN_START :
                                   right ? GTK_ALIGN_END : GTK_ALIGN_FILL);
        gtk_widget_set_valign(zone, top ? GTK_ALIGN_START :
                                   bottom ? GTK_ALIGN_END : GTK_ALIGN_FILL);
        gtk_widget_set_size_request(zone, corner ? 12 : (left || right ? 5 : -1),
                                          corner ? 12 : (top || bottom ? 5 : -1));
        g_object_set_data(G_OBJECT(zone), "wozt-window", window);
        g_signal_connect(zone, "realize", G_CALLBACK(wozt_resize_realized),
                         GINT_TO_POINTER(i));
        g_signal_connect(zone, "button-press-event", G_CALLBACK(wozt_resize_drag),
                         GINT_TO_POINTER(EDGES[i]));
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), zone);
    }
}

static GtkWidget *wozt_window_control(const char *label, const char *tooltip,
                                       GCallback callback, GtkShell *shell)
{
    const char *icon_name = NULL;
    if (!strcmp(tooltip, "Minimize")) icon_name = "window-minimize-symbolic";
    else if (!strcmp(tooltip, "Maximize / restore")) icon_name = "window-maximize-symbolic";
    else if (!strcmp(tooltip, "Hide to tray")) icon_name = "window-close-symbolic";
    else if (!strcmp(tooltip, "Reload themes and languages")) icon_name = "view-refresh-symbolic";
    GtkIconTheme *icons = gtk_icon_theme_get_default();
    GtkWidget *button = icon_name && gtk_icon_theme_has_icon(icons, icon_name)
        ? gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU)
        : gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_style_context_add_class(gtk_widget_get_style_context(button),
                                "wozt-window-button");
    g_signal_connect(button, "clicked", callback, shell);
    return button;
}


/* --- talking to the program ----------------------------------------- */

static void publish(GtkShell *shell) {
    if (shell->loading) {
        return;
    }
    AppSettings copy;
    SDL_LockMutex(shell->lock);
    copy = shell->settings;
    SDL_UnlockMutex(shell->lock);
    if (shell->callbacks.on_settings) {
        shell->callbacks.on_settings(shell->callbacks.userdata, &copy);
    }
}

static void act(GtkShell *shell, GtkShellAction action) {
    if (shell->callbacks.on_action) {
        shell->callbacks.on_action(shell->callbacks.userdata, action);
    }
}

/* --- console reset / wake controls ---------------------------------- */

static int reset_selected_adapter(void)
{
    if (!g_c.reset_bt_adapter) {
        return -1;
    }

    int index =
        gtk_combo_box_get_active(
            GTK_COMBO_BOX(g_c.reset_bt_adapter));

    return index >= 0 &&
           index < g_c.reset_bt_adapter_count
        ? index
        : -1;
}

/* Keep every asynchronous wake-status message available in full even
 * when the compact panel can display only a single line. */
static void wozt_reset_status_tooltip_changed(GObject *object,
                                               GParamSpec *property,
                                               gpointer userdata)
{
    (void)property;
    (void)userdata;
    if (GTK_IS_LABEL(object))
        gtk_widget_set_tooltip_text(GTK_WIDGET(object),
                                     gtk_label_get_text(GTK_LABEL(object)));
}

static void reset_update_controls(GtkShell *shell)
{
    (void)shell;

    if (!g_c.reset_method) {
        return;
    }

    ResetMethod method =
        gtk_combo_box_get_active(
            GTK_COMBO_BOX(g_c.reset_method)) == 1
            ? RESET_METHOD_BLUETOOTH
            : RESET_METHOD_SCRIPT;

    if (g_c.reset_script_panel) {
        if (method == RESET_METHOD_SCRIPT) {
            gtk_widget_set_no_show_all(
                g_c.reset_script_panel,
                FALSE);
            gtk_widget_show_all(
                g_c.reset_script_panel);
            gtk_widget_set_no_show_all(
                g_c.reset_script_panel,
                TRUE);
        } else {
            gtk_widget_hide(
                g_c.reset_script_panel);
        }
    }

    if (g_c.reset_bt_panel) {
        if (method == RESET_METHOD_BLUETOOTH) {
            gtk_widget_set_no_show_all(
                g_c.reset_bt_panel,
                FALSE);
            gtk_widget_show_all(
                g_c.reset_bt_panel);
            gtk_widget_set_no_show_all(
                g_c.reset_bt_panel,
                TRUE);
        } else {
            gtk_widget_hide(
                g_c.reset_bt_panel);
        }
    }
}

static void reset_method_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    GtkShell *shell = user_data;

    if (shell->loading) {
        return;
    }

    ResetMethod method =
        gtk_combo_box_get_active(
            GTK_COMBO_BOX(widget)) == 1
            ? RESET_METHOD_BLUETOOTH
            : RESET_METHOD_SCRIPT;

    if (reset_method_set(method) != 0) {
        gtk_shell_show_error(
            shell,
            "Unable to save RESET_METHOD.");
        return;
    }

    reset_update_controls(shell);
}

static void reset_script_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    GtkShell *shell = user_data;

    if (shell->loading) {
        return;
    }

    const char *value =
        gtk_entry_get_text(
            GTK_ENTRY(widget));

    if (!value || !*value) {
        return;
    }

    if (reset_method_set_script(value) != 0) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Script path contains unsupported characters.");
    }
}

static void reset_target_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    GtkShell *shell = user_data;

    if (shell->loading) {
        return;
    }

    /*
     * Only one target exists today, but keep the selection explicit so
     * adding another console later does not change the config shape.
     */
    if (gtk_combo_box_get_active(
            GTK_COMBO_BOX(widget)) == 0) {
        reset_method_set_bluetooth_target(
            "switch2");
    }
}

static void reset_adapter_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;

    GtkShell *shell = user_data;

    if (shell->loading) {
        return;
    }

    int index =
        reset_selected_adapter();

    if (index < 0) {
        return;
    }

    if (reset_method_set_bluetooth_adapter(
            g_c.reset_bt_adapters[index].address) != 0) {
        gtk_shell_show_error(
            shell,
            "Unable to save the Bluetooth wake adapter.");
    }
}

static void reset_refresh_adapters(
    GtkWidget *button,
    gpointer user_data)
{
    (void)button;

    GtkShell *shell = user_data;

    char error[256];

    int count =
        reset_method_scan_bluetooth_adapters(
            g_c.reset_bt_adapters,
            error,
            sizeof(error));

    if (count < 0) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            error[0]
                ? error
                : "Unable to enumerate Bluetooth adapters.");
        return;
    }

    g_c.reset_bt_adapter_count = count;

    char preferred[32];
    reset_method_get_bluetooth_adapter(
        preferred,
        sizeof(preferred));

    shell->loading = 1;

    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(g_c.reset_bt_adapter));

    int selected = -1;

    for (int i = 0; i < count; i++) {
        char label[64];

        snprintf(
            label,
            sizeof(label),
            "%s · %s",
            g_c.reset_bt_adapters[i].id,
            g_c.reset_bt_adapters[i].address);

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.reset_bt_adapter),
            label);

        if (preferred[0] &&
            g_ascii_strcasecmp(
                preferred,
                g_c.reset_bt_adapters[i].address) == 0) {
            selected = i;
        }
    }

    if (count == 0) {
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.reset_bt_adapter),
            "no Bluetooth adapter detected");

        gtk_combo_box_set_active(
            GTK_COMBO_BOX(g_c.reset_bt_adapter),
            0);

        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "No Bluetooth adapter detected.");
    } else {
        if (selected < 0) {
            selected = 0;
        }

        gtk_combo_box_set_active(
            GTK_COMBO_BOX(g_c.reset_bt_adapter),
            selected);

        /*
         * If no saved adapter existed, make the visible default real
         * configuration immediately.
         */
        if (!preferred[0]) {
            reset_method_set_bluetooth_adapter(
                g_c.reset_bt_adapters[selected].address);
        }

        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Bluetooth adapter ready for configuration.");
    }

    shell->loading = 0;
}

typedef struct {
    GtkShell *shell;
    char adapter[16];
    int ok;
    char message[512];
} ResetProbeJob;

static gboolean reset_probe_finished(
    gpointer user_data)
{
    ResetProbeJob *job =
        user_data;

    if (g_c.reset_bt_status) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            job->message[0]
                ? job->message
                : (job->ok
                    ? "Bluetooth dongle is compatible."
                    : "Bluetooth dongle compatibility test failed."));
    }

    if (g_c.reset_bt_test_adapter) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_test_adapter,
            TRUE);
    }

    free(job);
    return G_SOURCE_REMOVE;
}

static int reset_probe_thread(
    void *user_data)
{
    ResetProbeJob *job =
        user_data;

    job->ok =
        reset_method_test_bluetooth_adapter(
            job->adapter,
            job->message,
            sizeof(job->message));

    g_idle_add(
        reset_probe_finished,
        job);

    return 0;
}

static void reset_test_adapter_clicked(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;

    GtkShell *shell =
        user_data;

    int index =
        reset_selected_adapter();

    if (index < 0) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Select a Bluetooth adapter first.");
        return;
    }

    /*
     * Do not fight a live Nintendo-controller helper for ownership of
     * the same Bluetooth stack. Shared-adapter wake orchestration will
     * explicitly release/reconnect pcble later.
     */
    if (output_pcble_session_running() ||
        output_pcble_ipc_up()) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Controller Bluetooth session is active. Stop it before testing the wake adapter.");
        return;
    }

    ResetProbeJob *job =
        calloc(
            1,
            sizeof(*job));

    if (!job) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Could not allocate Bluetooth test job.");
        return;
    }

    job->shell = shell;

    snprintf(
        job->adapter,
        sizeof(job->adapter),
        "%s",
        g_c.reset_bt_adapters[index].id);

    /*
     * The visible hciN is runtime-only. Persist the physical MAC.
     */
    reset_method_set_bluetooth_adapter(
        g_c.reset_bt_adapters[index].address);

    gtk_label_set_text(
        GTK_LABEL(g_c.reset_bt_status),
        "Testing LE scan and advertising support...");

    gtk_widget_set_sensitive(
        g_c.reset_bt_test_adapter,
        FALSE);

    SDL_Thread *thread =
        SDL_CreateThread(
            reset_probe_thread,
            "reset-bt-probe",
            job);

    if (!thread) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_test_adapter,
            TRUE);

        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Could not start Bluetooth compatibility test.");

        free(job);
        return;
    }

    SDL_DetachThread(thread);
}

typedef struct {
    char adapter[16];
    int ok;
    char message[512];
} ResetCaptureJob;

static gboolean reset_capture_finished(
    gpointer user_data)
{
    ResetCaptureJob *job =
        user_data;

    if (g_c.reset_bt_status) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            job->message[0]
                ? job->message
                : (job->ok
                    ? "Switch 2 wake beacon captured."
                    : "Wake beacon capture failed."));
    }

    if (g_c.reset_bt_capture) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_capture,
            TRUE);
    }

    if (g_c.reset_bt_test_adapter) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_test_adapter,
            TRUE);
    }

    free(job);
    return G_SOURCE_REMOVE;
}

static int reset_capture_thread(
    void *user_data)
{
    ResetCaptureJob *job =
        user_data;

    job->ok =
        reset_method_capture_bluetooth_beacon(
            job->adapter,
            job->message,
            sizeof(job->message));

    g_idle_add(
        reset_capture_finished,
        job);

    return 0;
}

static void reset_capture_clicked(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;
    (void)user_data;

    int index =
        reset_selected_adapter();

    if (index < 0) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Select a Bluetooth adapter first.");
        return;
    }

    if (output_pcble_session_running() ||
        output_pcble_ipc_up()) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Controller Bluetooth session is active. Wait for it to stop before capturing the wake beacon.");
        return;
    }

    ResetCaptureJob *job =
        calloc(
            1,
            sizeof(*job));

    if (!job) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Could not allocate wake capture job.");
        return;
    }

    snprintf(
        job->adapter,
        sizeof(job->adapter),
        "%s",
        g_c.reset_bt_adapters[index].id);

    reset_method_set_bluetooth_adapter(
        g_c.reset_bt_adapters[index].address);

    gtk_label_set_text(
        GTK_LABEL(g_c.reset_bt_status),
        "Listening for 20 seconds. Put the Switch 2 to sleep, then press HOME on a real paired controller.");

    gtk_widget_set_sensitive(
        g_c.reset_bt_capture,
        FALSE);

    gtk_widget_set_sensitive(
        g_c.reset_bt_test_adapter,
        FALSE);

    SDL_Thread *thread =
        SDL_CreateThread(
            reset_capture_thread,
            "reset-bt-capture",
            job);

    if (!thread) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_capture,
            TRUE);

        gtk_widget_set_sensitive(
            g_c.reset_bt_test_adapter,
            TRUE);

        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Could not start wake beacon capture.");

        free(job);
        return;
    }

    SDL_DetachThread(thread);
}

typedef struct {
    char adapter[16];
    int ok;
    char message[512];
} ResetBeaconTestJob;

static gboolean reset_beacon_test_finished(
    gpointer user_data)
{
    ResetBeaconTestJob *job =
        user_data;

    if (g_c.reset_bt_status) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            job->message[0]
                ? job->message
                : (job->ok
                    ? "Wake beacon transmitted."
                    : "Wake beacon transmission failed."));
    }

    if (g_c.reset_bt_test_beacon) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_test_beacon,
            TRUE);
    }

    if (g_c.reset_bt_capture) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_capture,
            TRUE);
    }

    if (g_c.reset_bt_test_adapter) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_test_adapter,
            TRUE);
    }

    free(job);
    return G_SOURCE_REMOVE;
}

static int reset_beacon_test_thread(
    void *user_data)
{
    ResetBeaconTestJob *job =
        user_data;

    job->ok =
        reset_method_test_bluetooth_beacon(
            job->adapter,
            job->message,
            sizeof(job->message));

    g_idle_add(
        reset_beacon_test_finished,
        job);

    return 0;
}

static void reset_test_beacon_clicked(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;
    (void)user_data;

    int index =
        reset_selected_adapter();

    if (index < 0) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Select a Bluetooth adapter first.");
        return;
    }

    if (output_pcble_session_running() ||
        output_pcble_ipc_up()) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Controller Bluetooth session is active. Wait for it to stop before transmitting the wake beacon.");
        return;
    }

    ResetBeaconTestJob *job =
        calloc(
            1,
            sizeof(*job));

    if (!job) {
        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Could not allocate wake beacon test job.");
        return;
    }

    snprintf(
        job->adapter,
        sizeof(job->adapter),
        "%s",
        g_c.reset_bt_adapters[index].id);

    reset_method_set_bluetooth_adapter(
        g_c.reset_bt_adapters[index].address);

    gtk_label_set_text(
        GTK_LABEL(g_c.reset_bt_status),
        "Transmitting saved Switch 2 wake beacon for 3 seconds...");

    gtk_widget_set_sensitive(
        g_c.reset_bt_test_beacon,
        FALSE);

    gtk_widget_set_sensitive(
        g_c.reset_bt_capture,
        FALSE);

    gtk_widget_set_sensitive(
        g_c.reset_bt_test_adapter,
        FALSE);

    SDL_Thread *thread =
        SDL_CreateThread(
            reset_beacon_test_thread,
            "reset-bt-send",
            job);

    if (!thread) {
        gtk_widget_set_sensitive(
            g_c.reset_bt_test_beacon,
            TRUE);

        gtk_widget_set_sensitive(
            g_c.reset_bt_capture,
            TRUE);

        gtk_widget_set_sensitive(
            g_c.reset_bt_test_adapter,
            TRUE);

        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Could not start wake beacon transmission.");

        free(job);
        return;
    }

    SDL_DetachThread(thread);
}

static void reset_load_config(GtkShell *shell)
{
    shell->loading = 1;

    gtk_combo_box_set_active(
        GTK_COMBO_BOX(g_c.reset_method),
        reset_method_current() == RESET_METHOD_BLUETOOTH
            ? 1
            : 0);

    char script[512];
    reset_method_get_script(
        script,
        sizeof(script));

    gtk_entry_set_text(
        GTK_ENTRY(g_c.reset_script),
        script);

    char target[32];
    reset_method_get_bluetooth_target(
        target,
        sizeof(target));

    gtk_combo_box_set_active(
        GTK_COMBO_BOX(g_c.reset_bt_target),
        0);

    /*
     * Do not probe Bluetooth while the GTK shell is being constructed.
     *
     * The tray icon is created only after build_settings_window()
     * returns. Running btmgmt here can block or disturb the Bluetooth
     * stack before GTK has even reached tray-icon creation.
     *
     * Show the saved adapter, if any, and enumerate controllers only
     * when the user explicitly presses Refresh adapters.
     */
    char saved_adapter[32];
    reset_method_get_bluetooth_adapter(
        saved_adapter,
        sizeof(saved_adapter));

    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(g_c.reset_bt_adapter));

    if (saved_adapter[0]) {
        char label[80];

        snprintf(
            label,
            sizeof(label),
            "saved · %s — press Refresh adapters",
            saved_adapter);

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.reset_bt_adapter),
            label);

        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Saved Bluetooth adapter loaded. Press Refresh adapters to enumerate controllers.");
    } else {
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.reset_bt_adapter),
            "press Refresh adapters");

        gtk_label_set_text(
            GTK_LABEL(g_c.reset_bt_status),
            "Press Refresh adapters to enumerate Bluetooth controllers.");
    }

    gtk_combo_box_set_active(
        GTK_COMBO_BOX(g_c.reset_bt_adapter),
        0);

    g_c.reset_bt_adapter_count = 0;

    shell->loading = 0;

    reset_update_controls(shell);
}


/* --- pcble Bluetooth controls --------------------------------------- */

static int pcble_mac_valid(const char *s)
{
    if (!s || strlen(s) != 17) {
        return 0;
    }

    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (s[i] != ':') {
                return 0;
            }
        } else if (!g_ascii_isxdigit(s[i])) {
            return 0;
        }
    }

    return 1;
}

static const char *pcble_profile(void)
{
    if (!g_c.pcble_controller) return "pro";
    switch (gtk_combo_box_get_active(GTK_COMBO_BOX(g_c.pcble_controller))) {
    case 1: return "joycon-pair";
    case 2: return "joycon-mixed";
    case 3: return "joycon2-left";
    case 4: return "joycon2-right";
    case 5: return "joycon2-pair";
    case 6: return "joycon-mixed-reverse";
    default: return "pro";
    }
}

static int pcble_profile_is_pair(const char *profile)
{
    return strcmp(profile, "joycon-pair") == 0 ||
           strcmp(profile, "joycon-mixed") == 0 ||
           strcmp(profile, "joycon2-pair") == 0 ||
           strcmp(profile, "joycon-mixed-reverse") == 0;
}

static int pcble_profile_is_ble_only(const char *profile)
{
    return strcmp(profile, "joycon2-left") == 0 ||
           strcmp(profile, "joycon2-right") == 0 ||
           strcmp(profile, "joycon2-pair") == 0;
}

static int pcble_profile_reconnect_supported(const char *profile)
{
    return strcmp(profile, "pro") == 0 ||
           strcmp(profile, "joycon-mixed") == 0 ||
           strcmp(profile, "joycon-mixed-reverse") == 0 ||
           strcmp(profile, "joycon-pair") == 0 ||
           pcble_profile_is_ble_only(profile);
}

static int pcble_selected_adapter(GtkWidget *combo)
{
    if (!combo) {
        return -1;
    }

    int index =
        gtk_combo_box_get_active(
            GTK_COMBO_BOX(combo));

    return index >= 0 &&
           index < g_c.pcble_adapter_count
        ? index
        : -1;
}

static void pcble_color_hex(GtkWidget *button, char out[7])
{
    GdkRGBA rgba = {0};

    gtk_color_chooser_get_rgba(
        GTK_COLOR_CHOOSER(button),
        &rgba);

    int r = CLAMP((int)(rgba.red * 255.0 + 0.5), 0, 255);
    int g = CLAMP((int)(rgba.green * 255.0 + 0.5), 0, 255);
    int b = CLAMP((int)(rgba.blue * 255.0 + 0.5), 0, 255);

    snprintf(out, 7, "%02X%02X%02X", r, g, b);
}

static void pcble_set_color_from_config(
    GtkWidget *button,
    const char *key,
    const char *fallback)
{
    char buf[16];
    const char *value =
        config_get_str(
            key,
            buf,
            sizeof(buf),
            fallback);

    char css[16];
    snprintf(css, sizeof(css), "#%s", value);

    GdkRGBA rgba;

    if (!gdk_rgba_parse(&rgba, css)) {
        snprintf(css, sizeof(css), "#%s", fallback);
        gdk_rgba_parse(&rgba, css);
    }

    gtk_color_chooser_set_rgba(
        GTK_COLOR_CHOOSER(button),
        &rgba);
}

static void pcble_update_controls(GtkShell *shell)
{
    if (!g_c.pcble_settings) {
        return;
    }

    int backend;

    SDL_LockMutex(shell->lock);
    backend = shell->settings.output_backend;
    SDL_UnlockMutex(shell->lock);

    const int pcble =
        gamepad_bridge_backend_from_name("pcble");

    const int titan =
        gamepad_bridge_backend_from_name("titan");

    const int selected_pcble =
        backend == pcble;

    const int pair =
        pcble_profile_is_pair(pcble_profile());

    const char *profile = pcble_profile();
    const int can_reconnect_profile = pcble_profile_reconnect_supported(profile);

    const int running =
        output_pcble_session_running() ||
        output_pcble_ipc_up();

    const int recovery_pending =
        output_pcble_reconnect_pending();

    const int reconnect_active =
        output_pcble_reconnect_active();

    const int stopping =
        output_pcble_session_stopping();

    const int recovery_busy =
        recovery_pending ||
        reconnect_active ||
        stopping;

    const int busy =
        running ||
        recovery_busy;

    if (selected_pcble) {
        gtk_widget_set_no_show_all(
            g_c.pcble_settings,
            FALSE);
        gtk_widget_show_all(
            g_c.pcble_settings);
        gtk_widget_set_no_show_all(
            g_c.pcble_settings,
            TRUE);
    } else {
        gtk_widget_hide(
            g_c.pcble_settings);
    }

    if (g_c.titan_settings) {
        if (backend == titan) {
            gtk_widget_set_no_show_all(
                g_c.titan_settings,
                FALSE);
            gtk_widget_show_all(
                g_c.titan_settings);
            gtk_widget_set_no_show_all(
                g_c.titan_settings,
                TRUE);
        } else {
            gtk_widget_hide(
                g_c.titan_settings);
        }
    }

    if (!selected_pcble) {
        return;
    }

    gtk_widget_set_sensitive(
        g_c.pcble_controller,
        !busy);

    gtk_widget_set_sensitive(
        g_c.pcble_primary,
        !busy);

    gtk_widget_set_sensitive(
        g_c.pcble_secondary,
        pair && !busy);

    gtk_widget_set_sensitive(
        g_c.pcble_mouse_side,
        strcmp(profile, "joycon2-pair") == 0);

    gtk_widget_set_sensitive(
        g_c.pcble_body_color,
        strcmp(profile, "pro") == 0 && !busy);

    gtk_widget_set_sensitive(
        g_c.pcble_button_color,
        strcmp(profile, "pro") == 0 && !busy);

    gtk_widget_set_sensitive(
        g_c.pcble_left_grip_color,
        strcmp(profile, "pro") == 0 && !busy);

    gtk_widget_set_sensitive(
        g_c.pcble_right_grip_color,
        strcmp(profile, "pro") == 0 && !busy);

    int primary =
        pcble_selected_adapter(
            g_c.pcble_primary);

    int secondary =
        pcble_selected_adapter(
            g_c.pcble_secondary);

    int adapters_ok =
        primary >= 0 &&
        (!pair ||
         (secondary >= 0 &&
          secondary != primary));

    int reconnect_ok = 0;

    if (pcble_profile_is_ble_only(profile) && adapters_ok) {
        /* A reconnect must use the adapters whose bonds were saved.
         * Switching a combo without Pair / Sync must not target old bonds. */
        char saved_primary[32];
        char saved_secondary[32];
        config_get_str("PCBLE_PRIMARY_ADAPTER", saved_primary,
                       sizeof(saved_primary), "");
        config_get_str("PCBLE_SECONDARY_ADAPTER", saved_secondary,
                       sizeof(saved_secondary), "");
        reconnect_ok = pcble_mac_valid(saved_primary) &&
            g_ascii_strcasecmp(saved_primary,
                               g_c.pcble_adapters[primary].address) == 0 &&
            (!pair || (pcble_mac_valid(saved_secondary) &&
                g_ascii_strcasecmp(saved_secondary,
                                   g_c.pcble_adapters[secondary].address) == 0));
    } else if (!pcble_profile_is_ble_only(profile) &&
               can_reconnect_profile &&
        primary >= 0 &&
        g_c.pcble_paired_switch[0] &&
        g_c.pcble_paired_adapter[0]) {
        const int classic = strcmp(profile, "joycon-mixed-reverse") == 0
            ? secondary : primary;
        char marker[32];
        const char *saved_profile = config_get_str(
            "PCBLE_PAIRED_PROFILE", marker, sizeof(marker), "");
        reconnect_ok = classic >= 0 &&
            g_ascii_strcasecmp(g_c.pcble_adapters[classic].address,
                               g_c.pcble_paired_adapter) == 0 &&
            (!*saved_profile || strcmp(saved_profile, profile) == 0) &&
            (strcmp(profile, "joycon-pair") != 0 || *saved_profile);
    }

    gtk_widget_set_sensitive(
        g_c.pcble_pair,
        adapters_ok && !busy);

    gtk_widget_set_sensitive(
        g_c.pcble_reconnect,
        reconnect_ok && !busy);

    gtk_widget_set_sensitive(
        g_c.pcble_stop,
        running);

    /*
     * Make externally-triggered recovery visible in Controller output.
     * A GtkButton is not a toggle, so the clearest persistent feedback is
     * a changed label plus disabled state while the operation is active.
     */
    gtk_button_set_label(
        GTK_BUTTON(g_c.pcble_reconnect),
        recovery_busy
            ? "Reconnecting paired Switch..."
            : "Reconnect paired Switch");

    if (g_c.backend_recovery) {
        gtk_button_set_label(
            GTK_BUTTON(g_c.backend_recovery),
            recovery_busy
                ? "reconnecting paired Switch..."
                : "reconnect paired Switch");

        gtk_widget_set_sensitive(
            g_c.backend_recovery,
            !recovery_busy);
    }
}

static void pcble_refresh_adapters(
    GtkWidget *button,
    gpointer user_data)
{
    (void)button;

    GtkShell *shell = user_data;

    char error[256];

    int count =
        output_pcble_scan_adapters(
            g_c.pcble_adapters,
            error,
            sizeof(error));

    if (count < 0) {
        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            error);

        gtk_shell_show_error(shell, error);
        return;
    }

    g_c.pcble_adapter_count = count;

    char preferred_primary[32] = {0};
    char preferred_secondary[32] = {0};

    config_get_str(
        "PCBLE_PRIMARY_ADAPTER",
        preferred_primary,
        sizeof(preferred_primary),
        "");

    config_get_str(
        "PCBLE_SECONDARY_ADAPTER",
        preferred_secondary,
        sizeof(preferred_secondary),
        "");

    /*
     * A saved Pro pairing belongs to one physical Bluetooth adapter
     * because its Link Key/identity belongs to that adapter. Prefer it
     * over an older arbitrary primary selection.
     */
    if (g_c.pcble_paired_adapter[0] &&
        g_c.pcble_paired_switch[0] &&
        strcmp(pcble_profile(), "pro") == 0) {
        snprintf(
            preferred_primary,
            sizeof(preferred_primary),
            "%s",
            g_c.pcble_paired_adapter);
    }

    shell->loading = 1;

    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(g_c.pcble_primary));

    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(g_c.pcble_secondary));

    int primary = -1;
    int secondary = -1;

    for (int i = 0; i < count; i++) {
        char label[64];

        snprintf(
            label,
            sizeof(label),
            "%s · %s",
            g_c.pcble_adapters[i].id,
            g_c.pcble_adapters[i].address);

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_primary),
            label);

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_secondary),
            label);

        if (preferred_primary[0] &&
            g_ascii_strcasecmp(
                preferred_primary,
                g_c.pcble_adapters[i].address) == 0) {
            primary = i;
        }

        if (preferred_secondary[0] &&
            g_ascii_strcasecmp(
                preferred_secondary,
                g_c.pcble_adapters[i].address) == 0) {
            secondary = i;
        }
    }

    if (count == 0) {
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_primary),
            "no Bluetooth adapter detected");

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_secondary),
            "no Bluetooth adapter detected");

        gtk_combo_box_set_active(
            GTK_COMBO_BOX(g_c.pcble_primary),
            0);

        gtk_combo_box_set_active(
            GTK_COMBO_BOX(g_c.pcble_secondary),
            0);
    } else {
        if (primary < 0) {
            primary = 0;
        }

        if (secondary < 0 ||
            secondary == primary) {
            secondary = primary;

            for (int i = 0; i < count; i++) {
                if (i != primary) {
                    secondary = i;
                    break;
                }
            }
        }

        gtk_combo_box_set_active(
            GTK_COMBO_BOX(g_c.pcble_primary),
            primary);

        gtk_combo_box_set_active(
            GTK_COMBO_BOX(g_c.pcble_secondary),
            secondary);
    }

    shell->loading = 0;

    pcble_update_controls(shell);
}

static void pcble_adapter_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    GtkShell *shell = user_data;

    if (shell->loading) {
        return;
    }

    int index =
        pcble_selected_adapter(widget);

    if (index < 0) {
        return;
    }

    const char *key =
        widget == g_c.pcble_primary
        ? "PCBLE_PRIMARY_ADAPTER"
        : "PCBLE_SECONDARY_ADAPTER";

    config_set_str(
        key,
        g_c.pcble_adapters[index].address);

    pcble_update_controls(shell);
}

static void pcble_controller_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;

    GtkShell *shell = user_data;

    if (shell->loading) {
        return;
    }

    const char *profile =
        pcble_profile();

    config_set_str(
        "PCBLE_CONTROLLER",
        profile);

    output_pcble_set_controller(
        profile);

    pcble_update_controls(shell);
}

static void pcble_mouse_side_changed(GtkWidget *widget, gpointer user_data)
{
    GtkShell *shell = user_data;
    if (shell->loading) return;
    int selected = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
    int side = selected == 1 ? 1 : 2;
    config_set_str("PCBLE_MOUSE_SIDE", side == 1 ? "left" : "right");
    output_pcble_set_mouse_side(side);
}

static void pcble_color_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    GtkShell *shell = user_data;

    if (shell->loading) {
        return;
    }

    const char *key = NULL;

    if (widget == g_c.pcble_body_color)
        key = "PCBLE_BODY_COLOR";
    else if (widget == g_c.pcble_button_color)
        key = "PCBLE_BUTTON_COLOR";
    else if (widget == g_c.pcble_left_grip_color)
        key = "PCBLE_LEFT_GRIP_COLOR";
    else if (widget == g_c.pcble_right_grip_color)
        key = "PCBLE_RIGHT_GRIP_COLOR";

    if (!key) {
        return;
    }

    char hex[7];
    pcble_color_hex(widget, hex);

    config_set_str(key, hex);
}

static void pcble_verbose_changed(
    GtkWidget *widget,
    gpointer user_data)
{
    GtkShell *shell = user_data;

    if (shell->loading) {
        return;
    }

    config_set_int(
        "PCBLE_VERBOSE",
        gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(widget))
            ? 1
            : 0);
}

static void pcble_start_pair(
    GtkShell *shell)
{
    int primary =
        pcble_selected_adapter(
            g_c.pcble_primary);

    if (primary < 0) {
        gtk_shell_show_error(
            shell,
            "Select a Bluetooth adapter first.");
        return;
    }

    const char *profile =
        pcble_profile();

    const int pair =
        pcble_profile_is_pair(profile);

    int secondary =
        pcble_selected_adapter(
            g_c.pcble_secondary);

    if (pair &&
        (secondary < 0 ||
         secondary == primary)) {
        gtk_shell_show_error(
            shell,
            "A Joy-Con pair requires two distinct Bluetooth adapters.");
        return;
    }

    char body[7];
    char buttons[7];
    char left[7];
    char right[7];

    pcble_color_hex(
        g_c.pcble_body_color,
        body);

    pcble_color_hex(
        g_c.pcble_button_color,
        buttons);

    pcble_color_hex(
        g_c.pcble_left_grip_color,
        left);

    pcble_color_hex(
        g_c.pcble_right_grip_color,
        right);

    config_set_str(
        "PCBLE_CONTROLLER",
        profile);

    config_set_str(
        "PCBLE_PRIMARY_ADAPTER",
        g_c.pcble_adapters[primary].address);

    if (pair) {
        config_set_str(
            "PCBLE_SECONDARY_ADAPTER",
            g_c.pcble_adapters[secondary].address);
    }

    char error[256];

    const int ok =
        output_pcble_start_session(
            g_c.pcble_adapters[primary].id,
            pair
                ? g_c.pcble_adapters[secondary].id
                : NULL,
            profile,
            NULL,
            body,
            buttons,
            left,
            right,
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(
                    g_c.pcble_verbose)),
            error,
            sizeof(error));

    if (!ok) {
        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            error);

        gtk_shell_show_error(
            shell,
            error);

        return;
    }

    gtk_label_set_text(
        GTK_LABEL(g_c.pcble_status),
        "waiting for pairing — open Controllers → Change Grip/Order on the Switch");

    pcble_update_controls(shell);
}

static void pcble_pair_clicked(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;
    pcble_start_pair(user_data);
}

static void pcble_reconnect_clicked(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;

    GtkShell *shell =
        user_data;

    /*
     * No Bluetooth logic lives here.
     *
     * This is intentionally the SAME call used by Maintenance,
     * remote clients and startup.
     */
    gamepad_bridge_reset();

    gtk_label_set_text(
        GTK_LABEL(g_c.pcble_status),
        "reconnect requested...");

    pcble_update_controls(shell);
}

static void pcble_stop_clicked(
    GtkWidget *widget,
    gpointer user_data)
{
    (void)widget;

    GtkShell *shell = user_data;

    output_pcble_stop_session();

    gtk_label_set_text(
        GTK_LABEL(g_c.pcble_status),
        "stopping Bluetooth session...");

    pcble_update_controls(shell);
}

static void pcble_load_config(GtkShell *shell)
{
    char profile[32];

    config_get_str(
        "PCBLE_CONTROLLER",
        profile,
        sizeof(profile),
        "pro");

    shell->loading = 1;

    static const char *const profiles[] = {
        "pro", "joycon-pair", "joycon-mixed", "joycon2-left",
        "joycon2-right", "joycon2-pair", "joycon-mixed-reverse"
    };
    int profile_index = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(profiles); ++i) {
        if (strcmp(profile, profiles[i]) == 0) {
            profile_index = (int)i;
            break;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.pcble_controller), profile_index);
    char mouse_side[16];
    const char *side = config_get_str("PCBLE_MOUSE_SIDE", mouse_side,
                                      sizeof(mouse_side), "right");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.pcble_mouse_side),
                             strcmp(side, "left") == 0 ? 1 : 0);

    config_get_str(
        "PCBLE_SWITCH_ADDRESS",
        g_c.pcble_paired_switch,
        sizeof(g_c.pcble_paired_switch),
        "");

    config_get_str(
        "PCBLE_PAIRED_ADAPTER",
        g_c.pcble_paired_adapter,
        sizeof(g_c.pcble_paired_adapter),
        "");

    gtk_label_set_text(
        GTK_LABEL(g_c.pcble_paired),
        g_c.pcble_paired_switch[0]
            ? g_c.pcble_paired_switch
            : "not paired yet");

    pcble_set_color_from_config(
        g_c.pcble_body_color,
        "PCBLE_BODY_COLOR",
        "828282");

    pcble_set_color_from_config(
        g_c.pcble_button_color,
        "PCBLE_BUTTON_COLOR",
        "0F0F0F");

    pcble_set_color_from_config(
        g_c.pcble_left_grip_color,
        "PCBLE_LEFT_GRIP_COLOR",
        "828282");

    pcble_set_color_from_config(
        g_c.pcble_right_grip_color,
        "PCBLE_RIGHT_GRIP_COLOR",
        "828282");

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(g_c.pcble_verbose),
        config_get_int(
            "PCBLE_VERBOSE",
            0,
            0,
            1));

    shell->loading = 0;

    output_pcble_set_controller(
        profile);

    pcble_refresh_adapters(
        NULL,
        shell);

    pcble_update_controls(shell);
}

static void pcble_refresh_status(GtkShell *shell)
{
    if (!g_c.pcble_status) {
        return;
    }

    char peer[64] = {0};

    output_pcble_peer(
        peer,
        sizeof(peer));

    const int ipc =
        output_pcble_ipc_up();

    const int connected =
        gamepad_bridge_link_up();

    const int running =
        output_pcble_session_running();

    const int recovery_pending =
        output_pcble_reconnect_pending();

    const int reconnect_active =
        output_pcble_reconnect_active();

    const int stopping =
        output_pcble_session_stopping();

    const char *profile =
        pcble_profile();

    if (recovery_pending && running) {
        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            stopping
                ? "reconnect requested — stopping current Bluetooth session..."
                : "reconnect requested...");
    } else if (recovery_pending) {
        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            "reconnect queued...");
    } else if (ipc && connected) {
        char text[128];

        snprintf(
            text,
            sizeof(text),
            peer[0]
                ? "connected — %s"
                : "connected",
            peer);

        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            text);

        /*
         * The Pro backend reports the real console Bluetooth address.
         * Remember it together with the physical adapter identity so a
         * later reconnect cannot accidentally use another dongle.
         */
        if (pcble_profile_reconnect_supported(profile) &&
            !pcble_profile_is_ble_only(profile) &&
            pcble_mac_valid(peer)) {
            int primary = pcble_selected_adapter(g_c.pcble_primary);
            if (strcmp(profile, "joycon-mixed-reverse") == 0)
                primary = pcble_selected_adapter(g_c.pcble_secondary);

            if (primary >= 0) {
                const char *adapter =
                    g_c.pcble_adapters[primary].address;

                if (g_ascii_strcasecmp(
                        g_c.pcble_paired_switch,
                        peer) != 0 ||
                    g_ascii_strcasecmp(
                        g_c.pcble_paired_adapter,
                        adapter) != 0) {
                    snprintf(
                        g_c.pcble_paired_switch,
                        sizeof(g_c.pcble_paired_switch),
                        "%s",
                        peer);

                    snprintf(
                        g_c.pcble_paired_adapter,
                        sizeof(g_c.pcble_paired_adapter),
                        "%s",
                        adapter);

                    config_set_str(
                        "PCBLE_SWITCH_ADDRESS",
                        peer);

                    config_set_str(
                        "PCBLE_PAIRED_ADAPTER",
                        adapter);

                    gtk_label_set_text(
                        GTK_LABEL(g_c.pcble_paired),
                        peer);
                }
                /* Persist the Classic profile independently from its peer
                 * and adapter addresses. A stale Pro bond is never reused
                 * for a Joy-Con 1 pair after a profile change. */
                char marker[32];
                const char *saved_profile = config_get_str(
                    "PCBLE_PAIRED_PROFILE", marker, sizeof(marker), "");
                if (strcmp(saved_profile, profile) != 0)
                    config_set_str("PCBLE_PAIRED_PROFILE", profile);
            }
        }
    } else if (reconnect_active) {
        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            "reconnecting to paired Switch...");
    } else if (ipc) {
        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            "Bluetooth session active — waiting for Switch");
    } else if (running) {
        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            "starting Bluetooth session / waiting for authorization...");
    } else {
        gtk_label_set_text(
            GTK_LABEL(g_c.pcble_status),
            "offline");
    }

    pcble_update_controls(shell);
}


/* --- stick shaping preview ------------------------------------------ */

static void queue_stick_preview_group(int output)
{
    GtkWidget **preview =
        output
            ? g_c.output_stick_preview
            : g_c.input_stick_preview;

    for (int i = 0; i < 2; i++) {
        if (preview[i]) {
            gtk_widget_queue_draw(preview[i]);
        }
    }
}

static void on_stick_preview_input_changed(GtkWidget *w, gpointer user_data)
{
    (void)w;
    (void)user_data;
    queue_stick_preview_group(0);
}

static void on_stick_preview_output_changed(GtkWidget *w, gpointer user_data)
{
    (void)w;
    (void)user_data;
    queue_stick_preview_group(1);
}

static void preview_attach_input_signal(GtkWidget *w, const char *signal)
{
    g_signal_connect(
        w,
        signal,
        G_CALLBACK(on_stick_preview_input_changed),
        NULL);
}

static void preview_attach_output_signal(GtkWidget *w, const char *signal)
{
    g_signal_connect(
        w,
        signal,
        G_CALLBACK(on_stick_preview_output_changed),
        NULL);
}

static void preview_get_values(
    GtkWidget *area,
    int *deadzone,
    int *range,
    int *diagonal)
{
    GtkShell *shell =
        g_object_get_data(
            G_OBJECT(area),
            "c2c-preview-shell");

    const int output =
        GPOINTER_TO_INT(
            g_object_get_data(
                G_OBJECT(area),
                "c2c-preview-output"));

    const int side =
        GPOINTER_TO_INT(
            g_object_get_data(
                G_OBJECT(area),
                "c2c-preview-side"));

    AppSettings settings;

    SDL_LockMutex(shell->lock);
    settings = shell->settings;
    SDL_UnlockMutex(shell->lock);

    if (output) {
        *deadzone = settings.output_stick_deadzone[side];
        *range = settings.output_stick_range[side];
        *diagonal = settings.output_stick_diagonal[side];
    } else {
        *deadzone = settings.stick_deadzone[side];
        *range = settings.stick_range[side];
        *diagonal = settings.stick_diagonal[side];
    }
}

static double preview_sat_for_angle(
    double angle,
    int range_pct,
    int diagonal_pct)
{
    const double x = fabs(cos(angle));
    const double y = fabs(sin(angle));

    const double peak = x > y ? x : y;
    const double least = x > y ? y : x;

    const double diagonality =
        peak > 0.00001
            ? least / peak
            : 0.0;

    return (range_pct +
           (diagonal_pct - range_pct) * diagonality) / 100.0;
}

/* The visible guide circle (including the 1px outline) has diameter N-2
 * inside an NxN drawing surface. The preview frame label is outside it. */
static void wozt_stick_preview_geometry(int w, int h,
                                        double *cx, double *cy, double *radius)
{
    const double guide_stroke = 1.0;
    const double inset = 1.0;
    *cx = w / 2.0;
    *cy = h / 2.0;
    *radius = MAX(0.0, (MIN(w, h) - 2.0 * inset - guide_stroke) / 2.0);
}

static gboolean stick_preview_draw(
    GtkWidget *area,
    cairo_t *cr,
    gpointer user_data)
{
    (void)user_data;

    /* Read the active WOZT JSON palette on each draw: this also works if
     * a new theme was selected while the window was already visible. */
    GtkShell *preview_shell = g_object_get_data(G_OBJECT(area), "c2c-preview-shell");
    WoztUiStandard *theme = preview_shell ? preview_shell->wozt : NULL;
    GdkRGBA preview_bg = {0.10, 0.10, 0.10, 1.0};
    GdkRGBA preview_border = {0.65, 0.65, 0.65, 1.0};
    GdkRGBA preview_muted = {0.60, 0.60, 0.60, 1.0};
    GdkRGBA preview_accent = {0.22, 0.56, 0.98, 1.0};
    GdkRGBA preview_negative = {0.95, 0.25, 0.25, 1.0};
    GdkRGBA preview_text = {0.92, 0.92, 0.92, 1.0};
    wozt_ui_standard_get_color(theme, "entry", &preview_bg);
    wozt_ui_standard_get_color(theme, "border", &preview_border);
    wozt_ui_standard_get_color(theme, "muted", &preview_muted);
    wozt_ui_standard_get_color(theme, "accent", &preview_accent);
    wozt_ui_standard_get_color(theme, "negative", &preview_negative);
    wozt_ui_standard_get_color(theme, "text", &preview_text);
    GdkRGBA translucent = preview_muted;

    int dz = 0;
    int range = 100;
    int diagonal = 100;

    preview_get_values(
        area,
        &dz,
        &range,
        &diagonal);

    const int w =
        gtk_widget_get_allocated_width(area);

    const int h =
        gtk_widget_get_allocated_height(area);

    double cx = 0.0, cy = 0.0, radius = 0.0;
    wozt_stick_preview_geometry(w, h, &cx, &cy, &radius);

    gdk_cairo_set_source_rgba(cr, &preview_bg);
    cairo_paint(cr);
    /* A real 70px square, not a GTK frame with an oversized inset. */
    cairo_set_line_width(cr, 1.0);
    gdk_cairo_set_source_rgba(cr, &preview_border);
    cairo_rectangle(cr, 0.5, 0.5, MAX(0, w - 1), MAX(0, h - 1));
    cairo_stroke(cr);

    /* Guide circle: 68px visible diameter for a 70px square. */
    cairo_set_line_width(cr, 1.0);
    gdk_cairo_set_source_rgba(cr, &preview_border);
    cairo_arc(cr, cx, cy, radius, 0.0, 2.0 * G_PI);
    cairo_stroke(cr);

    /* axes */
    translucent = preview_muted;
    translucent.alpha = 0.45;
    gdk_cairo_set_source_rgba(cr, &translucent);
    cairo_move_to(cr, cx - radius, cy);
    cairo_line_to(cr, cx + radius, cy);
    cairo_move_to(cr, cx, cy - radius);
    cairo_line_to(cr, cx, cy + radius);
    cairo_stroke(cr);

    /* diagonals */
    double dash[2] = {4.0, 4.0};
    cairo_set_dash(cr, dash, 2, 0.0);
    cairo_move_to(
        cr,
        cx - radius * 0.7071,
        cy - radius * 0.7071);
    cairo_line_to(
        cr,
        cx + radius * 0.7071,
        cy + radius * 0.7071);
    cairo_move_to(
        cr,
        cx - radius * 0.7071,
        cy + radius * 0.7071);
    cairo_line_to(
        cr,
        cx + radius * 0.7071,
        cy - radius * 0.7071);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0.0);

    /* saturation shape */
    cairo_new_path(cr);

    for (int i = 0; i <= 128; i++) {
        const double a =
            (2.0 * G_PI * i) / 128.0;

        const double sat =
            preview_sat_for_angle(
                a,
                range,
                diagonal);

        const double px =
            cx + cos(a) * radius * sat;

        const double py =
            cy - sin(a) * radius * sat;

        if (i == 0)
            cairo_move_to(cr, px, py);
        else
            cairo_line_to(cr, px, py);
    }

    cairo_close_path(cr);
    translucent = preview_accent;
    translucent.alpha = 0.18;
    gdk_cairo_set_source_rgba(cr, &translucent);
    cairo_fill_preserve(cr);
    gdk_cairo_set_source_rgba(cr, &preview_accent);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);

    /* dead zone */
    const double dzr =
        radius * dz / 100.0;

    translucent = preview_negative;
    translucent.alpha = 0.20;
    gdk_cairo_set_source_rgba(cr, &translucent);
    cairo_arc(cr, cx, cy, dzr, 0.0, 2.0 * G_PI);
    cairo_fill_preserve(cr);
    gdk_cairo_set_source_rgba(cr, &preview_negative);
    cairo_set_line_width(cr, 1.4);
    cairo_stroke(cr);

    /* center dot */
    gdk_cairo_set_source_rgba(cr, &preview_text);
    cairo_arc(cr, cx, cy, 2.2, 0.0, 2.0 * G_PI);
    cairo_fill(cr);

    /* Numerical values belong in controls and the live tooltip,
     * not inside the square (which would displace or shrink the circle). */
    char line[96];
    snprintf(line, sizeof(line), "dz %d%%  ax %d%%  dg %d%%",
             dz, range, diagonal);
    gchar *old_tooltip = gtk_widget_get_tooltip_text(area);
    if (g_strcmp0(old_tooltip, line) != 0)
        gtk_widget_set_tooltip_text(area, line);
    g_free(old_tooltip);

    return FALSE;
}

static GtkWidget *make_stick_preview(
    GtkShell *shell,
    int output,
    int side,
    const char *title)
{
    GtkWidget *frame =
        gtk_frame_new(title);
    /* Cairo draws the exact square border; a GTK shadow would add another
     * outline and padding around the 70x70 drawing area. */
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);

    GtkWidget *area =
        gtk_drawing_area_new();

    /* Small but still legible: original 180px tile dominated the panel. */
    gtk_widget_set_size_request(
        area,
        116,
        116);

    gtk_container_add(
        GTK_CONTAINER(frame),
        area);

    g_object_set_data(
        G_OBJECT(area),
        "c2c-preview-shell",
        shell);

    g_object_set_data(
        G_OBJECT(area),
        "c2c-preview-output",
        GINT_TO_POINTER(output));

    g_object_set_data(
        G_OBJECT(area),
        "c2c-preview-side",
        GINT_TO_POINTER(side));

    g_signal_connect(
        area,
        "draw",
        G_CALLBACK(stick_preview_draw),
        NULL);

    if (output)
        g_c.output_stick_preview[side] = area;
    else
        g_c.input_stick_preview[side] = area;

    return frame;
}

static GtkWidget *make_stick_preview_row(
    GtkShell *shell,
    int output)
{
    GtkWidget *box =
        gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL,
            6);

    gtk_box_pack_start(
        GTK_BOX(box),
        make_stick_preview(
            shell,
            output,
            0,
            "Left stick"),
        FALSE,
        FALSE,
        0);

    gtk_box_pack_start(
        GTK_BOX(box),
        make_stick_preview(
            shell,
            output,
            1,
            "Right stick"),
        FALSE,
        FALSE,
        0);

    return box;
}

/* --- the controls ---------------------------------------------------- */

static void on_toggle(GtkWidget *w, gpointer user_data) {
    GtkShell *shell = user_data;
    const gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    SDL_LockMutex(shell->lock);
    if (w == g_c.stream_enabled)       shell->settings.stream_enabled = on;
    else if (w == g_c.switch_enabled)  shell->settings.switch_enabled = on;
    else if (w == g_c.old3ds_enabled) shell->settings.old3ds_enabled = on;
    else if (w == g_c.wiiu_pad_enabled) shell->settings.wiiu_pad_enabled = on;
    else if (w == g_c.wup_stream_enabled) shell->settings.wup_stream_enabled = on;
    else if (w == g_c.wiiu_console_enabled) shell->settings.wiiu_console_enabled = on;
    else if (w == g_c.gamepad_enabled) shell->settings.gamepad_enabled = on;
    else if (w == g_c.invert_ry)       shell->settings.invert_ry = on;
    else if (w == g_c.output_invert_ry) shell->settings.output_invert_ry = on;
    else if (w == g_c.direct_sink)     shell->settings.local_direct_sink = on;
    else if (w == g_c.muted)           shell->settings.local_muted = on;
    else if (w == g_c.vsync)           shell->settings.vsync = on;
    SDL_UnlockMutex(shell->lock);
    /* WUP and Nintendo-radio bridge are mutually exclusive. */
    if (on && !shell->loading && w == g_c.wup_stream_enabled && g_c.wiiu_pad_enabled)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.wiiu_pad_enabled), FALSE);
    if (on && !shell->loading && w == g_c.wiiu_pad_enabled && g_c.wup_stream_enabled)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.wup_stream_enabled), FALSE);
    publish(shell);
}

static void on_spin(GtkWidget *w, gpointer user_data) {
    GtkShell *shell = user_data;
    const int v = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));
    SDL_LockMutex(shell->lock);
    if (w == g_c.port) shell->settings.web_port = v;
    else if (w == g_c.switch_port) shell->settings.switch_port = v;
    else if (w == g_c.wiiu_pad_port) shell->settings.wiiu_pad_port = v;
    else if (w == g_c.wiiu_console_port) shell->settings.wiiu_console_port = v;
    else if (w == g_c.old3ds_port) shell->settings.old3ds_port = v;
    SDL_UnlockMutex(shell->lock);
    publish(shell);
}

static void on_scale(GtkWidget *w, gpointer user_data) {
    GtkShell *shell = user_data;
    const int v = (int)gtk_range_get_value(GTK_RANGE(w));
    SDL_LockMutex(shell->lock);
    if (w == g_c.bitrate)           shell->settings.bitrate_mbps = v;
    else if (w == g_c.wiiu_pad_bitrate)     shell->settings.wiiu_pad_bitrate_mbps = v;
    else if (w == g_c.wiiu_console_bitrate) shell->settings.wiiu_console_bitrate_mbps = v;
    else if (w == g_c.lt_threshold) shell->settings.lt_threshold = v;
    else if (w == g_c.rt_threshold) shell->settings.rt_threshold = v;
    else if (w == g_c.deadzone[0])  shell->settings.stick_deadzone[0] = v;
    else if (w == g_c.deadzone[1])  shell->settings.stick_deadzone[1] = v;
    else if (w == g_c.range[0])     shell->settings.stick_range[0] = v;
    else if (w == g_c.range[1])     shell->settings.stick_range[1] = v;
    else if (w == g_c.diagonal[0])  shell->settings.stick_diagonal[0] = v;
    else if (w == g_c.diagonal[1])  shell->settings.stick_diagonal[1] = v;
    else if (w == g_c.output_lt_threshold) shell->settings.output_lt_threshold = v;
    else if (w == g_c.output_rt_threshold) shell->settings.output_rt_threshold = v;
    else if (w == g_c.output_deadzone[0]) shell->settings.output_stick_deadzone[0] = v;
    else if (w == g_c.output_deadzone[1]) shell->settings.output_stick_deadzone[1] = v;
    else if (w == g_c.output_range[0]) shell->settings.output_stick_range[0] = v;
    else if (w == g_c.output_range[1]) shell->settings.output_stick_range[1] = v;
    else if (w == g_c.output_diagonal[0]) shell->settings.output_stick_diagonal[0] = v;
    else if (w == g_c.output_diagonal[1]) shell->settings.output_stick_diagonal[1] = v;
    else if (w == g_c.volume)       shell->settings.local_volume = v;
    else if (w == g_c.brightness)   shell->settings.brightness = v;
    else if (w == g_c.contrast)     shell->settings.contrast = v;
    SDL_UnlockMutex(shell->lock);
    publish(shell);
}

/* Asks before changing what the adapter emulates, then watches the
 * replug it requires.
 *
 * Asking first, because the change is not a setting that can be tried
 * and undone: it costs a trip to the machine with both cables, and
 * picking the wrong row by accident should not start that. Watching
 * after, because the write returning proves nothing -- until the adapter
 * has been out and back the pad stays dead, and a dialog that said
 * "done" at that point would be lying.
 *
 * Not modal: the adapter is handled while this is on screen, and the
 * "adapter sees" line behind it is the thing worth being able to read.
 */
static void replug_show(GtkShell *shell, int wanted);

static void replug_set_phase(int confirming) {
    if (g_c.replug_cancel) gtk_widget_set_visible(g_c.replug_cancel, confirming);
    if (g_c.replug_apply)  gtk_widget_set_visible(g_c.replug_apply, confirming);
    if (g_c.replug_close)  gtk_widget_set_visible(g_c.replug_close, !confirming);
}

static void on_replug_response(GtkWidget *dialog, gint response, gpointer user_data) {
    GtkShell *shell = user_data;

    if (response == GTK_RESPONSE_ACCEPT && g_c.replug_pending >= 0) {
        SDL_LockMutex(shell->lock);
        shell->settings.output_protocol = g_c.replug_pending;
        SDL_UnlockMutex(shell->lock);
        publish(shell);
        g_c.replug_stage = REPLUG_WAIT_UNPLUG;
        g_c.replug_down_ticks = 0;
        gtk_label_set_text(GTK_LABEL(g_c.replug_label),
            "Unplug the adapter -- console side and this side.");
        replug_set_phase(FALSE);
        gtk_widget_set_sensitive(g_c.replug_close, FALSE);
        gtk_widget_show_all(dialog);
        replug_set_phase(FALSE);
        return;
    }

    if (g_c.replug_stage == REPLUG_CONFIRM) {
        /* Put the list back where it was. Guarded, or setting it would
         * come straight back here as a fresh change. */
        int current;
        SDL_LockMutex(shell->lock);
        current = shell->settings.output_protocol;
        SDL_UnlockMutex(shell->lock);
        shell->loading = 1;
        if (current >= 0) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.output_protocol), current);
        }
        shell->loading = 0;
    }
    gtk_widget_hide(dialog);
    g_c.replug_stage = REPLUG_IDLE;
    g_c.replug_pending = -1;
}

static void replug_show(GtkShell *shell, int wanted) {
    if (!g_c.replug_dialog) {
        g_c.replug_dialog = gtk_dialog_new_with_buttons(
            "Change what the adapter emulates", GTK_WINDOW(shell->settings_window),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            "Cancel", GTK_RESPONSE_CANCEL, "Change", GTK_RESPONSE_ACCEPT,
            "Close", GTK_RESPONSE_CLOSE, NULL);
        gtk_window_set_default_size(GTK_WINDOW(g_c.replug_dialog), 400, -1);
        g_c.replug_label = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(g_c.replug_label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_c.replug_label), 46);
        gtk_label_set_xalign(GTK_LABEL(g_c.replug_label), 0.0);
        gtk_widget_set_margin_start(g_c.replug_label, 14);
        gtk_widget_set_margin_end(g_c.replug_label, 14);
        gtk_widget_set_margin_top(g_c.replug_label, 12);
        gtk_widget_set_margin_bottom(g_c.replug_label, 12);
        gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(g_c.replug_dialog))),
                          g_c.replug_label);
        g_c.replug_cancel = gtk_dialog_get_widget_for_response(GTK_DIALOG(g_c.replug_dialog),
                                                               GTK_RESPONSE_CANCEL);
        g_c.replug_apply = gtk_dialog_get_widget_for_response(GTK_DIALOG(g_c.replug_dialog),
                                                              GTK_RESPONSE_ACCEPT);
        g_c.replug_close = gtk_dialog_get_widget_for_response(GTK_DIALOG(g_c.replug_dialog),
                                                              GTK_RESPONSE_CLOSE);
        /* Closing the window must hide it, not destroy it: it is reused
         * on the next change, and reopening a destroyed widget crashes. */
        g_signal_connect(g_c.replug_dialog, "response", G_CALLBACK(on_replug_response), shell);
        g_signal_connect(g_c.replug_dialog, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    }
    g_c.replug_pending = wanted;
    g_c.replug_stage = REPLUG_CONFIRM;

    char text[320];
    snprintf(text, sizeof(text),
             "Make the adapter emulate %s?\n\n"
             "You will then have to unplug it from the console and from this "
             "machine and plug both back in -- it does not take effect until "
             "you do.",
             gamepad_protocol_label(wanted));
    gtk_label_set_text(GTK_LABEL(g_c.replug_label), text);
    gtk_widget_show_all(g_c.replug_dialog);
    replug_set_phase(TRUE);
    gtk_window_present(GTK_WINDOW(g_c.replug_dialog));
}

static void on_combo(GtkWidget *w, gpointer user_data) {
    GtkShell *shell = user_data;
    const int i = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
    int asked_protocol = -1;
    SDL_LockMutex(shell->lock);
    if (w == g_c.resolution) {
        static const int HEIGHTS[] = {1080, 720, 480};
        if (i >= 0 && i < 3) shell->settings.browser_height = HEIGHTS[i];
    } else if (w == g_c.wup_touch_mode) {
        if (i >= 0 && i <= 2) shell->settings.wup_touch_mode = i;
    } else if (w == g_c.wiiu_console_resolution) {
        static const int HEIGHTS[] = {1080, 720, 480};
        if (i >= 0 && i < 3) shell->settings.wiiu_console_height = HEIGHTS[i];
    } else if (w == g_c.capture_format) {
        shell->settings.capture_mjpeg = (i == 1);
    } else if (w == g_c.gamepad_device) {
        shell->settings.gamepad_index = i - 1; /* the first row is "none" */
    } else if (w == g_c.output_backend) {
        if (i >= 0 &&
            i < gamepad_bridge_backend_count() &&
            gamepad_bridge_backend_available(i)) {
            shell->settings.output_backend = i;
        }
    } else if (w == g_c.output_protocol) {
        /* Deliberately NOT applied here. The rows are the protocol
         * values in order, but picking one is a request, not a setting:
         * it costs a trip to the machine with both cables, so it is
         * confirmed first and applied from the dialog. */
        if (i >= 0 && i != shell->settings.output_protocol) {
            asked_protocol = i;
        }
    }
    SDL_UnlockMutex(shell->lock);
    if (asked_protocol >= 0) {
        if (!shell->loading) {
            replug_show(shell, asked_protocol);
        }
        return; /* nothing else moved */
    }
    publish(shell);
}

static void on_action_button(GtkWidget *w, gpointer user_data) {
    GtkShell *shell = user_data;
    act(shell, (GtkShellAction)(intptr_t)g_object_get_data(G_OBJECT(w), "action"));
}

/* --- building the window --------------------------------------------- */

/* Defined below, beside the other things the radio page drives. */
static void on_pair_clicked(GtkWidget *w, gpointer user_data);

/* Keep controls at usable widths. gtk_widget_set_size_request() alone
 * only sets a minimum: the previous hexpand=TRUE stretched sliders,
 * combo boxes and color selectors across whole dashboard columns. */
/* A real two-column layout INSIDE the controller sections. Keep the
 * original GTK widget objects, callbacks, and setting pointers unchanged.
 * The wrapper's orientation is changed by configure-event at narrow widths. */
static GtkWidget *wozt_split_inner_section(GtkWidget *left, int output)
{
    GtkWidget *parent = gtk_widget_get_parent(left);
    if (!GTK_IS_BOX(parent)) return left;

    GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *right = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(right), 3);
    gtk_grid_set_column_spacing(GTK_GRID(right), 6);
    gtk_widget_set_hexpand(right, TRUE);

    g_object_ref(left);
    gtk_container_remove(GTK_CONTAINER(parent), left);
    gtk_box_pack_start(GTK_BOX(parent), columns, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(columns), left, TRUE, TRUE, 0);
    g_object_unref(left);
    gtk_box_pack_start(GTK_BOX(columns), right, TRUE, TRUE, 0);
    if (output) g_c.wozt_inner_output = columns;
    else g_c.wozt_inner_input = columns;
    return right;
}

/* An empty-label row carrying only instructional prose is not a control.
 * Keep its full English source as a tooltip on the preceding field.
 * The JSON translator already localizes tooltips during initial display and
 * after live language changes, using the original text as a stable key. */
static GtkWidget *wozt_attach_row_help(GtkWidget *grid, int row,
                                       const char *explanation)
{
    GtkWidget *label = NULL;
    GtkWidget *value = NULL;
    for (int y = row - 1; y >= 0; --y) {
        GtkWidget *candidate = gtk_grid_get_child_at(GTK_GRID(grid), 0, y);
        if (GTK_IS_LABEL(candidate)) {
            label = candidate;
            value = gtk_grid_get_child_at(GTK_GRID(grid), 1, y);
            break;
        }
        if (!value) value = gtk_grid_get_child_at(GTK_GRID(grid), 1, y);
    }
    if (label) gtk_widget_set_tooltip_text(label, explanation);
    if (value) {
        gchar *existing = gtk_widget_get_tooltip_text(value);
        if (!existing) gtk_widget_set_tooltip_text(value, explanation);
        g_free(existing);
    }
    return value ? value : label;
}

static GtkWidget *add_row(GtkWidget *grid, int row, const char *label, GtkWidget *control) {
    if (GTK_IS_LABEL(control) && !gtk_label_get_selectable(GTK_LABEL(control)) &&
        (!label || !*label || g_strcmp0(label, "profile") == 0)) {
        const char *explanation = gtk_label_get_text(GTK_LABEL(control));
        GtkWidget *context = wozt_attach_row_help(grid, row, explanation);
        gtk_widget_destroy(control);
        return context;
    }
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    /* Never increase row/button height because a field title wrapped.
     * Full names remain accessible and translated in the tooltip. */
    gtk_label_set_line_wrap(GTK_LABEL(l), FALSE);
    gtk_label_set_single_line_mode(GTK_LABEL(l), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 18);
    if (label && *label) gtk_widget_set_tooltip_text(l, label);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);

    gtk_widget_set_hexpand(control, FALSE);
    gtk_widget_set_halign(control, GTK_ALIGN_START);
    if (GTK_IS_SCALE(control)) {
        gtk_widget_set_size_request(control, 158, -1);
    } else if (GTK_IS_COMBO_BOX(control)) {
        /* Bound the visible cell, not only the combo's minimum width.
         * Long names are ellipsized; existing tooltips show details. */
        gtk_widget_set_size_request(control, 200, -1);
        GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(control));
        for (GList *c = cells; c; c = c->next) {
            if (GTK_IS_CELL_RENDERER_TEXT(c->data)) {
                gtk_cell_renderer_set_fixed_size(GTK_CELL_RENDERER(c->data), 176, -1);
                g_object_set(c->data, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
            }
        }
        g_list_free(cells);
    } else if (GTK_IS_SPIN_BUTTON(control)) {
        gtk_widget_set_size_request(control, 125, -1);
    } else if (GTK_IS_ENTRY(control)) {
        gtk_widget_set_size_request(control, 210, -1);
    } else if (GTK_IS_LABEL(control)) {
        gtk_label_set_line_wrap(GTK_LABEL(control), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(control), 46);
    }
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
    return control;
}

/* Move a pair of related controls into the same row in columns 0..3.
 * No widget is recreated: g_c pointers, GTK signals and saved settings
 * remain valid. Empty original rows collapse naturally in GtkGrid. */
static gboolean wozt_find_grid_row(GtkWidget *grid, const char *name,
                                    GtkWidget **label, GtkWidget **control, int *row)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(grid));
    gboolean found = FALSE;
    for (GList *it = children; it && !found; it = it->next) {
        GtkWidget *child = it->data;
        if (!GTK_IS_LABEL(child) || g_strcmp0(gtk_label_get_text(GTK_LABEL(child)), name))
            continue;
        gint x = -1, y = -1;
        gtk_container_child_get(GTK_CONTAINER(grid), child,
                                "left-attach", &x, "top-attach", &y, NULL);
        if (x != 0) continue;
        GtkWidget *other = gtk_grid_get_child_at(GTK_GRID(grid), 1, y);
        if (!other) continue;
        *label = child; *control = other; *row = y;
        found = TRUE;
    }
    g_list_free(children);
    return found;
}

static void wozt_pair_named_rows(GtkWidget *grid, const char *left, const char *right)
{
    GtkWidget *l0 = NULL, *c0 = NULL, *l1 = NULL, *c1 = NULL;
    int row0 = -1, row1 = -1;
    if (!wozt_find_grid_row(grid, left, &l0, &c0, &row0) ||
        !wozt_find_grid_row(grid, right, &l1, &c1, &row1) || row0 == row1)
        return;
    g_object_ref(l1);
    g_object_ref(c1);
    gtk_container_remove(GTK_CONTAINER(grid), l1);
    gtk_container_remove(GTK_CONTAINER(grid), c1);
    gtk_grid_attach(GTK_GRID(grid), l1, 2, row0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), c1, 3, row0, 1, 1);
    g_object_unref(l1);
    g_object_unref(c1);
}

/* WOZT_0836_CONTROLLER_OUTPUT_COMPACT */
static void wozt_unellipsize_tree(GtkWidget *widget)
{
    if (!widget) return;
    if (GTK_IS_LABEL(widget)) {
        gtk_label_set_line_wrap(GTK_LABEL(widget), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_NONE);
        gtk_label_set_max_width_chars(GTK_LABEL(widget), -1);
        gtk_widget_set_halign(widget, GTK_ALIGN_START);
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *it = children; it; it = it->next)
            wozt_unellipsize_tree(GTK_WIDGET(it->data));
        g_list_free(children);
    }
}

static void wozt_limit_scale_tree(GtkWidget *widget, int width)
{
    if (!widget) return;
    if (GTK_IS_SCALE(widget)) {
        gtk_widget_set_size_request(widget, width, -1);
        gtk_widget_set_hexpand(widget, FALSE);
        gtk_widget_set_halign(widget, GTK_ALIGN_START);
        return;
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *it = children; it; it = it->next)
            wozt_limit_scale_tree(GTK_WIDGET(it->data), width);
        g_list_free(children);
    }
}

static void wozt_compact_button_widget(GtkWidget *button)
{
    if (!button) return;
    gtk_widget_set_hexpand(button, FALSE);
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    if (GTK_IS_BIN(button)) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(button));
        if (GTK_IS_LABEL(child)) {
            gtk_label_set_line_wrap(GTK_LABEL(child), FALSE);
            gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_NONE);
        }
    }
}

static void wozt_compact_session_buttons(GtkWidget *grid, const char *row_name)
{
    GtkWidget *label = NULL, *control = NULL;
    int row = -1;
    if (!wozt_find_grid_row(grid, row_name, &label, &control, &row) ||
        !GTK_IS_BOX(control))
        return;
    GtkWidget *buttons[] = {
        g_c.pcble_reconnect, g_c.pcble_pair,
        g_c.pcble_stop, g_c.pcble_refresh
    };
    for (guint i = 0; i < G_N_ELEMENTS(buttons); ++i)
        if (!buttons[i] || gtk_widget_get_parent(buttons[i]) != control)
            return;
    GtkWidget *tight = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(tight), 2);
    gtk_grid_set_column_spacing(GTK_GRID(tight), 4);
    gtk_widget_set_halign(tight, GTK_ALIGN_START);
    for (guint i = 0; i < G_N_ELEMENTS(buttons); ++i) {
        g_object_ref(buttons[i]);
        gtk_container_remove(GTK_CONTAINER(control), buttons[i]);
        wozt_compact_button_widget(buttons[i]);
        gtk_grid_attach(GTK_GRID(tight), buttons[i], i % 2, i / 2, 1, 1);
        g_object_unref(buttons[i]);
    }
    gtk_container_remove(GTK_CONTAINER(grid), control);
    gtk_container_remove(GTK_CONTAINER(grid), label);
    gtk_grid_attach(GTK_GRID(grid), tight, 0, row, 2, 1);
    gtk_widget_show_all(tight);
}

static void wozt_compact_color_button(GtkWidget *widget)
{
    if (!widget) return;
    gtk_widget_set_hexpand(widget, FALSE);
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    gtk_widget_set_size_request(widget, 42, -1);
}

static void wozt_compact_palette_rows(GtkWidget *grid)
{
    GtkWidget *body_l = NULL, *body_c = NULL;
    GtkWidget *left_l = NULL, *left_c = NULL;
    int row0 = -1, row1 = -1;
    if (!wozt_find_grid_row(grid, "body", &body_l, &body_c, &row0) ||
        !wozt_find_grid_row(grid, "left grip", &left_l, &left_c, &row1))
        return;
    /* Earlier WOZT pairing already moved "buttons" and "right grip" to
     * columns 2 and 3 of those same rows. Looking for them in column 0
     * returned nothing and left the large empty gap untouched. */
    GtkWidget *buttons_l = gtk_grid_get_child_at(GTK_GRID(grid), 2, row0);
    GtkWidget *buttons_c = gtk_grid_get_child_at(GTK_GRID(grid), 3, row0);
    GtkWidget *right_l = gtk_grid_get_child_at(GTK_GRID(grid), 2, row1);
    GtkWidget *right_c = gtk_grid_get_child_at(GTK_GRID(grid), 3, row1);
    if (!GTK_IS_LABEL(buttons_l) || !GTK_IS_COLOR_BUTTON(buttons_c) ||
        !GTK_IS_LABEL(right_l) || !GTK_IS_COLOR_BUTTON(right_c))
        return;
    GtkWidget *tight = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(tight), 2);
    gtk_grid_set_column_spacing(GTK_GRID(tight), 4);
    gtk_widget_set_halign(tight, GTK_ALIGN_START);

    GtkWidget *widgets[] = {body_l, body_c, buttons_l, buttons_c,
                            left_l, left_c, right_l, right_c};
    for (guint i = 0; i < G_N_ELEMENTS(widgets); ++i) {
        g_object_ref(widgets[i]);
        gtk_container_remove(GTK_CONTAINER(grid), widgets[i]);
    }
    wozt_compact_color_button(body_c);
    wozt_compact_color_button(buttons_c);
    wozt_compact_color_button(left_c);
    wozt_compact_color_button(right_c);
    gtk_grid_attach(GTK_GRID(tight), body_l,    0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(tight), body_c,    1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(tight), buttons_l, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(tight), buttons_c, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(tight), left_l,    0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(tight), left_c,    1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(tight), right_l,   2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(tight), right_c,   3, 1, 1, 1);
    for (guint i = 0; i < G_N_ELEMENTS(widgets); ++i)
        g_object_unref(widgets[i]);
    /* A two-column cell in the original grid replaces all four wide cells. */
    gtk_grid_attach(GTK_GRID(grid), tight, 0, row0, 2, 1);
    gtk_widget_show_all(tight);
}

static void wozt_unellipsize_pcble_combo(GtkWidget *combo, int width)
{
    if (!GTK_IS_COMBO_BOX(combo)) return;
    gtk_widget_set_size_request(combo, width, -1);
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo));
    for (GList *it = cells; it; it = it->next) {
        if (GTK_IS_CELL_RENDERER_TEXT(it->data)) {
            gtk_cell_renderer_set_fixed_size(GTK_CELL_RENDERER(it->data),
                                             MAX(48, width - 30), 18);
            g_object_set(it->data, "ellipsize", PANGO_ELLIPSIZE_NONE,
                         "single-paragraph-mode", TRUE,
                         "wrap-width", -1, NULL);
        }
    }
    g_list_free(cells);
}

/* WOZT_0838_STACK_OUTPUT_SLIDERS
 * Keep the eight controls in one narrow column, ordered by left then right.
 * Move the existing GTK widgets; do not recreate their signals/settings.
 * Validate all rows before detaching anything so a source variation cannot
 * silently discard a setting or leave a partially modified grid. */
static void wozt_stack_stick_sliders(GtkWidget *grid)
{
    if (!GTK_IS_GRID(grid)) return;
    const char *names[] = {
        "LT threshold (%)",
        "left stick deadzone (%)",
        "left axis saturation (%)",
        "left diagonal saturation (%)",
        "RT threshold (%)",
        "right stick deadzone (%)",
        "right axis saturation (%)",
        "right diagonal saturation (%)"
    };
    GtkWidget *labels[G_N_ELEMENTS(names)] = {NULL};
    GtkWidget *controls[G_N_ELEMENTS(names)] = {NULL};
    int first_row = G_MAXINT;
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        int row = -1;
        if (!wozt_find_grid_row(grid, names[i], &labels[i], &controls[i], &row)) {
            g_warning("wozt-ui: output slider field not found: %s", names[i]);
            return;
        }
        first_row = MIN(first_row, row);
    }
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        g_object_ref(labels[i]);
        g_object_ref(controls[i]);
        gtk_container_remove(GTK_CONTAINER(grid), labels[i]);
        gtk_container_remove(GTK_CONTAINER(grid), controls[i]);
    }
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gtk_grid_attach(GTK_GRID(grid), labels[i], 0, first_row + (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), controls[i], 1, first_row + (int)i, 1, 1);
        g_object_unref(labels[i]);
        g_object_unref(controls[i]);
    }
}

/* WOZT_0839_INLINE_OUTPUT_LAYOUT */
/* A short row with two widgets must not inherit the left grid's widest label
 * column (which is sized by "right Joy-Con adapter"). Keep both widgets,
 * their GTK signals, accessibility labels and live translations intact. */
static gboolean wozt_inline_output_row(GtkWidget *grid, const char *name)
{
    if (!GTK_IS_GRID(grid)) return FALSE;
    GtkWidget *label = NULL, *control = NULL;
    int row = -1;
    if (!wozt_find_grid_row(grid, name, &label, &control, &row))
        return FALSE;
    GtkWidget *inline_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_halign(inline_box, GTK_ALIGN_START);
    gtk_widget_set_hexpand(inline_box, FALSE);
    g_object_ref(label);
    g_object_ref(control);
    gtk_container_remove(GTK_CONTAINER(grid), label);
    gtk_container_remove(GTK_CONTAINER(grid), control);
    gtk_box_pack_start(GTK_BOX(inline_box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inline_box), control, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), inline_box, 0, row, 2, 1);
    g_object_unref(label);
    g_object_unref(control);
    gtk_widget_show_all(inline_box);
    return TRUE;
}

/* Use natural widths instead of TRUE/TRUE GtkBox packing, which left a large
 * empty strip between backend and calibration. The section FRAME still grows
 * with the responsive dashboard, and narrow views still stack vertically. */
static void wozt_pack_controller_columns_tightly(GtkWidget *columns)
{
    if (!GTK_IS_BOX(columns)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(columns));
    if (g_list_length(children) == 2) {
        for (GList *it = children; it; it = it->next) {
            GtkWidget *child = GTK_WIDGET(it->data);
            gtk_widget_set_hexpand(child, FALSE);
            gtk_widget_set_halign(child, GTK_ALIGN_START);
            gtk_box_set_child_packing(GTK_BOX(columns), child,
                                      FALSE, FALSE, 0, GTK_PACK_START);
        }
        gtk_box_set_spacing(GTK_BOX(columns), 2);
    }
    g_list_free(children);
}

/* WOZT_0841_COMPACT_PREVIEW */
/* "visual preview" is already an explanation on the section and preview;
 * the inline label only consumes space. Keep the framed stick widgets and
 * their signals, removing just that redundant text widget. */
static gboolean wozt_remove_preview_caption(GtkWidget *area)
{
    if (!GTK_IS_WIDGET(area)) return FALSE;
    GtkWidget *frame = gtk_widget_get_parent(area);
    GtkWidget *pair = frame ? gtk_widget_get_parent(frame) : NULL;
    GtkWidget *inline_row = pair ? gtk_widget_get_parent(pair) : NULL;
    if (!GTK_IS_BOX(inline_row)) return FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(inline_row));
    gboolean found = FALSE;
    for (GList *it = children; it; it = it->next) {
        GtkWidget *child = GTK_WIDGET(it->data);
        if (GTK_IS_LABEL(child) &&
            g_strcmp0(gtk_label_get_text(GTK_LABEL(child)), "visual preview") == 0) {
            gtk_widget_destroy(child);
            found = TRUE;
            break;
        }
    }
    g_list_free(children);
    gtk_box_set_spacing(GTK_BOX(inline_row), 0);
    gtk_widget_set_halign(inline_row, GTK_ALIGN_START);
    return found;
}

/* WOZT_0842_COMPACT_CALIBRATION
 * GTK3's native scale margins used to make adjacent rows tall. A forced
 * 14px widget without thin trough CSS instead made their green tracks merge.
 * Apply one scoped style to both sets of eight scales, keeping their model,
 * value labels and signals. The CSS gives a 3px track and 12px thumb core
 * (1px border on each side); a 15px row targets a 1px thumb-to-thumb gap. */
static void wozt_compact_calibration_scales(GtkWidget *const controls[8])
{
    for (int i = 0; i < 8; ++i) {
        GtkWidget *scale = controls[i];
        if (!GTK_IS_SCALE(scale)) continue;
        gtk_widget_set_margin_top(scale, 0);
        gtk_widget_set_margin_bottom(scale, 0);
        gtk_widget_set_valign(scale, GTK_ALIGN_FILL);
        gtk_widget_set_size_request(scale, 116, 15);
        gtk_style_context_add_class(gtk_widget_get_style_context(scale),
                                    "wozt-calibration-scale");
    }
}

static void wozt_compact_controller_output(void)
{
    if (GTK_IS_GRID(g_c.pcble_settings)) {
        gtk_grid_set_row_spacing(GTK_GRID(g_c.pcble_settings), 2);
        gtk_grid_set_column_spacing(GTK_GRID(g_c.pcble_settings), 4);
        wozt_unellipsize_tree(g_c.pcble_settings);
        /* PCBLE controls share Backend's right edge; full values remain in
         * their model and tooltips, with no ellipses or multiline rows. */
        wozt_unellipsize_pcble_combo(g_c.pcble_controller, 170);
        wozt_unellipsize_pcble_combo(g_c.pcble_primary, 170);
        wozt_unellipsize_pcble_combo(g_c.pcble_secondary, 170);
        wozt_unellipsize_pcble_combo(g_c.pcble_mouse_side, 170);
        wozt_compact_palette_rows(g_c.pcble_settings);
        wozt_compact_session_buttons(g_c.pcble_settings, "Bluetooth session");
    }
    /* The PCBLE logging row now sits in Diagnostics, above Output shaping. */
    GtkWidget *diagnostics_grid = g_c.pcble_verbose
        ? gtk_widget_get_parent(g_c.pcble_verbose) : NULL;
    if (!GTK_IS_GRID(diagnostics_grid) ||
        !wozt_inline_output_row(diagnostics_grid, "logging"))
        g_warning("wozt-ui: cannot inline right-column diagnostics logging");
    wozt_pack_controller_columns_tightly(g_c.wozt_inner_output);

    if (g_c.output_stick_preview[0])
        gtk_widget_set_size_request(g_c.output_stick_preview[0], 70, 70);
    if (g_c.output_stick_preview[1])
        gtk_widget_set_size_request(g_c.output_stick_preview[1], 70, 70);

    GtkWidget *shaping = NULL;
    if (g_c.wozt_inner_output && GTK_IS_BOX(g_c.wozt_inner_output)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(g_c.wozt_inner_output));
        shaping = children && children->next ? GTK_WIDGET(children->next->data) : NULL;
        g_list_free(children);
    }
    if (GTK_IS_GRID(shaping)) {
        gtk_grid_set_row_spacing(GTK_GRID(shaping), 0);
        gtk_grid_set_column_spacing(GTK_GRID(shaping), 2);
        wozt_stack_stick_sliders(shaping);
        wozt_unellipsize_tree(shaping);
        wozt_limit_scale_tree(shaping, 116);
        GtkWidget *output_scales[8] = {
            g_c.output_lt_threshold, g_c.output_deadzone[0],
            g_c.output_range[0], g_c.output_diagonal[0],
            g_c.output_rt_threshold, g_c.output_deadzone[1],
            g_c.output_range[1], g_c.output_diagonal[1]
        };
        wozt_compact_calibration_scales(output_scales);
        /* The preview must not inherit the long slider-label column width. */
        if (!wozt_inline_output_row(shaping, "visual preview"))
            g_warning("wozt-ui: cannot inline output visual preview");
        else if (!wozt_remove_preview_caption(g_c.output_stick_preview[0]))
            g_warning("wozt-ui: cannot remove redundant output preview label");
    }
}
/* WOZT_0842_SINGLE_COLUMN_INPUT
 * One shared grid: source/device, two previews and all calibration rows,
 * in that order. No inner two-column wrapper or separating panel. Existing
 * controls, callbacks and persisted settings are preserved. */
/* WOZT_0852_LOCAL_INPUT_MINUS_18PX
 * The two diagonal labels determine the minimum width of the input grid.
 * The screenshot measures ~20px between the final `100` glyph and its frame;
 * retain 2px. Compact the labels' glyph spacing by ~18px rather than
 * shorten the 116px scales, truncate text, or falsify responsive budgets.
 * -660 Pango units is ~0.64px between adjacent letters. */
static void wozt_tighten_local_diagonal_label(GtkWidget *grid, const char *name)
{
    GtkWidget *label = NULL, *control = NULL;
    int row = -1;
    if (!wozt_find_grid_row(grid, name, &label, &control, &row) ||
        !GTK_IS_LABEL(label)) {
        g_warning("wozt-ui: local diagonal calibration label not found: %s", name);
        return;
    }
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_letter_spacing_new(-660));
    gtk_label_set_attributes(GTK_LABEL(label), attrs);
    pango_attr_list_unref(attrs);
}

static void wozt_compact_controller_input(void)
{
    GtkWidget *grid = g_c.wozt_inner_input;
    if (!GTK_IS_GRID(grid)) {
        g_warning("wozt-ui: expected one shared Local input grid");
        return;
    }
    gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
    /* Recover 2px without shortening or clipping slider labels. */
    gtk_grid_set_column_spacing(GTK_GRID(grid), 0);
    wozt_unellipsize_tree(grid);
    /* WOZT_0853_INPUT_NATURAL_WIDTH: GTK's combobox renderer can request
     * more than its visible button. 190px still fits the standard local
     * device name, while avoiding an oversized row requisition. */
    wozt_unellipsize_pcble_combo(g_c.gamepad_device, 190);
    if (!wozt_inline_output_row(grid, "device")) {
        g_warning("wozt-ui: cannot inline local device row");
    } else {
        GtkWidget *device_row = gtk_widget_get_parent(g_c.gamepad_device);
        if (GTK_IS_BOX(device_row)) gtk_box_set_spacing(GTK_BOX(device_row), 1);
    }
    if (!wozt_inline_output_row(grid, "visual preview"))
        g_warning("wozt-ui: cannot inline local input preview");
    else if (!wozt_remove_preview_caption(g_c.input_stick_preview[0]))
        g_warning("wozt-ui: cannot remove redundant input preview label");

    for (int side = 0; side < 2; ++side)
        if (g_c.input_stick_preview[side])
            gtk_widget_set_size_request(g_c.input_stick_preview[side], 70, 70);

    wozt_stack_stick_sliders(grid);
    wozt_unellipsize_tree(grid);
    wozt_limit_scale_tree(grid, 116);
    GtkWidget *input_scales[8] = {
        g_c.lt_threshold, g_c.deadzone[0], g_c.range[0], g_c.diagonal[0],
        g_c.rt_threshold, g_c.deadzone[1], g_c.range[1], g_c.diagonal[1]
    };
    wozt_compact_calibration_scales(input_scales);
    wozt_tighten_local_diagonal_label(grid, "left diagonal saturation (%)");
    wozt_tighten_local_diagonal_label(grid, "right diagonal saturation (%)");

    /* The native checkbox can be wider than a 116px GtkScale, leaving
     * an invisible tail in column 1. Condense its LABEL, not the button
     * or its original signal, to keep the calibrated sliders aligned. */
    GtkWidget *invert_text = GTK_IS_BIN(g_c.invert_ry)
        ? gtk_bin_get_child(GTK_BIN(g_c.invert_ry)) : NULL;
    if (GTK_IS_LABEL(invert_text)) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_letter_spacing_new(-750));
        gtk_label_set_attributes(GTK_LABEL(invert_text), attrs);
        pango_attr_list_unref(attrs);
    }

    /* The rightmost dashboard section still expands responsively, but its
     * compact natural size no longer carries a redundant 2px inner margin. */
    GtkWidget *input_frame = g_c.wozt_sections[3];
    GtkWidget *input_outer = GTK_IS_BIN(input_frame)
        ? gtk_bin_get_child(GTK_BIN(input_frame)) : NULL;
    if (GTK_IS_WIDGET(input_outer)) gtk_widget_set_margin_end(input_outer, 0);
}

/* WOZT_0831_COMPACT_BUTTONS
 * GtkButton labels stay at their existing font size. Ellipsizing their
 * width instead of wrapping avoids double-height actions on a narrow row.
 * If a button had no tooltip, preserve its full original label there. */
static void wozt_compact_action_labels(GtkWidget *widget)
{
    if (widget == g_c.wozt_sections[2] ||
        widget == g_c.wozt_sections[3]) return;
    if (GTK_IS_BUTTON(widget) && GTK_IS_BIN(widget)) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
        if (GTK_IS_LABEL(child)) {
            const char *original = gtk_label_get_text(GTK_LABEL(child));
            if (original && *original) {
                gchar *existing = gtk_widget_get_tooltip_text(widget);
                if (!existing || !*existing)
                    gtk_widget_set_tooltip_text(widget, original);
                g_free(existing);
                gtk_label_set_line_wrap(GTK_LABEL(child), FALSE);
                gtk_label_set_single_line_mode(GTK_LABEL(child), TRUE);
                /* Overview's quick actions must NEVER show an ellipsis,
                 * including after switching the live interface language. */
                const gboolean full_text = gtk_style_context_has_class(
                    gtk_widget_get_style_context(widget),
                    "wozt-overview-action-fulltext");
                gtk_label_set_ellipsize(GTK_LABEL(child),
                    full_text ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END);
                gtk_label_set_max_width_chars(GTK_LABEL(child),
                    full_text ? -1 : 18);
            }
        }
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *it = children; it; it = it->next)
            wozt_compact_action_labels(GTK_WIDGET(it->data));
        g_list_free(children);
    }
}

static GtkWidget *make_scale(GtkShell *shell, int lo, int hi, int step, const char *suffix) {
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    gtk_widget_set_size_request(s, 136, -1);
    g_object_set_data(G_OBJECT(s), "suffix", (gpointer)suffix);
    g_signal_connect(s, "value-changed", G_CALLBACK(on_scale), shell);
    return s;
}

static GtkWidget *make_check(GtkShell *shell, const char *label) {
    GtkWidget *c = gtk_check_button_new_with_label(label);
    g_signal_connect(c, "toggled", G_CALLBACK(on_toggle), shell);
    return c;
}

static GtkWidget *make_button(GtkShell *shell, const char *label, GtkShellAction action,
                              const char *tooltip) {
    GtkWidget *b = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(b), "action", (gpointer)(intptr_t)action);
    gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", G_CALLBACK(on_action_button), shell);
    return b;
}

/* A client family's statistics line: selectable, wrapping, and dim until
 * the host has something to put in it. Written once a second from the
 * program, which is the only side that knows who is connected. */
/* The compact Overview summarizes absence without duplicating the full
 * wording of the corresponding Client tab. Never infer connectivity. */
static const char *wozt_overview_client_summary(const char *full)
{
    if (g_strcmp0(full, "nothing connected — not encoding") == 0)
        return "idle — no-enc";
    return full ? full : "";
}

static GtkWidget *add_client_status(GtkWidget *grid, int row, const char *label) {
    GtkWidget *w = gtk_label_new("nothing connected");
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(w), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(w), 52);
    add_row(grid, row, label, w);
    return w;
}

/* WOZT_0854_CLIENT_SVG: locally bundled open SVGs; no online UI dependency.
 * Keep icon size equal to the 11px WOZT font. Accessible full names and
 * tooltips survive replacing both status labels and notebook tab captions. */
static void wozt_asset_path(char *out, size_t out_size, const char *relative);

static GtkWidget *wozt_client_icon(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "assets/client-icons/%s.svg", name);
    char absolute[512];
    wozt_asset_path(absolute, sizeof(absolute), path);
    GError *err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
        absolute, 11, 11, TRUE, &err);
    GtkWidget *image = pixbuf
        ? gtk_image_new_from_pixbuf(pixbuf)
        : gtk_image_new_from_icon_name("applications-multimedia-symbolic",
                                       GTK_ICON_SIZE_MENU);
    if (pixbuf) g_object_unref(pixbuf);
    if (err) g_clear_error(&err);
    gtk_widget_set_size_request(image, 11, 11);
    return image;
}

static GtkWidget *wozt_client_icon_label(const char *title)
{
    const char *first = NULL, *second = NULL;
    if (strstr(title, "Browser")) first = "browser";
    else if (strstr(title, "Switch / Android")) {
        first = "switch";
        second = "android";
    } else if (strstr(title, "WUP Stream")) first = "wup";
    else if (strstr(title, "Switch")) first = "switch";
    else if (strstr(title, "Android")) first = "android";
    else if (strstr(title, "GamePad")) first = "gamepad";
    else if (strstr(title, "Wii U Console")) first = "wiiu";
    else if (strstr(title, "Old 3DS")) first = "3ds";
    if (!first) return gtk_label_new(title);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_halign(box, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), wozt_client_icon(first), FALSE, FALSE, 0);
    if (second)
        gtk_box_pack_start(GTK_BOX(box), wozt_client_icon(second), FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(box, title);
    AtkObject *accessible = gtk_widget_get_accessible(box);
    if (accessible) atk_object_set_name(accessible, title);
    gtk_widget_show_all(box);
    return box;
}

static void wozt_compact_client_widgets(GtkWidget *widget)
{
    if (!widget) return;
    if (GTK_IS_SPIN_BUTTON(widget)) {
        gtk_widget_set_size_request(widget, 93, -1);
        gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                    "wozt-client-port");
    } else if (GTK_IS_SCALE(widget)) {
        gtk_widget_set_size_request(widget, 116, -1);
        gtk_widget_set_hexpand(widget, FALSE);
    } else if (GTK_IS_COMBO_BOX(widget)) {
        gtk_widget_set_size_request(widget, 164, -1);
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *it = children; it; it = it->next)
            wozt_compact_client_widgets(GTK_WIDGET(it->data));
        g_list_free(children);
    }
}

/* WOZT_0859_SERVER_ICONS: each client line owns its own 3px icon-to-text
 * gap. A shared GtkGrid column must NEVER measure that gap: spanning server
 * labels and quick-action buttons used to stretch the icon column to 100px. */
static GtkWidget *add_client_icon_status(GtkWidget *list, const char *name)
{
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_halign(line, GTK_ALIGN_START);
    GtkWidget *icon = wozt_client_icon_label(name);
    gtk_box_pack_start(GTK_BOX(line), icon, FALSE, FALSE, 0);
    GtkWidget *status = gtk_label_new("idle — no-enc");
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(status), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(status), FALSE);
    gtk_label_set_single_line_mode(GTK_LABEL(status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(status), 34);
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(status, "nothing connected — not encoding");
    gtk_box_pack_start(GTK_BOX(line), status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(list), line, FALSE, FALSE, 0);
    return status;
}

/* The complete status strings still belong to the footer/tray and the two
 * original labels. This small indicator strip only visualizes that state;
 * it does not guess stream metrics or replace their source of truth. */
static void wozt_overview_update_indicator(GtkWidget *widget,
                                           const char *enabled_icon,
                                           const char *disabled_icon,
                                           gboolean enabled,
                                           const char *tooltip)
{
    if (!GTK_IS_IMAGE(widget)) return;
    gpointer state = GINT_TO_POINTER(enabled ? 2 : 1);
    if (g_object_get_data(G_OBJECT(widget), "wozt-indicator-state") != state) {
        const char *icon = enabled ? enabled_icon : disabled_icon;
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        if (theme && !gtk_icon_theme_has_icon(theme, icon))
            icon = "computer-symbolic"; /* already used by the Clients frame */
        gtk_image_set_from_icon_name(GTK_IMAGE(widget), icon, GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(widget), 11);
        gtk_widget_set_opacity(widget, enabled ? 1.0 : 0.45);
        g_object_set_data(G_OBJECT(widget), "wozt-indicator-state", state);
    }
    gchar *old = gtk_widget_get_tooltip_text(widget);
    if (g_strcmp0(old, tooltip) != 0) {
        gtk_widget_set_tooltip_text(widget, tooltip);
        AtkObject *accessible = gtk_widget_get_accessible(widget);
        if (accessible) atk_object_set_name(accessible, tooltip);
    }
    g_free(old);
}

static void wozt_overview_update_server_indicators(const char *full,
                                                   const char *backend)
{
    const gboolean running = full && g_str_has_prefix(full, "running");
    const gboolean capture = full && strstr(full, "capture online") != NULL;
    const gboolean browser = full && strstr(full, "browser server listening") != NULL;
    const gboolean native = full && strstr(full, "native server listening") != NULL;
    const char *run_text = full && g_str_has_prefix(full, "starting")
        ? "starting" : (running ? "running" : "stopped");
    gchar *server_tip = g_strdup_printf("Server: %s\nController output: %s",
        run_text, backend && *backend ? backend : "unavailable");
    wozt_overview_update_indicator(g_c.overview_indicators[0],
        "emblem-ok-symbolic", "dialog-warning-symbolic", running, server_tip);
    wozt_overview_update_indicator(g_c.overview_indicators[1],
        "camera-video-symbolic", "camera-video-symbolic", capture,
        capture ? "Capture: online" : "Capture: waiting / offline");
    wozt_overview_update_indicator(g_c.overview_indicators[2],
        "web-browser-symbolic", "web-browser-symbolic", browser,
        browser ? "Browser server: listening" : "Browser server: off");
    wozt_overview_update_indicator(g_c.overview_indicators[3],
        "network-server-symbolic", "network-server-symbolic", native,
        native ? "Native server: listening" : "Native server: off");
    g_free(server_tip);
}

static GtkWidget *make_page(const char *title, GtkWidget *notebook, GtkWidget **grid_out) {
    /* GtkNotebook otherwise reserves the height of the largest client tab
     * even when Browser (five small rows) is selected. Bound that height;
     * the contents of longer client tabs remain accessible by scrolling. */
    GtkWidget *grid = gtk_grid_new();
    gtk_widget_set_valign(grid, GTK_ALIGN_START);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 1);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 1);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 0);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_widget_set_size_request(scroll, 1, 190);
    gtk_container_add(GTK_CONTAINER(scroll), grid);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll,
                             wozt_client_icon_label(title));
    *grid_out = grid;
    return grid;
}

static void add_section_header(GtkWidget *grid, int row, const char *title) {
    GtkWidget *label = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(label),
                                "wozt-subsection");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_top(label, row == 0 ? 0 : 4);
    gtk_widget_set_margin_bottom(label, 1);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 2, 1);
}

static GtkWidget *make_stack_page(
    GtkWidget *dashboard,
    const char *name,
    const char *title,
    const char *subtitle,
    GtkWidget **grid_out)
{
    /* A former navigation page is now one framed functional section.
     * Clients retains its separate GtkNotebook below this frame. */
    static const char *const SECTION_ICONS[8] = {
        "view-dashboard-symbolic", "computer-symbolic", "input-gaming-symbolic",
        "input-gaming-symbolic", "camera-video-symbolic", "video-display-symbolic",
        "system-reboot-symbolic", "emblem-system-symbolic"
    };
    const int index = g_c.wozt_section_count++;
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "wozt-section");
    gtk_widget_set_hexpand(frame, TRUE);
    g_object_set_data_full(G_OBJECT(frame), "wozt-section-id", g_strdup(name), g_free);

    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *icon = gtk_image_new_from_icon_name(
        SECTION_ICONS[index < 8 ? index : 0], GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(heading), icon, FALSE, FALSE, 0);
    GtkWidget *text = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(text), "wozt-section-title");
    gtk_box_pack_start(GTK_BOX(heading), text, FALSE, FALSE, 0);
    gtk_frame_set_label_widget(GTK_FRAME(frame), heading);
    if (subtitle && *subtitle) {
        gtk_widget_set_tooltip_text(heading, subtitle);
        gtk_widget_set_tooltip_text(text, subtitle);
        GtkWidget *info = gtk_label_new("ⓘ");
        gtk_style_context_add_class(gtk_widget_get_style_context(info), "wozt-muted");
        gtk_widget_set_tooltip_text(info, subtitle);
        gtk_box_pack_start(GTK_BOX(heading), info, FALSE, FALSE, 2);
    }

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_start(outer, 5);
    /* Save 3px only in Local input; other dashboard sections keep 5px. */
    gtk_widget_set_margin_end(outer, index == 3 ? 2 : 5);
    /* The Clients notebook should sit 1px inside the section border
     * both vertically and use its full height, including wrapped rows. */
    gtk_widget_set_margin_top(outer, index == 1 ? 1 : 2);
    gtk_widget_set_margin_bottom(outer, index == 1 ? 1 : 3);
    if (index == 1) {
        gtk_widget_set_valign(outer, GTK_ALIGN_FILL);
        gtk_widget_set_vexpand(outer, TRUE);
    }
    gtk_container_add(GTK_CONTAINER(frame), outer);
    /* Description is on the section heading/info tooltip only. */
    GtkWidget *grid = gtk_grid_new();
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    /* Without vertical expansion, the 190px notebook is centered inside
     * a frame as tall as Overview, leaving a conspicuous empty strip. */
    if (index == 1) {
        gtk_widget_set_valign(grid, GTK_ALIGN_FILL);
        gtk_widget_set_vexpand(grid, TRUE);
    }
    gtk_box_pack_start(GTK_BOX(outer), grid,
                       index == 1, index == 1, 0);
    if (index < 8) g_c.wozt_sections[index] = frame;
    /* Temporarily append the frame. The first configure event moves it
     * into an independent horizontal row, without changing widget identity. */
    gtk_box_pack_start(GTK_BOX(dashboard), frame, FALSE, FALSE, 0);
    *grid_out = grid;
    return frame;
}

enum {
    BACKEND_COL_LABEL = 0,
    BACKEND_COL_SENSITIVE,
    BACKEND_COL_COUNT
};

static GtkWidget *make_backend_combo(GtkShell *shell) {
    GtkListStore *store = gtk_list_store_new(
        BACKEND_COL_COUNT,
        G_TYPE_STRING,
        G_TYPE_BOOLEAN);

    for (int i = 0; i < gamepad_bridge_backend_count(); i++) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(
            store,
            &iter,
            BACKEND_COL_LABEL,
            gamepad_bridge_backend_label(i),
            BACKEND_COL_SENSITIVE,
            gamepad_bridge_backend_available(i),
            -1);
    }

    GtkWidget *combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), renderer, TRUE);
    gtk_cell_layout_add_attribute(
        GTK_CELL_LAYOUT(combo),
        renderer,
        "text",
        BACKEND_COL_LABEL);
    gtk_cell_layout_add_attribute(
        GTK_CELL_LAYOUT(combo),
        renderer,
        "sensitive",
        BACKEND_COL_SENSITIVE);

    gtk_widget_set_tooltip_text(
        combo,
        "Which mechanism sends the merged controller state to the real console.\n\n"
        "Changing backend is saved to scripts/.env and restarts the server cleanly. "
        "Backends shown grey are known to the architecture but are not implemented "
        "in this build yet.");

    g_signal_connect(combo, "changed", G_CALLBACK(on_combo), shell);
    return combo;
}

/* Fills every control from the settings, without those changes being
 * reported back as the user having moved something. */
static void load_controls(GtkShell *shell) {
    AppSettings s;
    SDL_LockMutex(shell->lock);
    s = shell->settings;
    SDL_UnlockMutex(shell->lock);

    shell->loading = 1;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.stream_enabled), s.stream_enabled);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_c.port), s.web_port);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.switch_enabled), s.switch_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.old3ds_enabled), s.old3ds_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.wiiu_pad_enabled), s.wiiu_pad_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.wup_stream_enabled), s.wup_stream_enabled);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.wup_touch_mode),
                             s.wup_touch_mode >= 0 && s.wup_touch_mode <= 2
                                 ? s.wup_touch_mode : 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_c.wiiu_pad_port), s.wiiu_pad_port);
    gtk_range_set_value(GTK_RANGE(g_c.wiiu_pad_bitrate), s.wiiu_pad_bitrate_mbps);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.wiiu_console_enabled),
                                 s.wiiu_console_enabled);
    gtk_range_set_value(GTK_RANGE(g_c.wiiu_console_bitrate), s.wiiu_console_bitrate_mbps);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_c.wiiu_console_port), s.wiiu_console_port);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.wiiu_console_resolution),
                             s.wiiu_console_height == 1080 ? 0
                             : s.wiiu_console_height == 480 ? 2 : 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_c.switch_port), s.switch_port);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_c.old3ds_port), s.old3ds_port);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.resolution),
                             s.browser_height == 1080 ? 0 : (s.browser_height == 720 ? 1 : 2));
    gtk_range_set_value(GTK_RANGE(g_c.bitrate), s.bitrate_mbps);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.capture_format), s.capture_mjpeg ? 1 : 0);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.gamepad_enabled), s.gamepad_enabled);

    if (g_c.output_backend &&
        s.output_backend >= 0 &&
        s.output_backend < gamepad_bridge_backend_count()) {
        gtk_combo_box_set_active(
            GTK_COMBO_BOX(g_c.output_backend),
            s.output_backend);
    }

    pcble_update_controls(shell);

    if (s.output_protocol >= 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.output_protocol), s.output_protocol);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.invert_ry), s.invert_ry);
    gtk_range_set_value(GTK_RANGE(g_c.lt_threshold), s.lt_threshold);
    gtk_range_set_value(GTK_RANGE(g_c.rt_threshold), s.rt_threshold);
    for (int i = 0; i < 2; i++) {
        gtk_range_set_value(GTK_RANGE(g_c.deadzone[i]), s.stick_deadzone[i]);
        gtk_range_set_value(GTK_RANGE(g_c.range[i]), s.stick_range[i]);
        gtk_range_set_value(GTK_RANGE(g_c.diagonal[i]), s.stick_diagonal[i]);

        gtk_range_set_value(
            GTK_RANGE(g_c.output_deadzone[i]),
            s.output_stick_deadzone[i]);

        gtk_range_set_value(
            GTK_RANGE(g_c.output_range[i]),
            s.output_stick_range[i]);

        gtk_range_set_value(
            GTK_RANGE(g_c.output_diagonal[i]),
            s.output_stick_diagonal[i]);
    }

    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(g_c.output_invert_ry),
        s.output_invert_ry);

    gtk_range_set_value(
        GTK_RANGE(g_c.output_lt_threshold),
        s.output_lt_threshold);

    gtk_range_set_value(
        GTK_RANGE(g_c.output_rt_threshold),
        s.output_rt_threshold);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.muted), s.local_muted);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.direct_sink), s.local_direct_sink);
    gtk_range_set_value(GTK_RANGE(g_c.volume), s.local_volume);
    gtk_range_set_value(GTK_RANGE(g_c.brightness), s.brightness);
    gtk_range_set_value(GTK_RANGE(g_c.contrast), s.contrast);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.vsync), s.vsync);
    shell->loading = 0;
}

static gboolean on_settings_delete(GtkWidget *w, GdkEvent *e, gpointer user_data) {
    (void)e;
    (void)user_data;
    /* Hidden, not destroyed: closing the settings is not quitting the
     * program, which is the whole point of living in the tray. */
    gtk_widget_hide(w);
    return TRUE;
}

/* The same icon on the settings window, so the thing in the tray and the
 * thing in the window list are recognisably one program. */
/* Test-only resource override: test_gtk_layout is in tests/.build.
 * The persistent scripts/.env remains binary-relative and is never redirected. */
static void wozt_asset_path(char *out, size_t out_size, const char *relative)
{
    if (!out_size) return;
    const char *root = getenv("C2C_RESOURCE_ROOT");
    if (!root || !*root) {
        app_path(out, out_size, relative);
        return;
    }
    const size_t a = strlen(root), b = strlen(relative);
    if (a + 1 + b + 1 > out_size) {
        out[0] = '\0';
        return;
    }
    memcpy(out, root, a);
    out[a] = '/';
    memcpy(out + a + 1, relative, b);
    out[a + b + 1] = '\0';
}

static void set_window_icon(GtkWidget *win) {
    char path[512];
    wozt_asset_path(path, sizeof(path), "assets/icon-64.png");
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
        gtk_window_set_icon_from_file(GTK_WINDOW(win), path, NULL);
    else
        gtk_window_set_icon_name(GTK_WINDOW(win), "camera-video");
}

/* WOZT_0868_HALF_RESET_SELECTORS:
 * add_row() deliberately requests a 200px combo for the general dashboard.
 * These TWO Reset-method combos need half that width. Restrict the cell's
 * natural requisition as well: size_request alone is only a minimum in GTK3.
 * The selected option stays fully readable through a live tooltip. */
static void wozt_reset_selector_tooltip_changed(GtkComboBox *widget,
                                                 gpointer unused)
{
    (void)unused;
    gchar *selected = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
    gtk_widget_set_tooltip_text(GTK_WIDGET(widget), selected ? selected : "");
    g_free(selected);
}

static void wozt_half_width_reset_selector(GtkWidget *widget)
{
    if (!GTK_IS_COMBO_BOX_TEXT(widget)) return;
    enum { NORMAL_RESET_SELECTOR_WIDTH = 200,
           COMPACT_RESET_SELECTOR_WIDTH = NORMAL_RESET_SELECTOR_WIDTH / 2,
           COMPACT_RESET_CELL_WIDTH = COMPACT_RESET_SELECTOR_WIDTH - 24 };
    gtk_widget_set_size_request(widget, COMPACT_RESET_SELECTOR_WIDTH, -1);
    gtk_widget_set_hexpand(widget, FALSE);
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(widget));
    for (GList *it = cells; it; it = it->next) {
        if (!GTK_IS_CELL_RENDERER_TEXT(it->data)) continue;
        gtk_cell_renderer_set_fixed_size(GTK_CELL_RENDERER(it->data),
                                         COMPACT_RESET_CELL_WIDTH, -1);
        g_object_set(it->data, "ellipsize", PANGO_ELLIPSIZE_END,
                     "single-paragraph-mode", TRUE, NULL);
    }
    g_list_free(cells);
    g_signal_connect(widget, "changed",
                     G_CALLBACK(wozt_reset_selector_tooltip_changed), NULL);
    wozt_reset_selector_tooltip_changed(GTK_COMBO_BOX(widget), NULL);
}

/* Move a *single instance* of every live reset control, never recreating
 * its signals/settings. Validate the old grid completely before detaching. */
static void wozt_compact_reset_method(GtkWidget *root)
{
    if (!GTK_IS_GRID(root) || !GTK_IS_GRID(g_c.reset_bt_panel)) return;
    GtkWidget *panel = g_c.reset_bt_panel;
    GtkWidget *wake_heading = gtk_grid_get_child_at(GTK_GRID(root), 0, 0);
    GtkWidget *old_method_label = gtk_grid_get_child_at(GTK_GRID(root), 0, 1);
    if (!GTK_IS_LABEL(wake_heading) || !GTK_IS_LABEL(old_method_label) ||
        g_strcmp0(gtk_label_get_text(GTK_LABEL(wake_heading)), "Wake method") ||
        g_strcmp0(gtk_label_get_text(GTK_LABEL(old_method_label)), "method") ||
        gtk_widget_get_parent(g_c.reset_method) != root) {
        g_warning("wozt-ui: Reset method title/selector structure changed");
        return;
    }

    GtkWidget *bt_heading = gtk_grid_get_child_at(GTK_GRID(panel), 0, 0);
    GtkWidget *target_l = NULL, *target_c = NULL;
    GtkWidget *adapter_l = NULL, *adapter_c = NULL;
    GtkWidget *tools_l = NULL, *tools_c = NULL;
    GtkWidget *beacon_l = NULL, *beacon_c = NULL;
    int target_row = -1, adapter_row = -1, tools_row = -1, beacon_row = -1;
    if (!GTK_IS_LABEL(bt_heading) ||
        !wozt_find_grid_row(panel, "target console", &target_l, &target_c, &target_row) ||
        !wozt_find_grid_row(panel, "Bluetooth adapter", &adapter_l, &adapter_c, &adapter_row) ||
        !wozt_find_grid_row(panel, "adapter tools", &tools_l, &tools_c, &tools_row) ||
        !wozt_find_grid_row(panel, "beacon", &beacon_l, &beacon_c, &beacon_row) ||
        !GTK_IS_LABEL(g_c.reset_bt_status) ||
        gtk_widget_get_parent(g_c.reset_bt_status) != panel) {
        g_warning("wozt-ui: Bluetooth reset grid changed, keeping existing layout");
        return;
    }
    GtkWidget *parts[] = {bt_heading, target_l, target_c, adapter_l, adapter_c,
                          tools_l, tools_c, beacon_l, beacon_c, g_c.reset_bt_status};
    GList *existing = gtk_container_get_children(GTK_CONTAINER(panel));
    const gboolean complete = g_list_length(existing) == G_N_ELEMENTS(parts);
    g_list_free(existing);
    if (!complete) {
        g_warning("wozt-ui: unexpected Bluetooth reset children, not reparenting");
        return;
    }

    /* Heading and selector form ONE row. Destroy only the redundant
     * 'method' GtkLabel; the original combo and its callbacks survive. */
    GtkWidget *combo = g_c.reset_method;
    g_object_ref(wake_heading);
    g_object_ref(combo);
    gtk_container_remove(GTK_CONTAINER(root), wake_heading);
    gtk_container_remove(GTK_CONTAINER(root), combo);
    gtk_widget_destroy(old_method_label);
    GtkWidget *method_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(method_bar, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(method_bar), wake_heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(method_bar), combo, FALSE, FALSE, 0);
    wozt_half_width_reset_selector(combo);
    g_object_unref(wake_heading);
    g_object_unref(combo);
    gtk_grid_attach(GTK_GRID(root), method_bar, 0, 0, 2, 1);
    gtk_widget_show_all(method_bar);
    gtk_grid_set_row_spacing(GTK_GRID(root), 1);
    gtk_grid_set_column_spacing(GTK_GRID(root), 4);

    for (guint i = 0; i < G_N_ELEMENTS(parts); ++i) {
        g_object_ref(parts[i]);
        gtk_container_remove(GTK_CONTAINER(panel), parts[i]);
    }
    GtkWidget *left = gtk_grid_new();
    GtkWidget *right = gtk_grid_new();
    for (int side = 0; side < 2; ++side) {
        GtkWidget *g = side ? right : left;
        gtk_grid_set_row_spacing(GTK_GRID(g), 1);
        gtk_grid_set_column_spacing(GTK_GRID(g), 4);
        gtk_widget_set_halign(g, GTK_ALIGN_START);
        gtk_widget_set_valign(g, GTK_ALIGN_START);
    }
    /* LEFT: Bluetooth wake beacon heading, then target console.
     * RIGHT: starts with Bluetooth adapter, then tools, beacon, status. */
    gtk_grid_attach(GTK_GRID(left), bt_heading, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(left), target_l, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(left), target_c, 1, 1, 1, 1);
    wozt_half_width_reset_selector(target_c);
    gtk_grid_attach(GTK_GRID(right), adapter_l, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(right), adapter_c, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(right), tools_l, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(right), tools_c, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(right), beacon_l, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(right), beacon_c, 1, 2, 1, 1);
    gtk_widget_set_halign(g_c.reset_bt_status, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(right), g_c.reset_bt_status, 0, 3, 2, 1);
    for (guint i = 0; i < G_N_ELEMENTS(parts); ++i)
        g_object_unref(parts[i]);

    /* GtkFlowBox gives two side-by-side groups when they fit, and one
     * below the other at narrow widths, with no hard-coded breakpoint. */
    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 2);
    gtk_widget_set_halign(flow, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(flow, TRUE);
    gtk_container_add(GTK_CONTAINER(flow), left);
    gtk_container_add(GTK_CONTAINER(flow), right);
    gtk_grid_set_row_spacing(GTK_GRID(panel), 0);
    gtk_grid_set_column_spacing(GTK_GRID(panel), 0);
    gtk_grid_attach(GTK_GRID(panel), flow, 0, 0, 2, 1);
    gtk_widget_show_all(flow);
    g_object_set_data(G_OBJECT(panel), "wozt-reset-two-columns", GINT_TO_POINTER(1));
}

static void wozt_compact_maintenance(GtkWidget *grid)
{
    if (!GTK_IS_GRID(grid)) return;
    gtk_grid_set_row_spacing(GTK_GRID(grid), 1);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_halign(grid, GTK_ALIGN_START);
    gtk_widget_set_hexpand(grid, FALSE);
    GList *children = gtk_container_get_children(GTK_CONTAINER(grid));
    for (GList *it = children; it; it = it->next) {
        GtkWidget *w = GTK_WIDGET(it->data);
        if (GTK_IS_LABEL(w)) {
            gtk_label_set_xalign(GTK_LABEL(w), 0.0f);
            if (gtk_style_context_has_class(gtk_widget_get_style_context(w),
                                            "wozt-subsection")) {
                gtk_widget_set_margin_top(w, 2);
                gtk_widget_set_margin_bottom(w, 0);
            }
        } else if (GTK_IS_BUTTON(w)) {
            wozt_compact_button_widget(w);
        }
    }
    g_list_free(children);
}

static void build_settings_window(GtkShell *shell) {
    g_c.replug_pending = -1;

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    {
        char title[96];
        snprintf(
            title,
            sizeof(title),
            "Capture2Cloud Server — v%s",
            C2C_VERSION);
        gtk_window_set_title(GTK_WINDOW(win), title);
    }

    /*
     * This is a server control panel now, not a tiny preferences dialog.
     * 1000x700 still fits comfortably on a 1280x720 display while
     * leaving enough room for meaningful labels and status lines.
     */
    gtk_window_set_default_size(GTK_WINDOW(win), 1144, 840);
    GdkGeometry geometry = {0};
    /* Actual minimum, not the natural width of the nested controller grids.
     * Main scroll view handles any temporary overflow while resizing. */
    geometry.min_width = 600;
    geometry.min_height = 420;
    gtk_window_set_geometry_hints(GTK_WINDOW(win), win, &geometry, GDK_HINT_MIN_SIZE);
    /* GTK3 exposes a setter, not a public geometry-hints getter. Keep the
     * exact width passed above available to the in-thread layout probe. */
    g_object_set_data(G_OBJECT(win), "wozt-minimum-width",
                      GINT_TO_POINTER(geometry.min_width));
    g_signal_connect(
        win,
        "delete-event",
        G_CALLBACK(on_settings_delete),
        shell);

    /* GtkWindow accepts one direct child: the GtkOverlay below.
     * Do not add a second GtkBox here; the previous migration
     * duplicated 'root' and would also violate GtkWindow parenting. */

    /* WOZT v1.2: borderless GTK window with a stable X11 taskbar identity. */
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
    gtk_window_set_wmclass(GTK_WINDOW(win), "capture2cloud", "capture2cloud");
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "wozt-window");
    g_signal_connect(win, "configure-event", G_CALLBACK(wozt_window_configured), shell);

    char ui_path[512], theme_id[48], language_id[48];
    wozt_asset_path(ui_path, sizeof(ui_path), "data");
    shell->wozt = wozt_ui_standard_new(
        ui_path,
        config_get_str("WOZT_UI_THEME", theme_id, sizeof(theme_id), "graphite"),
        config_get_str("WOZT_UI_LANGUAGE", language_id, sizeof(language_id), "en"));

    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(win), overlay);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(overlay), root);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_style_context_add_class(gtk_widget_get_style_context(header), "wozt-titlebar");
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 6);
    gtk_widget_set_margin_top(header, 2);
    gtk_widget_set_margin_bottom(header, 2);

    /* Dragging only the noninteractive title area moves the window. */
    GtkWidget *drag = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(drag), FALSE);
    gtk_widget_add_events(drag, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(drag, "button-press-event", G_CALLBACK(wozt_title_drag), shell);
    GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    char icon_path[512];
    wozt_asset_path(icon_path, sizeof(icon_path), "assets/icon-64.png");
    /* gtk_image_set_pixel_size() does NOT scale an image loaded from PNG.
     * Decode directly to a real 20x20 pixbuf instead of showing 64x64. */
    GError *icon_error = NULL;
    GdkPixbuf *small_icon = gdk_pixbuf_new_from_file_at_scale(
        icon_path, 20, 20, TRUE, &icon_error);
    GtkWidget *brand_icon = small_icon
        ? gtk_image_new_from_pixbuf(small_icon)
        : gtk_image_new_from_icon_name("applications-multimedia-symbolic", GTK_ICON_SIZE_MENU);
    if (small_icon) g_object_unref(small_icon);
    if (icon_error) g_clear_error(&icon_error);
    gtk_widget_set_size_request(brand_icon, 20, 20);
    g_object_set_data(G_OBJECT(win), "wozt-brand-icon", brand_icon);
    gtk_box_pack_start(GTK_BOX(brand), brand_icon, FALSE, FALSE, 0);
    GtkWidget *brand_title = gtk_label_new("Capture2Cloud server");
    gtk_style_context_add_class(gtk_widget_get_style_context(brand_title), "wozt-title");
    gtk_box_pack_start(GTK_BOX(brand), brand_title, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(drag), brand);
    gtk_box_pack_start(GTK_BOX(header), drag, TRUE, TRUE, 0);

    g_c.wozt_theme_combo = wozt_ui_standard_theme_combo(shell->wozt);
    gtk_widget_set_tooltip_text(g_c.wozt_theme_combo, "Theme");
    gtk_widget_set_size_request(g_c.wozt_theme_combo, 98, -1);
    gtk_box_pack_start(GTK_BOX(header), g_c.wozt_theme_combo, FALSE, FALSE, 0);
    g_signal_connect(g_c.wozt_theme_combo, "changed", G_CALLBACK(wozt_theme_changed), shell);

    g_c.wozt_language_combo = wozt_ui_standard_language_combo(shell->wozt);
    gtk_widget_set_tooltip_text(g_c.wozt_language_combo, "Language");
    gtk_widget_set_size_request(g_c.wozt_language_combo, 88, -1);
    gtk_box_pack_start(GTK_BOX(header), g_c.wozt_language_combo, FALSE, FALSE, 0);
    g_signal_connect(g_c.wozt_language_combo, "changed", G_CALLBACK(wozt_language_changed), shell);

    GtkWidget *reload = wozt_window_control("↻", "Reload themes and languages",
                                            G_CALLBACK(wozt_reload_clicked), shell);
    gtk_box_pack_start(GTK_BOX(header), reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header),
                       wozt_window_control("−", "Minimize",
                                           G_CALLBACK(wozt_window_minimize), shell),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header),
                       wozt_window_control("□", "Maximize / restore",
                                           G_CALLBACK(wozt_window_maximize), shell),
                       FALSE, FALSE, 0);
    GtkWidget *close_button = wozt_window_control("×", "Hide to tray",
                                                   G_CALLBACK(wozt_window_hide), shell);
    gtk_style_context_add_class(gtk_widget_get_style_context(close_button), "wozt-danger");
    gtk_box_pack_start(GTK_BOX(header), close_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

    /* Responsive independently-packed horizontal lines, not a GtkGrid.
     * AUTOMATIC horizontal overflow matters: GTK must never prevent the
     * user shrinking the window because a translated button is too wide.
     * Narrow layouts remain usable through scrolling while lines reflow. */
    GtkWidget *body = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(body),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(body), 0);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(body), FALSE);
    gtk_widget_set_size_request(body, 1, 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(body), "wozt-dashboard");
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);
    GtkWidget *dashboard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_hexpand(dashboard, TRUE);
    gtk_widget_set_margin_start(dashboard, 6);
    gtk_widget_set_margin_end(dashboard, 6);
    gtk_widget_set_margin_top(dashboard, 5);
    gtk_widget_set_margin_bottom(dashboard, 5);
    gtk_container_add(GTK_CONTAINER(body), dashboard);
    g_c.wozt_dashboard = dashboard;
    g_c.wozt_section_count = 0;
    g_c.wozt_layout_mode = -1;
    wozt_add_resize_zones(overlay, win);
    /* Existing make_stack_page calls now append visible functional frames. */
    GtkWidget *stack = dashboard;

    GtkWidget *grid;
    int row;

    /* ===============================================================
     * OVERVIEW
     * =============================================================== */
    make_stack_page(
        stack,
        "overview",
        "Overview",
        "The things worth seeing first: server state, console output and "
        "who is connected.",
        &grid);

    row = 0;

    /* WOZT_0855_OVERVIEW_COMPACT: only the SVG icons occupy column 0.
     * The two Server status lines and the action row span both columns,
     * preventing old text labels from widening the icon/status gap. */
    gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
    add_section_header(grid, row++, "Server");

    /* One compact row: server - capture - browser - native. The hyphens
     * are plain separators, never icons or multi-line text. Hover each
     * actual indicator for its live state and full explanation. */
    GtkWidget *indicators = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_halign(indicators, GTK_ALIGN_START);
    g_c.overview_indicator_bar = indicators;
    static const char *const icon_names[4] = {
        "emblem-ok-symbolic", "camera-video-symbolic",
        "web-browser-symbolic", "network-server-symbolic"
    };
    for (int k = 0; k < 4; ++k) {
        GtkWidget *icon = gtk_image_new_from_icon_name(
            icon_names[k], GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 11);
        g_c.overview_indicators[k] = icon;
        gtk_box_pack_start(GTK_BOX(indicators), icon, FALSE, FALSE, 0);
        if (k < 3) {
            GtkWidget *dash = gtk_label_new("-");
            gtk_box_pack_start(GTK_BOX(indicators), dash, FALSE, FALSE, 0);
        }
    }
    gtk_grid_attach(GTK_GRID(grid), indicators, 0, row++, 2, 1);

    /* Hidden full-text labels preserve the existing live status API and
     * test probes without reserving any width or height in Overview. */
    g_c.overview_status = gtk_label_new("starting...");
    gtk_label_set_selectable(GTK_LABEL(g_c.overview_status), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(g_c.overview_status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(g_c.overview_status), 34);
    gtk_widget_set_no_show_all(g_c.overview_status, TRUE);
    gtk_grid_attach(GTK_GRID(grid), g_c.overview_status, 0, row++, 2, 1);
    g_c.backend_status = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(g_c.backend_status), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(g_c.backend_status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(g_c.backend_status), 34);
    gtk_widget_set_no_show_all(g_c.backend_status, TRUE);
    gtk_grid_attach(GTK_GRID(grid), g_c.backend_status, 0, row++, 2, 1);

    add_section_header(grid, row++, "Connected clients");
    GtkWidget *overview_clients = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(overview_clients, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), overview_clients, 0, row++, 2, 1);
    g_c.overview_client_status[GTK_SHELL_CLIENT_BROWSER] =
        add_client_icon_status(overview_clients, "Browser");
    g_c.overview_client_status[GTK_SHELL_CLIENT_NATIVE] =
        add_client_icon_status(overview_clients, "Switch");
    g_c.overview_client_status[GTK_SHELL_CLIENT_ANDROID] =
        add_client_icon_status(overview_clients, "Android");
    g_c.overview_client_status[GTK_SHELL_CLIENT_WIIU_PAD] =
        add_client_icon_status(overview_clients, "Wii U GamePad");
    g_c.overview_client_status[GTK_SHELL_CLIENT_WUPSTREAM] =
        add_client_icon_status(overview_clients, "WUP Stream GamePad");
    g_c.overview_client_status[GTK_SHELL_CLIENT_WIIU_CONSOLE] =
        add_client_icon_status(overview_clients, "Wii U Console");
    g_c.overview_client_status[GTK_SHELL_CLIENT_OLD3DS] =
        add_client_icon_status(overview_clients, "Old 3DS / Old 2DS");

    /* All seven concise client labels are already single-line in their
     * own horizontal icon/status rows; only the Client tabs wrap. */

    add_section_header(grid, row++, "Quick actions");

    {
        GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign(actions, GTK_ALIGN_START);
        gtk_widget_set_hexpand(actions, FALSE);
        GtkWidget *show_capture = make_button(
            shell, "show capture window", GTK_SHELL_ACTION_SHOW_CAPTURE,
            "Shows the local capture monitor.");
        GtkWidget *wake = make_button(
            shell, "wake console", GTK_SHELL_ACTION_WAKE_CONSOLE,
            "Runs the configured wake action, then recovers controller output.");
        gtk_style_context_add_class(gtk_widget_get_style_context(show_capture),
                                    "wozt-overview-action-fulltext");
        gtk_style_context_add_class(gtk_widget_get_style_context(wake),
                                    "wozt-overview-action-fulltext");
        gtk_box_pack_start(GTK_BOX(actions), show_capture, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(actions), wake, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), actions, 0, row++, 2, 1);
    }

    /* ===============================================================
     * CLIENTS
     * =============================================================== */
    make_stack_page(
        stack,
        "clients",
        "Clients",
        "Everything on this page changes what a remote client receives. "
        "The sub-tabs are deliberately named Client · ... so these controls "
        "cannot be confused with server hardware settings.",
        &grid);

    GtkWidget *clients = gtk_notebook_new();
    g_c.wozt_clients_notebook = clients;
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(clients), TRUE);
    gtk_widget_set_hexpand(clients, TRUE);
    gtk_widget_set_vexpand(clients, TRUE);
    gtk_grid_attach(GTK_GRID(grid), clients, 0, 0, 2, 1);

    /* --- Client · Browser ------------------------------------------ */
    make_page("Client · Browser", clients, &grid);
    row = 0;

    g_c.stream_enabled = add_row(
        grid,
        row++,
        "serve to browsers",
        make_check(shell, "on"));

    g_c.port = add_row(
        grid,
        row++,
        "port",
        gtk_spin_button_new_with_range(1, 65535, 1));
    g_signal_connect(
        g_c.port,
        "value-changed",
        G_CALLBACK(on_spin),
        shell);

    g_c.resolution = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.resolution), "1080p60");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.resolution), "720p60");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.resolution), "480p60");
    gtk_widget_set_tooltip_text(
        g_c.resolution,
        "What the browser encoder produces. This affects browsers only.");
    g_signal_connect(
        g_c.resolution,
        "changed",
        G_CALLBACK(on_combo),
        shell);
    add_row(grid, row++, "resolution", g_c.resolution);

    g_c.bitrate = add_row(
        grid,
        row++,
        "bitrate (Mbps)",
        make_scale(shell, 2, 50, 1, ""));

    g_c.client_status[GTK_SHELL_CLIENT_BROWSER] =
        add_client_status(grid, row++, "watching now");


    /* --- Client · Switch ----------------------------------------- */
    make_page("Client · Switch", clients, &grid);
    row = 0;

    g_c.switch_enabled = add_row(
        grid,
        row++,
        "native client server",
        make_check(shell, "on"));

    gtk_widget_set_tooltip_text(
        g_c.switch_enabled,
        "The Nintendo Switch homebrew and Android application use this "
        "native protocol server.");

    g_c.switch_port = add_row(
        grid,
        row++,
        "port",
        gtk_spin_button_new_with_range(1, 65535, 1));

    g_signal_connect(
        g_c.switch_port,
        "value-changed",
        G_CALLBACK(on_spin),
        shell);

    g_c.client_status[GTK_SHELL_CLIENT_NATIVE] =
        add_client_status(grid, row++, "connected now");

    {
        GtkWidget *note = gtk_label_new(
            "Native TCP settings are shared by Switch and Android. "
            "Connected devices are counted separately by their handshake.");

        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(note), 70);
        gtk_widget_set_halign(note, GTK_ALIGN_START);
        add_row(grid, row++, "", note);
    }


    /* WOZT_0856_SPLIT_CLIENTS: direct radio and WUP occupy separate tabs. */
    /* --- Client · Android ----------------------------------------- */
    make_page("Client · Android", clients, &grid);
    row = 0;
    add_section_header(grid, row++, "Android native client");
    g_c.client_status[GTK_SHELL_CLIENT_ANDROID] =
        add_client_status(grid, row++, "connected now");
    GtkWidget *android_note = gtk_label_new(
        "Android uses the same native TCP listener as Switch. "
        "Port and enable settings are on the Switch tab; device identity "
        "is measured from the handshake, not guessed from the codec.");
    gtk_label_set_line_wrap(GTK_LABEL(android_note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(android_note), 42);
    gtk_widget_set_halign(android_note, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), android_note, 0, row++, 2, 1);

    /* --- Client · GamePad direct radio ----------------------------- */
    make_page("Client · Wii U GamePad", clients, &grid);
    row = 0;
    add_section_header(grid, row++, "Direct radio bridge");

    g_c.wiiu_pad_enabled = add_row(
        grid,
        row++,
        "serve to GamePad",
        make_check(shell, "on"));

    gtk_widget_set_tooltip_text(
        g_c.wiiu_pad_enabled,
        "Feeds a real Wii U GamePad over its dedicated radio path.");

    g_c.wiiu_pad_port = add_row(
        grid,
        row++,
        "TCP port",
        gtk_spin_button_new_with_range(1, 65535, 1));

    g_signal_connect(
        g_c.wiiu_pad_port,
        "value-changed",
        G_CALLBACK(on_spin),
        shell);

    g_c.wiiu_pad_bitrate = add_row(
        grid,
        row++,
        "bitrate (Mbps)",
        make_scale(shell, 2, 20, 1, ""));

    g_c.wiiu_status = gtk_label_new("");
    gtk_widget_set_halign(g_c.wiiu_status, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(g_c.wiiu_status), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(g_c.wiiu_status), TRUE);
    add_row(grid, row++, "bridge status", g_c.wiiu_status);

    g_c.client_status[GTK_SHELL_CLIENT_WIIU_PAD] =
        add_client_status(grid, row++, "stream");

    {
        GtkWidget *pad_buttons = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(pad_buttons), 1);
        gtk_grid_set_column_spacing(GTK_GRID(pad_buttons), 3);

        gtk_grid_attach(GTK_GRID(pad_buttons),
            make_button(shell, "start stream", GTK_SHELL_ACTION_WIIU_START,
                        "Starts the Wii U GamePad bridge."), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(pad_buttons),
            make_button(shell, "stop stream", GTK_SHELL_ACTION_WIIU_STOP,
                        "Stops the Wii U GamePad bridge."), 1, 0, 1, 1);

        add_row(grid, row++, "stream control", pad_buttons);

        GtkWidget *ap_buttons = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(ap_buttons), 1);
        gtk_grid_set_column_spacing(GTK_GRID(ap_buttons), 3);

        gtk_grid_attach(GTK_GRID(ap_buttons),
            make_button(shell, "start AP", GTK_SHELL_ACTION_WIIU_AP_START,
                        "Starts the Wii U GamePad access point."), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(ap_buttons),
            make_button(shell, "stop AP", GTK_SHELL_ACTION_WIIU_AP_STOP,
                        "Stops the Wii U GamePad access point."), 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(ap_buttons),
            make_button(shell, "deauth pad", GTK_SHELL_ACTION_WIIU_DEAUTH,
                        "Forces the GamePad to associate again."), 0, 1, 1, 1);

        GtkWidget *pair = gtk_button_new_with_label("pair GamePad...");
        g_signal_connect(
            pair,
            "clicked",
            G_CALLBACK(on_pair_clicked),
            shell);

        gtk_grid_attach(GTK_GRID(ap_buttons), pair, 1, 1, 1, 1);

        add_row(grid, row++, "radio", ap_buttons);
    }


    /* --- Client · GamePad WUP Wi-Fi ------------------------------- */
    make_page("Client · WUP Stream GamePad", clients, &grid);
    row = 0;
    add_section_header(grid, row++, "WUP Stream (Wi-Fi homebrew)");
    g_c.wup_stream_enabled = add_row(
        grid, row++, "WUP Stream via Wi-Fi", make_check(shell, "on"));
    gtk_widget_set_tooltip_text(g_c.wup_stream_enabled,
        "Uses WUP Stream running on the GamePad; no Nintendo Wi-Fi adapter. "
        "Exclusive with the direct radio bridge. Build: scripts/build_wupstream.sh");
    g_c.wup_touch_mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.wup_touch_mode),
        "Disabled (no WUP mouse input)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.wup_touch_mode),
        "Relative touchpad (tap / drag / long press)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.wup_touch_mode),
        "Absolute (virtual Joy-Con 2 cursor position)");
    g_signal_connect(g_c.wup_touch_mode, "changed", G_CALLBACK(on_combo), shell);
    add_row(grid, row++, "touch -> Joy-Con 2", g_c.wup_touch_mode);
    GtkWidget *wup_note = gtk_label_new(
        "WUP Stream carries HDMI video/audio and GamePad controls. "
        "Touch is available only with a Joy-Con 2 PCBLE profile. "
        "Switch touch mode live (Off / Relative / Absolute) without reconnecting. "
        "GamePad sticks always remain controller axes, not mouse motion. "
        "Absolute mode estimates position from relative Joy-Con 2 movements.");
    gtk_label_set_line_wrap(GTK_LABEL(wup_note), TRUE);
    gtk_widget_set_halign(wup_note, GTK_ALIGN_START);
    add_row(grid, row++, "", wup_note);
    gtk_label_set_max_width_chars(GTK_LABEL(wup_note), 40);

    g_c.client_status[GTK_SHELL_CLIENT_WUPSTREAM] =
        add_client_status(grid, row++, "stream");
    g_c.wup_metrics_label = gtk_label_new("FPS -- | New -- | Capt --\nRepeat -- | Drop -- | Err --\nScale -- | Frame -- ms");
    gtk_widget_set_halign(g_c.wup_metrics_label, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(g_c.wup_metrics_label), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(g_c.wup_metrics_label), TRUE);
    gtk_widget_set_tooltip_text(g_c.wup_metrics_label,
        "Measured helper throughput: Tx frames, new frames, producer frames, "
        "repeated/overwritten frames, failures and frame-call duration "
        "(including pacing). Not network bitrate or end-to-end latency.");
    gtk_grid_attach(GTK_GRID(grid), g_c.wup_metrics_label, 0, row++, 2, 1);

    /* --- Client · Wii U Console ----------------------------------- */
    make_page("Client · Wii U Console", clients, &grid);
    row = 0;

    g_c.wiiu_console_enabled = add_row(
        grid,
        row++,
        "serve to Wii U console",
        make_check(shell, "on"));

    g_c.wiiu_console_port = add_row(
        grid,
        row++,
        "TCP port",
        gtk_spin_button_new_with_range(1, 65535, 1));

    g_signal_connect(
        g_c.wiiu_console_port,
        "value-changed",
        G_CALLBACK(on_spin),
        shell);

    g_c.wiiu_console_resolution = gtk_combo_box_text_new();

    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.wiiu_console_resolution),
        "1080p60 (not supported)");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.wiiu_console_resolution),
        "720p60");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.wiiu_console_resolution),
        "480p60");

    g_signal_connect(
        g_c.wiiu_console_resolution,
        "changed",
        G_CALLBACK(on_combo),
        shell);

    add_row(
        grid,
        row++,
        "resolution",
        g_c.wiiu_console_resolution);

    g_c.wiiu_console_bitrate = add_row(
        grid,
        row++,
        "bitrate (Mbps)",
        make_scale(shell, 2, 30, 1, ""));

    g_c.client_status[GTK_SHELL_CLIENT_WIIU_CONSOLE] =
        add_client_status(grid, row++, "connected now");

    {
        GtkWidget *buttons =
            gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        gtk_box_pack_start(
            GTK_BOX(buttons),
            make_button(
                shell,
                "force keyframe",
                GTK_SHELL_ACTION_WIIU_CONSOLE_KEYFRAME,
                "Requests an H.264 recovery point now."),
            FALSE, FALSE, 0);

        add_row(grid, row++, "", buttons);
    }


    /* --- Client · Old 3DS / Old 2DS ------------------------------- */
    make_page("Client · Old 3DS / Old 2DS", clients, &grid);
    row = 0;

    g_c.old3ds_enabled = add_row(
        grid, row++, "serve to Old 3DS / 2DS", make_check(shell, "on"));
    gtk_widget_set_tooltip_text(g_c.old3ds_enabled,
        "Open or close the dedicated Old 3DS/2DS TCP listener. "
        "Does not disable Switch, Android, or the Wii U clients.");

    g_c.old3ds_port = add_row(
        grid,
        row++,
        "TCP port",
        gtk_spin_button_new_with_range(1, 65535, 1));

    g_signal_connect(
        g_c.old3ds_port,
        "value-changed",
        G_CALLBACK(on_spin),
        shell);

    {
        GtkWidget *profile = gtk_label_new(
            "400×240 @ 30 FPS · JPEG / MPEG-1 / MPEG-2 / MPEG-4 · dedicated encoder");
        gtk_widget_set_halign(profile, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(profile), TRUE);
        add_row(grid, row++, "profile", profile);
    }

    g_c.client_status[GTK_SHELL_CLIENT_OLD3DS] =
        add_client_status(grid, row++, "connected now");

    {
        GtkWidget *note = gtk_label_new(
            "The web/login port is the Browser port. Configure that port, "
            "this TCP port and the same host address in the 3DS client.");
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(note), 70);
        gtk_widget_set_halign(note, GTK_ALIGN_START);
        add_row(grid, row++, "", note);
    }


    /* All seven notebook pages inherit the same compact control density.
     * Keep the WUP touch options usable without a very wide combo. */
    for (int page_i = 0; page_i < 7; ++page_i)
        wozt_compact_client_widgets(gtk_notebook_get_nth_page(
            GTK_NOTEBOOK(clients), page_i));
    if (g_c.wup_touch_mode) {
        gtk_widget_set_size_request(g_c.wup_touch_mode, 195, -1);
        gtk_widget_set_tooltip_text(g_c.wup_touch_mode,
                                    "WUP touch: Off / Relative / Absolute");
    }

    /* ===============================================================
     * CONTROLLER OUTPUT
     * =============================================================== */
    make_stack_page(
        stack,
        "controller-output",
        "Controller output",
        "Inputs from browsers, native clients and the local controller are "
        "merged first. This page chooses how that final state reaches the "
        "real console.",
        &grid);

    row = 0;

    add_section_header(grid, row++, "Console output backend");

    g_c.output_backend = make_backend_combo(shell);
    add_row(grid, row++, "backend", g_c.output_backend);

    {
        GtkWidget *note = gtk_label_new(
            "Changing backend is persistent and restarts the server cleanly. "
            "Grey entries are part of the planned architecture but have no "
            "implementation in this build yet.");
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(note), 72);
        gtk_widget_set_halign(note, GTK_ALIGN_START);
        add_row(grid, row++, "", note);
    }

    /* backend_status is created on Overview; it is intentionally not
     * packed twice. The overview is the dashboard, this page contains
     * the controls. */

    g_c.titan_settings = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g_c.titan_settings), 2);
    gtk_grid_set_column_spacing(GTK_GRID(g_c.titan_settings), 14);
    gtk_widget_set_no_show_all(g_c.titan_settings, TRUE);
    gtk_grid_attach(
        GTK_GRID(grid),
        g_c.titan_settings,
        0,
        row++,
        2,
        1);

    {
        int tr = 0;

        add_section_header(
            g_c.titan_settings,
            tr++,
            "Titan / ConsoleTuner backend");

        g_c.output_protocol = gtk_combo_box_text_new();

        for (int i = 0; i < gamepad_protocol_count(); i++) {
            gtk_combo_box_text_append_text(
                GTK_COMBO_BOX_TEXT(g_c.output_protocol),
                gamepad_protocol_label(i));
        }

        g_signal_connect(
            g_c.output_protocol,
            "changed",
            G_CALLBACK(on_combo),
            shell);

        add_row(
            g_c.titan_settings,
            tr++,
            "adapter emulates",
            g_c.output_protocol);

        g_c.adapter_sees = gtk_label_new("");
        gtk_widget_set_halign(g_c.adapter_sees, GTK_ALIGN_START);
        add_row(
            g_c.titan_settings,
            tr++,
            "adapter sees",
            g_c.adapter_sees);

        gtk_widget_set_tooltip_text(
            g_c.output_protocol,
            "Titan/ConsoleTuner only. Which controller protocol the adapter "
            "presents to the console.");
    }


    /* --- Nintendo Bluetooth backend ------------------------------ */

    g_c.pcble_settings = gtk_grid_new();
    gtk_grid_set_row_spacing(
        GTK_GRID(g_c.pcble_settings),
        2);
    gtk_grid_set_column_spacing(
        GTK_GRID(g_c.pcble_settings),
        6);
    gtk_widget_set_no_show_all(
        g_c.pcble_settings,
        TRUE);

    gtk_grid_attach(
        GTK_GRID(grid),
        g_c.pcble_settings,
        0,
        row++,
        2,
        1);

    {
        int pr = 0;

        add_section_header(
            g_c.pcble_settings,
            pr++,
            "pcble2gamepad");

        g_c.pcble_controller =
            gtk_combo_box_text_new();

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_controller),
            "Nintendo Switch Pro Controller");

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_controller),
            "Nintendo Joy-Con Pair");

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_controller),
            "Joy-Con 1 (L) Classic + Joy-Con 2 (R) BLE");

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_controller),
            "Joy-Con 2 (L) BLE");
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_controller),
            "Joy-Con 2 (R) BLE");
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_controller),
            "Joy-Con 2 pair (L + R) BLE");
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_controller),
            "Joy-Con 2 (L) BLE + Joy-Con 1 (R) Classic");

        gtk_widget_set_tooltip_text(
            g_c.pcble_controller,
            "Joy-Con 1 (L) + Joy-Con 2 (R): the right adapter must support "
            "Bluetooth LE with LE 2M and is taken over exclusively during "
            "the session. Pair / Sync validates the pair with L+R "
            "automatically. Click the stream window to capture the mouse "
            "for the Joy-Con 2 mouse mode; Right Ctrl releases it.");

        g_signal_connect(
            g_c.pcble_controller,
            "changed",
            G_CALLBACK(pcble_controller_changed),
            shell);

        add_row(
            g_c.pcble_settings,
            pr++,
            "emulated controller",
            g_c.pcble_controller);

        g_c.pcble_primary =
            gtk_combo_box_text_new();

        g_signal_connect(
            g_c.pcble_primary,
            "changed",
            G_CALLBACK(pcble_adapter_changed),
            shell);

        add_row(
            g_c.pcble_settings,
            pr++,
            "primary adapter",
            g_c.pcble_primary);

        gtk_widget_set_tooltip_text(
            g_c.pcble_primary,
            "Pro Controller, or left Joy-Con. "
            "The physical Bluetooth address is saved because hci numbers "
            "can change after reboot.");

        g_c.pcble_secondary =
            gtk_combo_box_text_new();

        g_signal_connect(
            g_c.pcble_secondary,
            "changed",
            G_CALLBACK(pcble_adapter_changed),
            shell);

        add_row(
            g_c.pcble_settings,
            pr++,
            "right Joy-Con adapter",
            g_c.pcble_secondary);

        g_c.pcble_mouse_side = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_mouse_side), "Right Joy-Con 2");
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.pcble_mouse_side), "Left Joy-Con 2");
        g_signal_connect(g_c.pcble_mouse_side, "changed",
                         G_CALLBACK(pcble_mouse_side_changed), shell);
        add_row(g_c.pcble_settings, pr++, "BLE mouse sensor",
                g_c.pcble_mouse_side);

        gtk_widget_set_tooltip_text(
            g_c.pcble_secondary,
            "Used by all pair profiles. In reverse hybrid mode this is "
            "the Classic right-hand adapter, not the BLE adapter. "
            "Both adapters must be different.");

        g_c.pcble_paired =
            gtk_label_new("not paired yet");

        gtk_widget_set_halign(
            g_c.pcble_paired,
            GTK_ALIGN_START);

        gtk_label_set_selectable(
            GTK_LABEL(g_c.pcble_paired),
            TRUE);

        add_row(
            g_c.pcble_settings,
            pr++,
            "paired Switch",
            g_c.pcble_paired);

        g_c.pcble_status =
            gtk_label_new("offline");

        gtk_widget_set_halign(
            g_c.pcble_status,
            GTK_ALIGN_START);

        gtk_label_set_line_wrap(
            GTK_LABEL(g_c.pcble_status),
            TRUE);

        add_row(
            g_c.pcble_settings,
            pr++,
            "session",
            g_c.pcble_status);

        GtkWidget *buttons =
            gtk_box_new(
                GTK_ORIENTATION_HORIZONTAL,
                8);

        g_c.pcble_reconnect =
            gtk_button_new_with_label(
                "Reconnect paired Switch");

        g_c.pcble_pair =
            gtk_button_new_with_label(
                "Pair / Sync new Switch");

        g_c.pcble_stop =
            gtk_button_new_with_label(
                "Stop session");

        g_c.pcble_refresh =
            gtk_button_new_with_label(
                "Refresh adapters");

        g_signal_connect(
            g_c.pcble_reconnect,
            "clicked",
            G_CALLBACK(pcble_reconnect_clicked),
            shell);

        g_signal_connect(
            g_c.pcble_pair,
            "clicked",
            G_CALLBACK(pcble_pair_clicked),
            shell);

        g_signal_connect(
            g_c.pcble_stop,
            "clicked",
            G_CALLBACK(pcble_stop_clicked),
            shell);

        g_signal_connect(
            g_c.pcble_refresh,
            "clicked",
            G_CALLBACK(pcble_refresh_adapters),
            shell);

        gtk_box_pack_start(
            GTK_BOX(buttons),
            g_c.pcble_reconnect,
            FALSE, FALSE, 0);

        gtk_box_pack_start(
            GTK_BOX(buttons),
            g_c.pcble_pair,
            FALSE, FALSE, 0);

        gtk_box_pack_start(
            GTK_BOX(buttons),
            g_c.pcble_stop,
            FALSE, FALSE, 0);

        gtk_box_pack_start(
            GTK_BOX(buttons),
            g_c.pcble_refresh,
            FALSE, FALSE, 0);

        add_row(
            g_c.pcble_settings,
            pr++,
            "Bluetooth session",
            buttons);

        {
            GtkWidget *hint =
                gtk_label_new(
                    "For first pairing, press Pair / Sync and open "
                    "Controllers → Change Grip/Order on the Switch. "
                    "Reconnect uses the same Bluetooth adapter that performed "
                    "the original pairing.");

            gtk_widget_set_halign(
                hint,
                GTK_ALIGN_START);

            gtk_label_set_line_wrap(
                GTK_LABEL(hint),
                TRUE);

            gtk_label_set_max_width_chars(
                GTK_LABEL(hint),
                72);

            gtk_style_context_add_class(
                gtk_widget_get_style_context(hint),
                "dim-label");

            add_row(
                g_c.pcble_settings,
                pr++,
                "",
                hint);
        }

        add_section_header(
            g_c.pcble_settings,
            pr++,
            "Pro Controller appearance");

        g_c.pcble_body_color =
            gtk_color_button_new();

        g_c.pcble_button_color =
            gtk_color_button_new();

        g_c.pcble_left_grip_color =
            gtk_color_button_new();

        g_c.pcble_right_grip_color =
            gtk_color_button_new();

        GtkWidget *colors[] = {
            g_c.pcble_body_color,
            g_c.pcble_button_color,
            g_c.pcble_left_grip_color,
            g_c.pcble_right_grip_color,
        };

        for (unsigned i = 0;
             i < sizeof(colors) / sizeof(colors[0]);
             i++) {
            gtk_color_button_set_use_alpha(
                GTK_COLOR_BUTTON(colors[i]),
                FALSE);

            g_signal_connect(
                colors[i],
                "color-set",
                G_CALLBACK(pcble_color_changed),
                shell);
        }

        add_row(
            g_c.pcble_settings,
            pr++,
            "body",
            g_c.pcble_body_color);

        add_row(
            g_c.pcble_settings,
            pr++,
            "buttons",
            g_c.pcble_button_color);

        add_row(
            g_c.pcble_settings,
            pr++,
            "left grip",
            g_c.pcble_left_grip_color);

        add_row(
            g_c.pcble_settings,
            pr++,
            "right grip",
            g_c.pcble_right_grip_color);

        /* The logging toggle must exist before pcble_load_config(), but
         * Diagnostics now begins the SECOND output column. Its GtkGrid
         * parent is assigned immediately after wozt_split_inner_section(). */
        g_c.pcble_verbose =
            gtk_check_button_new_with_label(
                "detailed Bluetooth / HID logging");

        g_signal_connect(
            g_c.pcble_verbose,
            "toggled",
            G_CALLBACK(pcble_verbose_changed),
            shell);
    }

    /* Pair the four appearance controls into two compact rows. */
    wozt_pair_named_rows(g_c.pcble_settings, "body", "buttons");
    wozt_pair_named_rows(g_c.pcble_settings, "left grip", "right grip");
    pcble_load_config(shell);


    /* --- right output column: Diagnostics, then Output shaping ---- */
    grid = wozt_split_inner_section(grid, 1);
    row = 0;

    /* WOZT_0845_RIGHT_DIAGNOSTICS
     * Keep the PCBLE-only logging control and its original signal alive.
     * Its compact group is the FIRST right-column row, above shaping.
     * The visible property is bound to the backend's PCBLE panel so the
     * Diagnostics block vanishes for Titan/other backends just as before.
     * no-show-all prevents the main window's show_all from un-hiding it. */
    GtkWidget *pcble_diagnostics = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(pcble_diagnostics), 2);
    gtk_grid_set_column_spacing(GTK_GRID(pcble_diagnostics), 3);
    add_section_header(pcble_diagnostics, 0, "Diagnostics");
    add_row(pcble_diagnostics, 1, "logging", g_c.pcble_verbose);
    /* GTK show_all() skips a no-show-all container AND its children.
     * Make the heading and checkbox visible FIRST, then preserve the
     * backend-dependent visibility binding on the container itself. */
    gtk_widget_show_all(pcble_diagnostics);
    gtk_widget_set_no_show_all(pcble_diagnostics, TRUE);
    gtk_grid_attach(GTK_GRID(grid), pcble_diagnostics, 0, row++, 2, 1);
    g_object_bind_property(g_c.pcble_settings, "visible",
                           pcble_diagnostics, "visible", G_BINDING_SYNC_CREATE);

    add_section_header(
        grid,
        row++,
        "Output shaping");

    add_row(
        grid,
        row++,
        "visual preview",
        make_stick_preview_row(
            shell,
            1));

    {
        GtkWidget *note =
            gtk_label_new(
                "Applied after browser, native and local-controller inputs "
                "are merged, immediately before the selected backend. "
                "Changes take effect live; no restart is required.");

        gtk_widget_set_halign(
            note,
            GTK_ALIGN_START);

        gtk_label_set_line_wrap(
            GTK_LABEL(note),
            TRUE);

        gtk_label_set_max_width_chars(
            GTK_LABEL(note),
            72);

        gtk_style_context_add_class(
            gtk_widget_get_style_context(note),
            "dim-label");

        add_row(
            grid,
            row++,
            "",
            note);
    }

    g_c.output_invert_ry = add_row(
        grid,
        row++,
        "right stick",
        make_check(
            shell,
            "invert up/down"));

    preview_attach_output_signal(
        g_c.output_invert_ry,
        "toggled");

    g_c.output_lt_threshold = add_row(
        grid,
        row++,
        "LT threshold (%)",
        make_scale(
            shell,
            0,
            100,
            5,
            "%"));

    preview_attach_output_signal(
        g_c.output_lt_threshold,
        "value-changed");

    gtk_widget_set_tooltip_text(
        g_c.output_lt_threshold,
        "0 preserves the incoming trigger value. "
        "A positive value turns it into a digital 0/100 press.");

    g_c.output_rt_threshold = add_row(
        grid,
        row++,
        "RT threshold (%)",
        make_scale(
            shell,
            0,
            100,
            5,
            "%"));

    preview_attach_output_signal(
        g_c.output_rt_threshold,
        "value-changed");

    gtk_widget_set_tooltip_text(
        g_c.output_rt_threshold,
        "0 preserves the incoming trigger value. "
        "A positive value turns it into a digital 0/100 press.");

    static const char *OUTPUT_SIDE[2] = {
        "left",
        "right"
    };

    for (int i = 0; i < 2; i++) {
        char label[64];

        snprintf(
            label,
            sizeof(label),
            "%s stick deadzone (%%)",
            OUTPUT_SIDE[i]);

        g_c.output_deadzone[i] = add_row(
            grid,
            row++,
            label,
            make_scale(
                shell,
                0,
                40,
                1,
                "%"));

        preview_attach_output_signal(
            g_c.output_deadzone[i],
            "value-changed");

        snprintf(
            label,
            sizeof(label),
            "%s axis saturation (%%)",
            OUTPUT_SIDE[i]);

        g_c.output_range[i] = add_row(
            grid,
            row++,
            label,
            make_scale(
                shell,
                45,
                100,
                1,
                "%"));

        preview_attach_output_signal(
            g_c.output_range[i],
            "value-changed");

        gtk_widget_set_tooltip_text(
            g_c.output_range[i],
            "Cardinal-axis saturation point. For example, 90% means an "
            "incoming 90% push along X or Y is sent to the console as 100%.");

        snprintf(
            label,
            sizeof(label),
            "%s diagonal saturation (%%)",
            OUTPUT_SIDE[i]);

        g_c.output_diagonal[i] = add_row(
            grid,
            row++,
            label,
            make_scale(
                shell,
                45,
                100,
                1,
                "%"));

        preview_attach_output_signal(
            g_c.output_diagonal[i],
            "value-changed");

        gtk_widget_set_tooltip_text(
            g_c.output_diagonal[i],
            "45-degree saturation point. This is separate from range because "
            "many physical stick gates reach a different radius in corners.");
    }


    /* Backend left, shaping right: two compact inner columns. */
    wozt_compact_controller_output();

    /* ===============================================================
     * LOCAL CONTROLLER INPUT
     * =============================================================== */
    make_stack_page(
        stack,
        "local-controller-input",
        "Local controller input",
        "Only the physical SDL controller connected to this computer. "
        "These settings are applied before its state joins browser and "
        "native-client input.",
        &grid);

    /* Source and calibration share this ONE GtkGrid, top to bottom. */
    g_c.wozt_inner_input = grid;
    row = 0;

    add_section_header(grid, row++, "Source controller");

    /* No orphan label column: checkbox starts at the LEFT edge of
     * the one-column source/calibration grid. Keep its original callback. */
    g_c.gamepad_enabled = make_check(shell, "send input from this machine");
    gtk_widget_set_hexpand(g_c.gamepad_enabled, FALSE);
    gtk_widget_set_halign(g_c.gamepad_enabled, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), g_c.gamepad_enabled, 0, row++, 2, 1);

    g_c.gamepad_device = gtk_combo_box_text_new();
    g_signal_connect(
        g_c.gamepad_device,
        "changed",
        G_CALLBACK(on_combo),
        shell);
    add_row(grid, row++, "device", g_c.gamepad_device);

    add_row(
        grid,
        row++,
        "visual preview",
        make_stick_preview_row(
            shell,
            0));

    {
        GtkWidget *note =
            gtk_label_new(
                "These controls affect only the local SDL controller. "
                "For calibration shared by every source, use Output shaping "
                "on the Controller output page.");

        gtk_widget_set_halign(
            note,
            GTK_ALIGN_START);

        gtk_label_set_line_wrap(
            GTK_LABEL(note),
            TRUE);

        gtk_label_set_max_width_chars(
            GTK_LABEL(note),
            72);

        gtk_style_context_add_class(
            gtk_widget_get_style_context(note),
            "dim-label");

        add_row(
            grid,
            row++,
            "",
            note);
    }

    /* Calibration follows immediately after the previews, without
     * any nested column or extra section gap. */
    add_section_header(grid, row, "Input calibration and Input shaping");
    GtkWidget *input_calibration_header =
        gtk_grid_get_child_at(GTK_GRID(grid), 0, row);
    if (input_calibration_header)
        gtk_widget_set_margin_top(input_calibration_header, 0);
    ++row;

    g_c.invert_ry = add_row(
        grid,
        row++,
        "right stick",
        make_check(shell, "invert up/down"));

    preview_attach_input_signal(
        g_c.invert_ry,
        "toggled");

    g_c.lt_threshold = add_row(
        grid,
        row++,
        "LT threshold (%)",
        make_scale(shell, 0, 100, 5, "%"));

    preview_attach_input_signal(
        g_c.lt_threshold,
        "value-changed");

    g_c.rt_threshold = add_row(
        grid,
        row++,
        "RT threshold (%)",
        make_scale(shell, 0, 100, 5, "%"));

    preview_attach_input_signal(
        g_c.rt_threshold,
        "value-changed");

    static const char *SIDE[2] = {"left", "right"};

    for (int i = 0; i < 2; i++) {
        char label[48];

        snprintf(
            label,
            sizeof(label),
            "%s stick deadzone (%%)",
            SIDE[i]);
        g_c.deadzone[i] = add_row(
            grid,
            row++,
            label,
            make_scale(shell, 0, 40, 1, "%"));

        preview_attach_input_signal(
            g_c.deadzone[i],
            "value-changed");

        snprintf(
            label,
            sizeof(label),
            "%s axis saturation (%%)",
            SIDE[i]);
        g_c.range[i] = add_row(
            grid,
            row++,
            label,
            make_scale(shell, 45, 100, 1, "%"));

        preview_attach_input_signal(
            g_c.range[i],
            "value-changed");

        snprintf(
            label,
            sizeof(label),
            "%s diagonal saturation (%%)",
            SIDE[i]);
        g_c.diagonal[i] = add_row(
            grid,
            row++,
            label,
            make_scale(shell, 45, 100, 1, "%"));

        preview_attach_input_signal(
            g_c.diagonal[i],
            "value-changed");
    }


    /* Source/previews/calibration share a single vertical grid. */
    wozt_compact_controller_input();

    /* ===============================================================
     * CAPTURE
     * =============================================================== */
    make_stack_page(
        stack,
        "capture",
        "Capture",
        "Settings for the HDMI capture source itself. These sit upstream of "
        "every client and therefore affect the whole server.",
        &grid);

    row = 0;

    g_c.capture_format = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.capture_format),
        "YUYV (raw)");
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.capture_format),
        "MJPEG (decoded)");

    g_signal_connect(
        g_c.capture_format,
        "changed",
        G_CALLBACK(on_combo),
        shell);

    add_row(
        grid,
        row++,
        "capture format",
        g_c.capture_format);
    /* A one-setting frame should not impose a wide column. */
    gtk_grid_set_row_spacing(GTK_GRID(grid), 1);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
    gtk_widget_set_halign(grid, GTK_ALIGN_START);
    gtk_widget_set_hexpand(grid, FALSE);
    wozt_unellipsize_pcble_combo(g_c.capture_format, 155);


    /* ===============================================================
     * LOCAL MONITOR
     * =============================================================== */
    make_stack_page(
        stack,
        "local-monitor",
        "Local monitor",
        "These controls affect only this PC's preview window and speakers. "
        "They do not alter what remote clients receive.",
        &grid);

    row = 0;

    g_c.muted = add_row(
        grid,
        row++,
        "speakers",
        make_check(shell, "mute"));

    g_c.volume = add_row(
        grid,
        row++,
        "volume (%)",
        make_scale(shell, 0, 100, 5, "%"));

    g_c.direct_sink = add_row(
        grid,
        row++,
        "audio output",
        make_check(shell, "use LOCAL_SINK directly"));

    g_c.brightness = add_row(
        grid,
        row++,
        "brightness (%)",
        make_scale(shell, 50, 150, 5, "%"));

    g_c.contrast = add_row(
        grid,
        row++,
        "contrast (%)",
        make_scale(shell, 50, 150, 5, "%"));

    g_c.vsync = add_row(
        grid,
        row++,
        "drawing",
        make_check(shell, "wait for display (vsync)"));

    /* One compact column, exactly like the controller panels.
     * Keep all six original controls and signal connections. */
    gtk_grid_set_row_spacing(GTK_GRID(grid), 1);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
    gtk_widget_set_halign(grid, GTK_ALIGN_START);
    gtk_widget_set_hexpand(grid, FALSE);
    GtkWidget *monitor_scales[] = {g_c.volume, g_c.brightness, g_c.contrast};
    for (guint i = 0; i < G_N_ELEMENTS(monitor_scales); ++i) {
        GtkWidget *scale = monitor_scales[i];
        gtk_widget_set_hexpand(scale, FALSE);
        gtk_widget_set_margin_top(scale, 0);
        gtk_widget_set_margin_bottom(scale, 0);
        gtk_widget_set_size_request(scale, 116, 15);
        gtk_widget_set_valign(scale, GTK_ALIGN_FILL);
        gtk_style_context_add_class(gtk_widget_get_style_context(scale),
                                    "wozt-calibration-scale");
    }


    /* ===============================================================
     * RESET METHOD
     * =============================================================== */
    make_stack_page(
        stack,
        "reset-method",
        "Reset method",
        "Choose how Capture2Cloud wakes the target console. This is "
        "independent from the controller-output backend.",
        &grid);

    row = 0;

    add_section_header(
        grid,
        row++,
        "Wake method");

    g_c.reset_method =
        gtk_combo_box_text_new();

    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.reset_method),
        "Bash script");

    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(g_c.reset_method),
        "Bluetooth beacon");

    g_signal_connect(
        g_c.reset_method,
        "changed",
        G_CALLBACK(reset_method_changed),
        shell);

    add_row(
        grid,
        row++,
        "method",
        g_c.reset_method);


    /* --- script method --------------------------------------------- */

    g_c.reset_script_panel =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(g_c.reset_script_panel),
        3);

    gtk_grid_set_column_spacing(
        GTK_GRID(g_c.reset_script_panel),
        14);

    gtk_widget_set_no_show_all(
        g_c.reset_script_panel,
        TRUE);

    gtk_grid_attach(
        GTK_GRID(grid),
        g_c.reset_script_panel,
        0,
        row++,
        2,
        1);

    {
        int rr = 0;

        add_section_header(
            g_c.reset_script_panel,
            rr++,
            "Bash script");

        g_c.reset_script =
            gtk_entry_new();

        gtk_entry_set_placeholder_text(
            GTK_ENTRY(g_c.reset_script),
            "scripts/wake_console.sh");

        g_signal_connect(
            g_c.reset_script,
            "changed",
            G_CALLBACK(reset_script_changed),
            shell);

        add_row(
            g_c.reset_script_panel,
            rr++,
            "script",
            g_c.reset_script);

        GtkWidget *note =
            gtk_label_new(
                "The bundled wake_console.sh keeps the existing Home "
                "Assistant power-cycle method. Relative paths are resolved "
                "from the Capture2Cloud directory.");

        gtk_widget_set_halign(
            note,
            GTK_ALIGN_START);

        gtk_label_set_line_wrap(
            GTK_LABEL(note),
            TRUE);

        gtk_label_set_max_width_chars(
            GTK_LABEL(note),
            72);

        gtk_style_context_add_class(
            gtk_widget_get_style_context(note),
            "dim-label");

        add_row(
            g_c.reset_script_panel,
            rr++,
            "",
            note);
    }


    /* --- Bluetooth beacon method --------------------------------- */

    g_c.reset_bt_panel =
        gtk_grid_new();

    gtk_grid_set_row_spacing(
        GTK_GRID(g_c.reset_bt_panel),
        3);

    gtk_grid_set_column_spacing(
        GTK_GRID(g_c.reset_bt_panel),
        14);

    gtk_widget_set_no_show_all(
        g_c.reset_bt_panel,
        TRUE);

    gtk_grid_attach(
        GTK_GRID(grid),
        g_c.reset_bt_panel,
        0,
        row++,
        2,
        1);

    {
        int rr = 0;

        add_section_header(
            g_c.reset_bt_panel,
            rr++,
            "Bluetooth wake beacon");

        g_c.reset_bt_target =
            gtk_combo_box_text_new();

        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(g_c.reset_bt_target),
            "Nintendo Switch 2");

        g_signal_connect(
            g_c.reset_bt_target,
            "changed",
            G_CALLBACK(reset_target_changed),
            shell);

        add_row(
            g_c.reset_bt_panel,
            rr++,
            "target console",
            g_c.reset_bt_target);

        g_c.reset_bt_adapter =
            gtk_combo_box_text_new();

        g_signal_connect(
            g_c.reset_bt_adapter,
            "changed",
            G_CALLBACK(reset_adapter_changed),
            shell);

        add_row(
            g_c.reset_bt_panel,
            rr++,
            "Bluetooth adapter",
            g_c.reset_bt_adapter);

        {
            GtkWidget *buttons =
                gtk_box_new(
                    GTK_ORIENTATION_HORIZONTAL,
                    8);

            g_c.reset_bt_refresh =
                gtk_button_new_with_label(
                    "Refresh adapters");

            g_c.reset_bt_test_adapter =
                gtk_button_new_with_label(
                    "Test dongle");

            g_signal_connect(
                g_c.reset_bt_refresh,
                "clicked",
                G_CALLBACK(reset_refresh_adapters),
                shell);

            g_signal_connect(
                g_c.reset_bt_test_adapter,
                "clicked",
                G_CALLBACK(reset_test_adapter_clicked),
                shell);

            gtk_box_pack_start(
                GTK_BOX(buttons),
                g_c.reset_bt_refresh,
                FALSE, FALSE, 0);

            gtk_box_pack_start(
                GTK_BOX(buttons),
                g_c.reset_bt_test_adapter,
                FALSE, FALSE, 0);

            add_row(
                g_c.reset_bt_panel,
                rr++,
                "adapter tools",
                buttons);
        }

        /* The buttons already say Capture/Test beacon. A second Wake beacon
         * heading wastes a full line inside this compact method panel. */
        {
            GtkWidget *buttons =
                gtk_box_new(
                    GTK_ORIENTATION_HORIZONTAL,
                    8);

            g_c.reset_bt_capture =
                gtk_button_new_with_label(
                    "Capture wake beacon");

            g_c.reset_bt_test_beacon =
                gtk_button_new_with_label(
                    "Test beacon");

            g_signal_connect(
                g_c.reset_bt_capture,
                "clicked",
                G_CALLBACK(reset_capture_clicked),
                shell);

            g_signal_connect(
                g_c.reset_bt_test_beacon,
                "clicked",
                G_CALLBACK(reset_test_beacon_clicked),
                shell);

            gtk_box_pack_start(
                GTK_BOX(buttons),
                g_c.reset_bt_capture,
                FALSE, FALSE, 0);

            gtk_box_pack_start(
                GTK_BOX(buttons),
                g_c.reset_bt_test_beacon,
                FALSE, FALSE, 0);

            add_row(
                g_c.reset_bt_panel,
                rr++,
                "beacon",
                buttons);
        }

        {
            GtkWidget *hint =
                gtk_label_new(
                    "Capture listens for the wake advertisement of an "
                    "already-paired Switch 2 controller. The captured beacon "
                    "will be stored locally and can then be replayed without "
                    "the physical controller.");

            gtk_widget_set_halign(
                hint,
                GTK_ALIGN_START);

            gtk_label_set_line_wrap(
                GTK_LABEL(hint),
                TRUE);

            gtk_label_set_max_width_chars(
                GTK_LABEL(hint),
                72);

            gtk_style_context_add_class(
                gtk_widget_get_style_context(hint),
                "dim-label");

            add_row(
                g_c.reset_bt_panel,
                rr++,
                "",
                hint);
        }

        g_c.reset_bt_status =
            gtk_label_new("Bluetooth wake not configured.");
        gtk_widget_set_halign(g_c.reset_bt_status, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(g_c.reset_bt_status, TRUE);
        gtk_label_set_xalign(GTK_LABEL(g_c.reset_bt_status), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(g_c.reset_bt_status), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(g_c.reset_bt_status), FALSE);
        gtk_label_set_single_line_mode(GTK_LABEL(g_c.reset_bt_status), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(g_c.reset_bt_status),
                                PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(g_c.reset_bt_status), 42);
        g_signal_connect(g_c.reset_bt_status, "notify::label",
                         G_CALLBACK(wozt_reset_status_tooltip_changed), NULL);
        gtk_widget_set_tooltip_text(g_c.reset_bt_status,
                                    gtk_label_get_text(GTK_LABEL(g_c.reset_bt_status)));
        /* Two columns, no "status" label; never add another blank grid cell. */
        gtk_grid_attach(GTK_GRID(g_c.reset_bt_panel),
                        g_c.reset_bt_status, 0, rr++, 2, 1);
    }

    wozt_compact_reset_method(grid);
    reset_load_config(shell);


    /* ===============================================================
     * MAINTENANCE
     * =============================================================== */
    make_stack_page(
        stack,
        "maintenance",
        "Maintenance",
        "One-shot server and hardware actions. These are operations, not "
        "client preferences.",
        &grid);

    row = 0;

    add_section_header(grid, row++, "Console power");

    add_row(
        grid,
        row++,
        "console",
        make_button(
            shell,
            "wake the console",
            GTK_SHELL_ACTION_WAKE_CONSOLE,
            "Runs the configured console wake action, then recovers controller output."));

    /*
     * Controller-output maintenance is backend-specific.
     *
     * The selected backend is a startup setting, so if it changes the
     * server restarts and this page is rebuilt with the new backend's
     * own maintenance operation and wording.
     */
    {
        int backend;

        SDL_LockMutex(shell->lock);
        backend = shell->settings.output_backend;
        SDL_UnlockMutex(shell->lock);

        char section[160];
        snprintf(
            section,
            sizeof(section),
            "Controller output — %s",
            gamepad_bridge_backend_label(backend));
        add_section_header(grid, row++, section);

        const char *label =
            gamepad_bridge_backend_maintenance_label(backend);
        const char *help =
            gamepad_bridge_backend_maintenance_help(backend);

        GtkWidget *button = make_button(
            shell,
            (label && *label) ? label : "no maintenance action",
            GTK_SHELL_ACTION_RECOVER_OUTPUT,
            (help && *help)
                ? help
                : "This backend does not expose a maintenance action.");

        g_c.backend_recovery =
            button;

        gtk_widget_set_sensitive(
            button,
            gamepad_bridge_backend_available(backend) &&
            label &&
            *label);

        add_row(
            grid,
            row++,
            "backend recovery",
            button);

        if (help && *help) {
            GtkWidget *note = gtk_label_new(help);
            gtk_widget_set_halign(note, GTK_ALIGN_START);
            gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(note), 72);
            gtk_style_context_add_class(
                gtk_widget_get_style_context(note),
                "dim-label");
            add_row(grid, row++, "", note);
        }
    }

    add_section_header(grid, row++, "Server process");

    add_row(
        grid,
        row++,
        "server",
        make_button(
            shell,
            "restart server",
            GTK_SHELL_ACTION_RESTART,
            "Performs the normal ordered shutdown and re-execs "
            "Capture2Cloud."));

    wozt_compact_maintenance(grid);

    /* ---------------------------------------------------------------
     * Footer
     * --------------------------------------------------------------- */
    gtk_box_pack_start(
        GTK_BOX(root),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
        FALSE,
        FALSE,
        0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(footer, 14);
    gtk_widget_set_margin_end(footer, 14);
    gtk_widget_set_margin_top(footer, 7);
    gtk_widget_set_margin_bottom(footer, 9);

    g_c.status_label = gtk_label_new("");
    gtk_widget_set_halign(g_c.status_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(g_c.status_label, TRUE);
    gtk_box_pack_start(
        GTK_BOX(footer),
        g_c.status_label,
        TRUE,
        TRUE,
        0);

    {
        char ver[96];
        snprintf(
            ver,
            sizeof(ver),
            "<small><span foreground=\"#888888\">v%s</span></small>",
            C2C_VERSION);

        GtkWidget *version = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(version), ver);
        gtk_widget_set_halign(version, GTK_ALIGN_END);

        gtk_box_pack_end(
            GTK_BOX(footer),
            version,
            FALSE,
            FALSE,
            0);
    }

    gtk_box_pack_end(GTK_BOX(root), footer, FALSE, FALSE, 0);

    set_window_icon(win);
    shell->settings_window = win;
    wozt_compact_action_labels(dashboard);

    load_controls(shell);
    shell->loading = 1;
    wozt_ui_standard_apply_language(shell->wozt,
        wozt_ui_standard_language_id(shell->wozt), shell->settings_window);
    shell->loading = 0;
}

/* Opens the settings window from outside, for the layout probe in the
 * tests. Nothing in the program calls it; the tray menu below is how a
 * person opens it. */
void gtk_shell_debug_show_settings(GtkShell *shell);
static gboolean debug_show_cb(gpointer user_data) {
    GtkShell *shell = user_data;
    if (shell->settings_window) {
        load_controls(shell);
        gtk_widget_show_all(shell->settings_window);
        pcble_update_controls(shell);
        reset_update_controls(shell);
        gtk_window_present(GTK_WINDOW(shell->settings_window));
    }
    return G_SOURCE_REMOVE;
}
void gtk_shell_debug_show_settings(GtkShell *shell) {
    if (shell) {
        g_idle_add(debug_show_cb, shell);
    }
}

/* --- the tray icon ---------------------------------------------------- */

static void on_menu_settings(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkShell *shell = user_data;
    load_controls(shell);
    gtk_widget_show_all(shell->settings_window);

    /* show_all() walks the entire window. Restore the backend-specific
     * visibility afterwards so only the selected backend's controls are
     * visible. */
    pcble_update_controls(shell);
    reset_update_controls(shell);

    gtk_window_present(GTK_WINDOW(shell->settings_window));
}

static void on_menu_gamepad(GtkCheckMenuItem *item, gpointer user_data) {
    GtkShell *shell = user_data;
    if (shell->loading) {
        return;
    }
    SDL_LockMutex(shell->lock);
    shell->settings.gamepad_enabled = gtk_check_menu_item_get_active(item);
    SDL_UnlockMutex(shell->lock);
    publish(shell);
    load_controls(shell);
}

static void on_menu_show_capture(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    act(user_data, GTK_SHELL_ACTION_SHOW_CAPTURE);
}

static void on_menu_quit(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    act(user_data, GTK_SHELL_ACTION_QUIT);
}

/*
 * Pairing, which is a ceremony rather than a setting.
 *
 * The pad is told a PIN of eight digits, and the first four are chosen
 * by the person: the GamePad shows them as card suits on its own screen
 * -- spade, heart, diamond, club -- and the last four are always 5678.
 * So this asks for four symbols rather than a number, because that is
 * what is written on the thing you are holding.
 *
 * It only arms the access point and the WPS exchange; the pad's own
 * "sync" is what completes it, and there is about a minute to press it.
 */
static void on_pair_clicked(GtkWidget *w, gpointer user_data) {
    (void)w;
    GtkShell *shell = user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Pair a Wii U GamePad", GTK_WINDOW(shell->settings_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL, "Start pairing", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_box_set_spacing(GTK_BOX(box), 10);

    GtkWidget *intro = gtk_label_new(
        "Choose the four symbols the pad will ask for, then start it.\n"
        "On the GamePad: Settings, then sync, and enter the same four.");
    gtk_widget_set_halign(intro, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *pick[4];
    /* The order IS the encoding: spade 0, heart 1, diamond 2, club 3. */
    static const char *const kSuits[] = { "\u2660", "\u2665", "\u2666", "\u2663" };
    for (int i = 0; i < 4; i++) {
        pick[i] = gtk_combo_box_text_new();
        for (int s2 = 0; s2 < 4; s2++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pick[i]), kSuits[s2]);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(pick[i]), i);
        gtk_box_pack_start(GTK_BOX(row), pick[i], TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *note = gtk_label_new(
        "The access point restarts while this runs, so anything already "
        "connected to it drops.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    const int answer = gtk_dialog_run(GTK_DIALOG(dialog));
    if (answer == GTK_RESPONSE_ACCEPT) {
        char pin[16];
        for (int i = 0; i < 4; i++) {
            int chosen = gtk_combo_box_get_active(GTK_COMBO_BOX(pick[i]));
            if (chosen < 0) chosen = 0;
            pin[i] = (char)('0' + chosen);
        }
        /* The four the pad always expects after the ones you chose. */
        memcpy(pin + 4, "5678", 5);
        if (shell->callbacks.on_pair) {
            shell->callbacks.on_pair(shell->callbacks.userdata, pin);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_icon_popup(GtkStatusIcon *icon, guint button, guint activate_time,
                          gpointer user_data) {
    GtkShell *shell = user_data;

    /* The one from the last right click. gtk_menu_new() hands back a
     * floating reference that gtk_menu_popup() does not take over, so a
     * menu built per click and never destroyed is a menu leaked per
     * click. */
    if (shell->menu) {
        gtk_widget_destroy(shell->menu);
        g_object_unref(shell->menu);   /* the ref_sink below took one */
        shell->menu = NULL;
    }

    GtkWidget *menu = gtk_menu_new();
    g_object_ref_sink(menu);
    shell->menu = menu;

    GtkWidget *show = gtk_menu_item_new_with_label("Show capture");
    gtk_widget_set_tooltip_text(show,
        "Brings the video window back -- after closing it by accident, or from "
        "headless, where there was none to begin with.");
    g_signal_connect(show, "activate", G_CALLBACK(on_menu_show_capture), shell);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show);

    GtkWidget *settings = gtk_menu_item_new_with_label("Settings...");
    g_signal_connect(settings, "activate", G_CALLBACK(on_menu_settings), shell);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), settings);

    SDL_LockMutex(shell->lock);
    const int gamepad_on = shell->settings.gamepad_enabled;
    SDL_UnlockMutex(shell->lock);
    GtkWidget *pad = gtk_check_menu_item_new_with_label("Send controller input");
    shell->loading = 1;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(pad), gamepad_on);
    shell->loading = 0;
    g_signal_connect(pad, "toggled", G_CALLBACK(on_menu_gamepad), shell);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pad);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit, "activate", G_CALLBACK(on_menu_quit), shell);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);

    gtk_widget_show_all(menu);

    /*
     * gtk_menu_popup(), with the button and the timestamp this signal
     * was handed -- not gtk_menu_popup_at_pointer(), which was here and
     * which cannot work under the tray this actually runs on.
     *
     * KDE has no XEmbed tray of its own: xembedsniproxy takes this icon
     * and republishes it as a StatusNotifierItem, so a right click on it
     * reaches this program as a SYNTHESISED X11 button press. There is
     * no real current event behind it, and popup_at_pointer asks GTK to
     * go and find one -- it finds nothing, and takes the menu's pointer
     * grab with GDK_CURRENT_TIME. A grab with no timestamp does not win
     * against what plasmashell is already holding, so the menu appeared,
     * highlighted under the pointer, and never saw the button release
     * that becomes "activate". Every item did nothing; Quit was simply
     * the one anybody noticed.
     *
     * The two values that fix it arrive as arguments to this very
     * function, and were being discarded.
     */
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL,
                   gtk_status_icon_position_menu, icon,
                   button, activate_time);
}

static void on_icon_activate(GtkStatusIcon *icon, gpointer user_data) {
    (void)icon;
    /* Left click opens the settings, which is what a left click on a
     * tray icon is expected to do. */
    on_menu_settings(NULL, user_data);
}

/* --- the loop -------------------------------------------------------- */

static void reload_controllers(GtkShell *shell);

/* Picks up what the program changed behind the interface's back. A
 * timeout rather than an idle callback so it costs nothing while
 * nothing is happening. */
static gboolean on_tick(gpointer user_data) {
    GtkShell *shell = user_data;
    if (!shell->running) {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    if (shell->controllers_dirty) {
        shell->controllers_dirty = 0;
        reload_controllers(shell);
    }
    if (shell->settings_dirty) {
        shell->settings_dirty = 0;
        load_controls(shell);
    }
    if (shell->client_status_dirty) {
        shell->client_status_dirty = 0;
        char text[GTK_SHELL_CLIENT_COUNT][160];
        SDL_LockMutex(shell->lock);
        memcpy(text, shell->client_status_text, sizeof(text));
        SDL_UnlockMutex(shell->lock);
        for (int i = 0; i < GTK_SHELL_CLIENT_COUNT; i++) {
            if (g_c.client_status[i]) {
                gtk_label_set_text(
                    GTK_LABEL(g_c.client_status[i]),
                    text[i]);
            }
            if (g_c.overview_client_status[i]) {
                gtk_label_set_text(
                    GTK_LABEL(g_c.overview_client_status[i]),
                    wozt_overview_client_summary(text[i]));
                /* Full message remains accessible on hover. */
                gtk_widget_set_tooltip_text(
                    g_c.overview_client_status[i], text[i]);
            }
        }
    }
    if (shell->wup_metrics_dirty) {
        shell->wup_metrics_dirty = 0;
        char snapshot[240];
        SDL_LockMutex(shell->lock);
        snprintf(snapshot, sizeof(snapshot), "%s", shell->wup_metrics_text);
        SDL_UnlockMutex(shell->lock);
        if (g_c.wup_metrics_label)
            gtk_label_set_text(GTK_LABEL(g_c.wup_metrics_label), snapshot);
    }
    if (shell->wiiu_status_dirty) {
        shell->wiiu_status_dirty = 0;
        char text[160];
        SDL_LockMutex(shell->lock);
        snprintf(text, sizeof(text), "%s", shell->wiiu_status_text);
        SDL_UnlockMutex(shell->lock);
        if (g_c.wiiu_status) {
            gtk_label_set_text(GTK_LABEL(g_c.wiiu_status), text);
        }
    }
    if (shell->status_dirty) {
        shell->status_dirty = 0;
        char text[160];
        SDL_LockMutex(shell->lock);
        snprintf(text, sizeof(text), "%s", shell->status);
        SDL_UnlockMutex(shell->lock);
        if (g_c.status_label) {
            gtk_label_set_text(GTK_LABEL(g_c.status_label), text);
        }
        if (g_c.overview_status) {
            gtk_label_set_text(GTK_LABEL(g_c.overview_status), text);
        }
        gtk_status_icon_set_tooltip_text(shell->icon, text);
    }
    /* The replug, watched rather than assumed. Each stage waits for a
     * thing that actually happened, so the dialog cannot say "done"
     * while the pad is still dead. */
    if (g_c.replug_stage != REPLUG_IDLE && g_c.replug_dialog) {
        int up = gamepad_bridge_link_up();
        int found = gamepad_bridge_console();
        const char *text = NULL;
        switch (g_c.replug_stage) {
            case REPLUG_WAIT_UNPLUG:
                /* The program drops the link itself right after writing,
                 * so a brief gap proves nothing -- only a sustained one
                 * means a hand pulled the cable. */
                g_c.replug_down_ticks = up ? 0 : g_c.replug_down_ticks + 1;
                if (g_c.replug_down_ticks >= REPLUG_DOWN_TICKS) {
                    g_c.replug_stage = REPLUG_WAIT_BACK;
                    text = "Unplugged. Now plug both ends back in.";
                }
                break;
            case REPLUG_WAIT_BACK:
                if (up) {
                    g_c.replug_stage = REPLUG_WAIT_CONSOLE;
                    text = "Back on this machine. Waiting for the console...";
                }
                break;
            case REPLUG_WAIT_CONSOLE:
                if (found > 0) {
                    g_c.replug_stage = REPLUG_DONE;
                    text = "Done -- the console is answering the adapter.";
                    if (g_c.replug_close) {
                        gtk_widget_set_sensitive(g_c.replug_close, TRUE);
                    }
                } else if (!up) {
                    /* It went away again mid-way. */
                    g_c.replug_stage = REPLUG_WAIT_BACK;
                    text = "It went away again. Plug it back in.";
                }
                break;
            default:
                break;
        }
        if (text) {
            gtk_label_set_text(GTK_LABEL(g_c.replug_label), text);
        }
    }

    if (g_c.backend_status) {
        int configured = -1;

        SDL_LockMutex(shell->lock);
        configured = shell->settings.output_backend;
        SDL_UnlockMutex(shell->lock);

        char line[256];

        if (configured < 0 ||
            configured >= gamepad_bridge_backend_count()) {
            snprintf(line, sizeof(line), "unknown backend");
        } else if (!gamepad_bridge_backend_available(configured)) {
            snprintf(
                line,
                sizeof(line),
                "%s — not implemented in this build",
                gamepad_bridge_backend_label(configured));
        } else {
            const char *active = gamepad_bridge_backend_name();

            if (!active || !*active) {
                snprintf(
                    line,
                    sizeof(line),
                    "%s — configured, but not connected/initialised",
                    gamepad_bridge_backend_label(configured));
            } else {
                snprintf(
                    line,
                    sizeof(line),
                    "%s — %s — %.0f reports/s",
                    gamepad_bridge_backend_label(configured),
                    gamepad_bridge_link_up() ? "link up" : "link down",
                    gamepad_bridge_report_rate());
            }
        }

        if (strcmp(
                gtk_label_get_text(GTK_LABEL(g_c.backend_status)),
                line) != 0) {
            gtk_label_set_text(
                GTK_LABEL(g_c.backend_status),
                line);
        }
    }

    /* Refresh all four symbols from the same full status messages used
     * by the footer and the backend. No new polling or stream work. */
    if (g_c.overview_status && g_c.backend_status)
        wozt_overview_update_server_indicators(
            gtk_label_get_text(GTK_LABEL(g_c.overview_status)),
            gtk_label_get_text(GTK_LABEL(g_c.backend_status)));

    /*
     * GTK only observes backend recovery state. The backend itself is
     * serviced by Capture2Cloud's main loop, including in --headless.
     */
    pcble_refresh_status(shell);

    if (g_c.adapter_sees) {
        /* Read straight from the bridge every tick rather than pushed
         * through the settings struct: it is not a setting, it is what
         * the hardware is doing right now, and it changes on its own. */
        static const char *const FOUND[] = {
            "nothing -- replug the adapter into the console",
            "a PlayStation 3", "an Xbox 360", "a PlayStation 4",
            "an Xbox One", "a Switch",
        };
        int found = gamepad_bridge_console();
        const char *text = (found >= 0 && found < (int)(sizeof FOUND / sizeof *FOUND))
                               ? FOUND[found] : "no adapter";
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_c.adapter_sees)), text) != 0) {
            gtk_label_set_text(GTK_LABEL(g_c.adapter_sees), text);
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_check_embedded(gpointer user_data) {
    GtkShell *shell = user_data;
    if (gtk_status_icon_is_embedded(shell->icon)) {
        fprintf(stderr, "gtk_shell: tray icon shown\n");
    } else {
        fprintf(stderr, "gtk_shell: the desktop has no system tray -- no icon. "
                        "Settings are still reachable from the web page.\n");
    }
    return G_SOURCE_REMOVE;
}

static int gtk_thread_main(void *arg) {
    GtkShell *shell = arg;

    /* Checked rather than assumed: headless is also how this runs over
     * ssh and from a systemd unit, where there is no display at all. The
     * tray is a convenience; not having one is not a reason to refuse to
     * capture. */
    if (!gtk_init_check(NULL, NULL)) {
        fprintf(stderr, "gtk_shell: no display, running without a tray icon\n");
        shell->running = 0;
        return 0;
    }
    /* Numbers on the wire stay machine-readable.
     *
     * gtk_init sets the locale from the environment, which on this
     * machine is French -- and from that moment printf("%.1f") writes
     * "9,9". That reached an HTTP endpoint and broke a client parsing
     * it. The interface keeps the locale for everything it shows a
     * person; only the numeric part goes back to C, which is what any
     * program that formats numbers for another program has to do.
     *
     * Set here rather than in main because this is what changed it. */
    setlocale(LC_NUMERIC, "C");

    build_settings_window(shell);

    /* A stock icon: shipping one would mean an asset to install and a
     * path to find at runtime, for a picture in a 22-pixel box. */
    /* The project's own icon rather than a theme name. "camera-video" is
     * whatever the current icon theme decides it is, which is a
     * different picture on every desktop and no picture at all on some;
     * this is one image, shipped with the program, and the same one the
     * page and the console client show. */
    {
        char path[512];
        wozt_asset_path(path, sizeof(path), "assets/icon-64.png");
        shell->icon = gtk_status_icon_new_from_file(path);
    }
    gtk_status_icon_set_title(shell->icon, "Capture2Cloud");
    gtk_status_icon_set_tooltip_text(shell->icon, "Capture2Cloud");
    g_signal_connect(shell->icon, "popup-menu", G_CALLBACK(on_icon_popup), shell);
    g_signal_connect(shell->icon, "activate", G_CALLBACK(on_icon_activate), shell);

    /* Whether the tray actually took it, said out loud once.
     *
     * Embedding is asynchronous and can simply not happen -- a desktop
     * with no system tray, a session where nothing owns the XEmbed
     * selection. The icon then exists and is nowhere, which from the
     * outside is indistinguishable from the program having failed to
     * start it. One line settles that. */
    g_timeout_add_seconds(2, on_check_embedded, shell);

    g_timeout_add(200, on_tick, shell);
    gtk_main();
    wozt_ui_standard_free(shell->wozt);
    shell->wozt = NULL;

    /* On this thread, because it is this thread's widget: the shell is
     * torn down from the main one, which must not destroy something the
     * GTK loop could still have been showing a moment ago. */
    if (shell->menu) {
        gtk_widget_destroy(shell->menu);
        g_object_unref(shell->menu);
        shell->menu = NULL;
    }
    return 0;
}

/* --- the interface --------------------------------------------------- */

GtkShell *gtk_shell_start(const AppSettings *settings, const GtkShellCallbacks *callbacks) {
    GtkShell *shell = calloc(1, sizeof(*shell));
    if (!shell) {
        return NULL;
    }
    shell->settings = *settings;
    shell->callbacks = *callbacks;
    shell->running = 1;
    shell->lock = SDL_CreateMutex();
    if (!shell->lock) {
        free(shell);
        return NULL;
    }
    shell->thread = SDL_CreateThread(gtk_thread_main, "gtk-shell", shell);
    if (!shell->thread) {
        SDL_DestroyMutex(shell->lock);
        free(shell);
        return NULL;
    }
    return shell;
}

void gtk_shell_update(GtkShell *shell, const AppSettings *settings) {
    if (!shell) {
        return;
    }
    SDL_LockMutex(shell->lock);
    shell->settings = *settings;
    SDL_UnlockMutex(shell->lock);
    shell->settings_dirty = 1;
}

void gtk_shell_set_client_status(GtkShell *shell, GtkShellClient client, const char *text) {
    if (!shell || !text || client < 0 || client >= GTK_SHELL_CLIENT_COUNT) {
        return;
    }
    SDL_LockMutex(shell->lock);
    snprintf(shell->client_status_text[client], sizeof(shell->client_status_text[client]),
             "%s", text);
    SDL_UnlockMutex(shell->lock);
    shell->client_status_dirty = 1;
}

void gtk_shell_set_wup_metrics(GtkShell *shell, const char *text)
{
    if (!shell || !text) return;
    SDL_LockMutex(shell->lock);
    snprintf(shell->wup_metrics_text, sizeof(shell->wup_metrics_text), "%s", text);
    SDL_UnlockMutex(shell->lock);
    shell->wup_metrics_dirty = 1;
}

void gtk_shell_set_wiiu_status(GtkShell *shell, const char *text) {
    if (!shell || !text) {
        return;
    }
    SDL_LockMutex(shell->lock);
    snprintf(shell->wiiu_status_text, sizeof(shell->wiiu_status_text), "%s", text);
    shell->wiiu_status_dirty = 1;
    SDL_UnlockMutex(shell->lock);
}

void gtk_shell_set_status(GtkShell *shell, const char *text) {
    if (!shell || !text) {
        return;
    }
    SDL_LockMutex(shell->lock);
    snprintf(shell->status, sizeof(shell->status), "%s", text);
    SDL_UnlockMutex(shell->lock);
    shell->status_dirty = 1;
}

typedef struct {
    GtkShell *shell;
    char *message;
} ErrorRequest;

static gboolean show_error_on_gtk_thread(gpointer data) {
    ErrorRequest *req = data;
    GtkWidget *dialog = gtk_message_dialog_new(
        req->shell->settings_window ? GTK_WINDOW(req->shell->settings_window) : NULL,
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", req->message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    free(req->message);
    free(req);
    return G_SOURCE_REMOVE;
}

void gtk_shell_show_error(GtkShell *shell, const char *message) {
    if (!shell || !message) {
        return;
    }
    ErrorRequest *req = calloc(1, sizeof(*req));
    if (!req) {
        return;
    }
    req->shell = shell;
    req->message = strdup(message);
    g_idle_add(show_error_on_gtk_thread, req);
}

void gtk_shell_stop(GtkShell *shell) {
    if (!shell) {
        return;
    }
    shell->running = 0;
    /* The menu is the GTK thread's; dropping it here, before the join,
     * would be this thread destroying a widget the other one may still
     * be showing. It goes when that thread's loop ends. */
    if (shell->thread) {
        SDL_WaitThread(shell->thread, NULL);
    }
    SDL_DestroyMutex(shell->lock);
    free(shell);
}

void gtk_shell_set_controllers(GtkShell *shell, const char *const *names, int count) {
    if (!shell) {
        return;
    }
    if (count > 8) {
        count = 8;
    }
    SDL_LockMutex(shell->lock);
    int changed = (count != shell->controller_count);
    for (int i = 0; i < count; i++) {
        if (strncmp(shell->controllers[i], names[i], sizeof(shell->controllers[i])) != 0) {
            changed = 1;
        }
        snprintf(shell->controllers[i], sizeof(shell->controllers[i]), "%s", names[i]);
    }
    shell->controller_count = count;
    SDL_UnlockMutex(shell->lock);
    /* Only when it actually differs: rebuilding the list resets the
     * selection, and doing that every second would make the control
     * impossible to use. */
    if (changed) {
        shell->controllers_dirty = 1;
    }
}

/* Caller is on the GTK thread. */
static void reload_controllers(GtkShell *shell) {
    if (!g_c.gamepad_device) {
        return;
    }
    char names[8][96];
    int count, chosen;
    SDL_LockMutex(shell->lock);
    count = shell->controller_count;
    memcpy(names, shell->controllers, sizeof(names));
    chosen = shell->settings.gamepad_index;
    SDL_UnlockMutex(shell->lock);

    shell->loading = 1;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_c.gamepad_device));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.gamepad_device),
                                   count ? "none" : "none detected");
    for (int i = 0; i < count; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.gamepad_device), names[i]);
    }
    if (chosen >= count) {
        chosen = -1;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.gamepad_device), chosen + 1);
    shell->loading = 0;
}
