#include "gtk_shell.h"

#include "app_config.h"
#include "gamepad_bridge.h" /* the output-protocol names shown in the combo */

#include <SDL2/SDL.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    GtkShellCallbacks callbacks;

    /* The settings, and the lock around them. The GTK thread writes them
     * when a control moves; the program writes them when it refuses
     * something or corrects it. */
    SDL_mutex *lock;
    AppSettings settings;
    char status[160];

    /* Set while the code is filling the controls in from `settings`, so
     * the "value changed" handlers do not report those as the user
     * moving something -- which would echo straight back and, with two
     * threads, could ping-pong. */
    int loading;

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
    GtkWidget *gamepad_enabled, *gamepad_device, *invert_ry, *output_protocol;
    GtkWidget *adapter_sees;
    GtkWidget *lt_threshold, *rt_threshold;
    GtkWidget *deadzone[2], *range[2], *diagonal[2];
    GtkWidget *muted, *volume, *direct_sink, *brightness, *contrast, *vsync;
    GtkWidget *status_label;

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

/* --- the controls ---------------------------------------------------- */

static void on_toggle(GtkWidget *w, gpointer user_data) {
    GtkShell *shell = user_data;
    const gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    SDL_LockMutex(shell->lock);
    if (w == g_c.stream_enabled)       shell->settings.stream_enabled = on;
    else if (w == g_c.switch_enabled)  shell->settings.switch_enabled = on;
    else if (w == g_c.gamepad_enabled) shell->settings.gamepad_enabled = on;
    else if (w == g_c.invert_ry)       shell->settings.invert_ry = on;
    else if (w == g_c.direct_sink)     shell->settings.local_direct_sink = on;
    else if (w == g_c.muted)           shell->settings.local_muted = on;
    else if (w == g_c.vsync)           shell->settings.vsync = on;
    SDL_UnlockMutex(shell->lock);
    publish(shell);
}

static void on_spin(GtkWidget *w, gpointer user_data) {
    GtkShell *shell = user_data;
    const int v = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));
    SDL_LockMutex(shell->lock);
    if (w == g_c.port) shell->settings.web_port = v;
    else if (w == g_c.switch_port) shell->settings.switch_port = v;
    SDL_UnlockMutex(shell->lock);
    publish(shell);
}

static void on_scale(GtkWidget *w, gpointer user_data) {
    GtkShell *shell = user_data;
    const int v = (int)gtk_range_get_value(GTK_RANGE(w));
    SDL_LockMutex(shell->lock);
    if (w == g_c.bitrate)           shell->settings.bitrate_mbps = v;
    else if (w == g_c.lt_threshold) shell->settings.lt_threshold = v;
    else if (w == g_c.rt_threshold) shell->settings.rt_threshold = v;
    else if (w == g_c.deadzone[0])  shell->settings.stick_deadzone[0] = v;
    else if (w == g_c.deadzone[1])  shell->settings.stick_deadzone[1] = v;
    else if (w == g_c.range[0])     shell->settings.stick_range[0] = v;
    else if (w == g_c.range[1])     shell->settings.stick_range[1] = v;
    else if (w == g_c.diagonal[0])  shell->settings.stick_diagonal[0] = v;
    else if (w == g_c.diagonal[1])  shell->settings.stick_diagonal[1] = v;
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
    } else if (w == g_c.capture_format) {
        shell->settings.capture_mjpeg = (i == 1);
    } else if (w == g_c.gamepad_device) {
        shell->settings.gamepad_index = i - 1; /* the first row is "none" */
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

static GtkWidget *add_row(GtkWidget *grid, int row, const char *label, GtkWidget *control) {
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
    return control;
}

static GtkWidget *make_scale(GtkShell *shell, int lo, int hi, int step, const char *suffix) {
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    gtk_widget_set_size_request(s, 240, -1);
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

static GtkWidget *make_page(const char *title, GtkWidget *notebook, GtkWidget **grid_out) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid, gtk_label_new(title));
    *grid_out = grid;
    return grid;
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
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_c.switch_port), s.switch_port);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.resolution),
                             s.browser_height == 1080 ? 0 : (s.browser_height == 720 ? 1 : 2));
    gtk_range_set_value(GTK_RANGE(g_c.bitrate), s.bitrate_mbps);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_c.capture_format), s.capture_mjpeg ? 1 : 0);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_c.gamepad_enabled), s.gamepad_enabled);
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
    }

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
static void set_window_icon(GtkWidget *win) {
    char path[512];
    app_path(path, sizeof(path), "assets/icon-64.png");
    gtk_window_set_icon_from_file(GTK_WINDOW(win), path, NULL);
}

static void build_settings_window(GtkShell *shell) {
    /* Zero is a real protocol value ("automatic"), so "none pending" has
     * to be set rather than left to the static initialiser. */
    g_c.replug_pending = -1;
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Capture2Cloud settings");
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 520);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_settings_delete), shell);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), box);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(box), notebook, TRUE, TRUE, 0);

    GtkWidget *grid;
    int row;

    /* --- stream: the server's settings, shared by everyone watching --- */
    make_page("stream", notebook, &grid);
    row = 0;
    g_c.stream_enabled = add_row(grid, row++, "serve to browsers",
                                 make_check(shell, "on"));
    g_c.port = add_row(grid, row++, "port",
                       gtk_spin_button_new_with_range(1, 65535, 1));
    g_signal_connect(g_c.port, "value-changed", G_CALLBACK(on_spin), shell);

    g_c.switch_enabled = add_row(grid, row++, "serve to switch",
                                 make_check(shell, "on"));
    gtk_widget_set_tooltip_text(g_c.switch_enabled,
        "Whether the Switch client's server is listening. Separate from the web "
        "one because they are separate servers -- and a session with nobody on a "
        "Switch has no reason to hold a port open.");
    g_c.switch_port = add_row(grid, row++, "console port",
                              gtk_spin_button_new_with_range(1, 65535, 1));
    gtk_widget_set_tooltip_text(g_c.switch_port,
        "Where the Switch client connects. Its own port, because the two streams "
        "are two servers -- the browser's is HTTP, this one is a small binary "
        "protocol -- and moving one has no reason to move the other. Changing it "
        "disconnects whatever is connected: it has to be changed on the console "
        "as well.");
    g_signal_connect(g_c.switch_port, "value-changed", G_CALLBACK(on_spin), shell);

    g_c.resolution = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.resolution), "1080p60");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.resolution), "720p60");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.resolution), "480p60");
    gtk_widget_set_tooltip_text(g_c.resolution,
        "What the browser stream is encoded at. Shared: one encoder feeds every "
        "browser, so this changes what everyone watching sees. The console's own "
        "stream has its own size and is not affected.");
    g_signal_connect(g_c.resolution, "changed", G_CALLBACK(on_combo), shell);
    add_row(grid, row++, "browser resolution", g_c.resolution);

    g_c.bitrate = add_row(grid, row++, "bitrate (Mbps)", make_scale(shell, 2, 50, 1, ""));

    g_c.capture_format = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.capture_format), "YUYV (raw)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.capture_format), "MJPEG (decoded)");
    gtk_widget_set_tooltip_text(g_c.capture_format,
        "How the card delivers frames. YUYV costs no decode at all and is the "
        "default; MJPEG moves a fraction of the bytes over USB, which matters if "
        "the USB3 path is shared.");
    g_signal_connect(g_c.capture_format, "changed", G_CALLBACK(on_combo), shell);
    add_row(grid, row++, "capture format", g_c.capture_format);

    /* --- controller: this machine's, driving the console --- */
    make_page("controller", notebook, &grid);
    row = 0;
    g_c.gamepad_enabled = add_row(grid, row++, "send input",
                                  make_check(shell, "a controller here drives the console"));
    g_c.gamepad_device = gtk_combo_box_text_new();
    g_signal_connect(g_c.gamepad_device, "changed", G_CALLBACK(on_combo), shell);
    add_row(grid, row++, "controller", g_c.gamepad_device);
    g_c.output_protocol = gtk_combo_box_text_new();
    for (int i = 0; i < gamepad_protocol_count(); i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_c.output_protocol),
                                       gamepad_protocol_label(i));
    }
    g_signal_connect(g_c.output_protocol, "changed", G_CALLBACK(on_combo), shell);
    add_row(grid, row++, "adapter emulates", g_c.output_protocol);
    /* What the adapter FOUND on its output port, which is the half that
     * makes a mistake visible: set to a Switch pad with nothing
     * answering reads very differently from set to an Xbox pad with a
     * Switch in front of it. It also says when the change has not landed
     * yet -- after one, the adapter has to be replugged into the console
     * and until then this reads "nothing". */
    g_c.adapter_sees = gtk_label_new("");
    gtk_widget_set_halign(g_c.adapter_sees, GTK_ALIGN_START);
    add_row(grid, row++, "adapter sees", g_c.adapter_sees);
    gtk_widget_set_tooltip_text(g_c.output_protocol,
        "Which controller the adapter pretends to be to the console. Its own "
        "guess is not always right -- plugged straight into a Switch dock it "
        "guessed Xbox 360 and every button was ignored. Stored in the adapter, "
        "so it survives unplugging.\n\nAfter changing it, unplug the adapter from "
        "the console and plug it back in: the change ends its conversation with the "
        "console and nothing this machine can send reaches that side of it.");

    g_c.invert_ry = add_row(grid, row++, "right stick",
                            make_check(shell, "invert up/down"));
    g_c.lt_threshold = add_row(grid, row++, "LT threshold (%)", make_scale(shell, 0, 100, 5, "%"));
    g_c.rt_threshold = add_row(grid, row++, "RT threshold (%)", make_scale(shell, 0, 100, 5, "%"));

    static const char *SIDE[2] = {"left", "right"};
    for (int i = 0; i < 2; i++) {
        char label[48];
        snprintf(label, sizeof(label), "%s stick deadzone (%%)", SIDE[i]);
        g_c.deadzone[i] = add_row(grid, row++, label, make_scale(shell, 0, 40, 1, "%"));
        snprintf(label, sizeof(label), "%s stick range (%%)", SIDE[i]);
        g_c.range[i] = add_row(grid, row++, label, make_scale(shell, 45, 100, 1, "%"));
        gtk_widget_set_tooltip_text(g_c.range[i],
            "How far the stick has to go to count as fully pushed. Lower it if "
            "pushing all the way reads as a gentle push.");
        snprintf(label, sizeof(label), "%s stick diagonals (%%)", SIDE[i]);
        g_c.diagonal[i] = add_row(grid, row++, label, make_scale(shell, 45, 100, 1, "%"));
        gtk_widget_set_tooltip_text(g_c.diagonal[i],
            "The same, for the corners. A stick reaches less far diagonally than "
            "along an axis, and by how much differs from one stick to the next.");
    }

    /* --- this window only --- */
    make_page("this window", notebook, &grid);
    row = 0;
    g_c.muted = add_row(grid, row++, "speakers here", make_check(shell, "mute"));
    gtk_widget_set_tooltip_text(g_c.muted,
        "The speakers on this machine only. What the browser and the console "
        "receive is untouched.");
    g_c.volume = add_row(grid, row++, "volume (%)", make_scale(shell, 0, 100, 5, "%"));
    g_c.direct_sink = add_row(grid, row++, "output",
                              make_check(shell, "straight to the card, not the default"));
    gtk_widget_set_tooltip_text(g_c.direct_sink,
        "Play on the device named by LOCAL_SINK in the .env instead of whatever "
        "the system calls its default output.\n\nWorth turning on where the "
        "default is a virtual device belonging to a routing setup: sound that "
        "goes into one and never comes out the other side looks exactly like "
        "sound this program failed to produce. Does nothing if LOCAL_SINK is "
        "not set.");
    gtk_widget_set_tooltip_text(g_c.volume,
        "0 to 100, where about 13 is the source's own level and 100 is eight times it -- "
        "the same scale as the page and the console client.");
    g_c.brightness = add_row(grid, row++, "brightness (%)", make_scale(shell, 50, 150, 5, "%"));
    g_c.contrast = add_row(grid, row++, "contrast (%)", make_scale(shell, 50, 150, 5, "%"));
    gtk_widget_set_tooltip_text(g_c.brightness,
        "This window's picture only, done by the graphics card. Saturation and hue "
        "are not here: they would mean rewriting the captured frame, which is the "
        "same frame being sent to everyone else.");
    g_c.vsync = add_row(grid, row++, "drawing", make_check(shell, "wait for the display (vsync)"));

    /* --- console --- */
    make_page("console", notebook, &grid);
    row = 0;
    add_row(grid, row++, "power",
            make_button(shell, "wake the console", GTK_SHELL_ACTION_WAKE_CONSOLE,
                        "Runs the wake script and waits for the picture to come back "
                        "before re-enumerating the adapter."));
    add_row(grid, row++, "adapter",
            make_button(shell, "reset the adapter", GTK_SHELL_ACTION_RESET_DONGLE,
                        "Releases the gamepad adapter and re-runs its handshake with "
                        "the console. The same thing as unplugging and replugging it."));
    add_row(grid, row++, "program",
            make_button(shell, "restart the server", GTK_SHELL_ACTION_RESTART,
                        "Stops and starts this program, keeping its pid and its log. "
                        "The one thing that has always brought the sound back when it "
                        "stopped arriving."));

    g_c.status_label = gtk_label_new("");
    gtk_widget_set_halign(g_c.status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(g_c.status_label, 14);
    gtk_widget_set_margin_bottom(g_c.status_label, 10);
    gtk_box_pack_start(GTK_BOX(box), g_c.status_label, FALSE, FALSE, 0);

    set_window_icon(win);
    shell->settings_window = win;
    load_controls(shell);
}

/* --- the tray icon ---------------------------------------------------- */

static void on_menu_settings(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkShell *shell = user_data;
    load_controls(shell);
    gtk_widget_show_all(shell->settings_window);
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

static void on_icon_popup(GtkStatusIcon *icon, guint button, guint activate_time,
                          gpointer user_data) {
    GtkShell *shell = user_data;

    GtkWidget *menu = gtk_menu_new();

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
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    (void)icon;
    (void)button;
    (void)activate_time;
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
    if (shell->status_dirty) {
        shell->status_dirty = 0;
        char text[160];
        SDL_LockMutex(shell->lock);
        snprintf(text, sizeof(text), "%s", shell->status);
        SDL_UnlockMutex(shell->lock);
        gtk_label_set_text(GTK_LABEL(g_c.status_label), text);
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
        app_path(path, sizeof(path), "assets/icon-64.png");
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
