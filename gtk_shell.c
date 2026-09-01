#include "gtk_shell.h"

#include "app_config.h"

#include <SDL2/SDL.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

struct GtkShell {
    SDL_Thread *thread;
    GtkWidget *window;
    GtkWidget *menubar;
    GtkWidget *toggle_item;
    GtkShellCallbacks callbacks;
    int current_port;
    unsigned long video_xid; /* XID of the SDL video window, for the
                               * "transient for" hint (WM_TRANSIENT_FOR)
                               * that tells the WM the two windows belong
                               * together. */

    int bar_height; /* intrinsic height of the menu bar, measured once
                      * after it is shown -- never recomputed from the
                      * current allocation, to avoid racing with an
                      * in-progress resize. */

    /* Last position/width WE imposed (pseudo-fullscreen toggle,
     * restoration...) -- pure bookkeeping, no longer used to detect a
     * programmatic move (see ignore_next_configure). */
    int last_docked_x, last_docked_y, last_docked_width;

    /* Number of upcoming "configure-event"s to ignore because they result
     * from a gtk_window_move/resize we did ourselves. A counter rather
     * than a coordinate comparison: the WM/decoration can slightly shift
     * the reported position compared to the one requested, which would
     * make an exact comparison fail. */
    int ignore_next_configure;
    int last_seen_configure_x, last_seen_configure_y; /* for the anti-drift
                                                          * dead zone below */

    /* GTK -> main thread communication: request to reposition (and
     * sometimes resize) the video window. w/h set to -1 means "don't
     * touch the size". */
    SDL_mutex *pos_mutex;
    int layout_pending;
    int layout_x, layout_y, layout_w, layout_h;

    /* "Fake fullscreen" triggered by the bar's maximize button: toggles
     * to/from a layout of full-width bar on top + video over the rest of
     * the screen. */
    int pseudo_maximized;
    int saved_video_x, saved_video_y, saved_video_w, saved_video_h;
    int last_video_height; /* last known video height, so it can be
                             * restored after a toggle round trip. */

    /* Minimize/restore, in both directions. last_set_iconified remembers
     * the last state WE imposed, to distinguish -- like
     * ignore_next_configure -- a programmatic state change from a real
     * user click on the bar's minimize button. */
    int last_set_iconified;
    int iconify_pending;
    int iconify_value;

    /* Main thread -> GTK communication: most recent docking target, with
     * at most a single pending GTK idle at a time (a closely-spaced
     * update replaces the previous one instead of stacking behind it, so
     * a stale position is never displayed). */
    SDL_mutex *dock_mutex;
    int dock_idle_scheduled;
    int dock_x, dock_y, dock_width;
};

static void on_toggle_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    GtkShell *shell = user_data;
    if (shell->callbacks.on_toggle_stream) {
        shell->callbacks.on_toggle_stream(shell->callbacks.userdata, gtk_check_menu_item_get_active(item));
    }
}

static void on_port_activate(GtkMenuItem *unused_item, gpointer user_data) {
    (void)unused_item;
    GtkShell *shell = user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Web server port", GTK_WINDOW(shell->window),
                                                     GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_OK",
                                                     GTK_RESPONSE_OK, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *label = gtk_label_new("Port (1-65535):");
    GtkAdjustment *adj = gtk_adjustment_new(shell->current_port, 1, 65535, 1, 100, 0);
    GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spin, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(content), box);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
        shell->current_port = port;
        if (shell->callbacks.on_set_port) {
            shell->callbacks.on_set_port(shell->callbacks.userdata, port);
        }
    }

    gtk_widget_destroy(dialog);
}

/* Runs scripts/wake_console.sh asynchronously --
 * g_spawn_async so the GTK main loop is never blocked, since the script
 * can take several seconds (it polls Home Assistant until the plug
 * confirms each state change). Manually triggered only, never automatic
 * on stream start/stop, so it never power-cycles the console just
 * because the user wanted to check the video feed. */
static void on_wake_console_activate(GtkMenuItem *unused_item, gpointer user_data) {
    (void)unused_item;
    GtkShell *shell = user_data;

    char path[PATH_MAX];
    app_path(path, sizeof(path), "scripts/wake_console.sh");

    char *argv[] = {"/bin/bash", path, NULL};
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL,
                       &error)) {
        char message[PATH_MAX + 256];
        snprintf(message, sizeof(message), "Wake console: failed to launch %s (%s)", path,
                 error ? error->message : "unknown error");
        gtk_shell_show_error(shell, message);
        if (error) g_error_free(error);
    }
}

static void do_quit(GtkShell *shell) {
    if (shell->callbacks.on_quit) {
        shell->callbacks.on_quit(shell->callbacks.userdata);
    }
    gtk_main_quit();
}

static void on_quit_activate(GtkMenuItem *unused_item, gpointer user_data) {
    (void)unused_item;
    do_quit((GtkShell *)user_data);
}

static gboolean on_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget;
    (void)event;
    do_quit((GtkShell *)user_data);
    return FALSE; /* let GTK destroy the window normally */
}

static gboolean on_configure_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget;
    GtkShell *shell = user_data;
    GdkEventConfigure *cfg = (GdkEventConfigure *)event;

    if (shell->ignore_next_configure > 0) {
        shell->ignore_next_configure--;
        shell->last_seen_configure_x = cfg->x;
        shell->last_seen_configure_y = cfg->y;
        return FALSE; /* result of one of our own move/resize calls, ignore it */
    }

    /* Dead zone: the WM/decoration can report a position that drifts by a
     * few pixels from one call to the next even without any user action
     * (decoration rounding). Without this filter, every micro-drift would
     * be taken for a real move, forwarded to the video window, which
     * would in turn forward it back to us -- an endless drift in small
     * steps. A real window drag moves by far more than a few pixels from
     * one event to the next. */
    int dx = cfg->x - shell->last_seen_configure_x;
    int dy = cfg->y - shell->last_seen_configure_y;
    shell->last_seen_configure_x = cfg->x;
    shell->last_seen_configure_y = cfg->y;
    if (dx > -5 && dx < 5 && dy > -5 && dy < 5) {
        return FALSE;
    }

    /* The user (or the WM) moved the control window independently: ask
     * the video window to dock right below it (without touching its
     * size). */
    SDL_LockMutex(shell->pos_mutex);
    shell->layout_pending = 1;
    shell->layout_x = cfg->x;
    shell->layout_y = cfg->y + shell->bar_height;
    shell->layout_w = -1;
    shell->layout_h = -1;
    SDL_UnlockMutex(shell->pos_mutex);

    return FALSE;
}

/* The "maximize" button must not trigger the WM's real maximize (which
 * would stretch the bar over the whole screen height): we intercept it
 * and give it our own meaning -- toggle to a "full-width bar on top +
 * video over the rest of the screen below" layout, and a second click
 * restores the previous layout. Since we always cancel the real
 * maximize, the WM reports every click to us as a new transition to the
 * "maximized" state, never as a restore -- it's our own pseudo_maximized
 * boolean that drives the alternation. */
static gboolean on_window_state_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    GtkShell *shell = user_data;
    GdkEventWindowState *wse = (GdkEventWindowState *)event;

    if (wse->changed_mask & GDK_WINDOW_STATE_ICONIFIED) {
        int now_iconified = (wse->new_window_state & GDK_WINDOW_STATE_ICONIFIED) ? 1 : 0;
        if (now_iconified != shell->last_set_iconified) {
            /* The user minimized/restored the bar themselves (not a call
             * to gtk_shell_set_iconified): ask the video window to do the
             * same. */
            shell->last_set_iconified = now_iconified;
            SDL_LockMutex(shell->pos_mutex);
            shell->iconify_pending = 1;
            shell->iconify_value = now_iconified;
            SDL_UnlockMutex(shell->pos_mutex);
        }
    }

    if (!((wse->changed_mask & GDK_WINDOW_STATE_MAXIMIZED) && (wse->new_window_state & GDK_WINDOW_STATE_MAXIMIZED))) {
        return FALSE;
    }
    gtk_window_unmaximize(GTK_WINDOW(widget));

    int bar_height = shell->bar_height > 0 ? shell->bar_height : 32;

    if (!shell->pseudo_maximized) {
        /* Save the current layout so we can return to it. */
        shell->saved_video_x = shell->last_docked_x;
        shell->saved_video_y = shell->last_docked_y + bar_height;
        shell->saved_video_w = shell->last_docked_width;
        shell->saved_video_h = shell->last_video_height;

        GdkDisplay *display = gtk_widget_get_display(widget);
        GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(widget));
        GdkRectangle geom;
        gdk_monitor_get_workarea(monitor, &geom);

        shell->last_docked_x = geom.x;
        shell->last_docked_y = geom.y;
        shell->last_docked_width = geom.width;
        shell->ignore_next_configure += 2;
        gtk_window_move(GTK_WINDOW(widget), geom.x, geom.y);
        gtk_window_resize(GTK_WINDOW(widget), geom.width, bar_height);

        SDL_LockMutex(shell->pos_mutex);
        shell->layout_pending = 1;
        shell->layout_x = geom.x;
        shell->layout_y = geom.y + bar_height;
        shell->layout_w = geom.width;
        shell->layout_h = geom.height - bar_height;
        SDL_UnlockMutex(shell->pos_mutex);

        shell->pseudo_maximized = 1;
    } else {
        shell->last_docked_x = shell->saved_video_x;
        shell->last_docked_y = shell->saved_video_y - bar_height;
        shell->last_docked_width = shell->saved_video_w;
        shell->ignore_next_configure += 2;
        gtk_window_move(GTK_WINDOW(widget), shell->last_docked_x, shell->last_docked_y);
        gtk_window_resize(GTK_WINDOW(widget), shell->saved_video_w, bar_height);

        SDL_LockMutex(shell->pos_mutex);
        shell->layout_pending = 1;
        shell->layout_x = shell->saved_video_x;
        shell->layout_y = shell->saved_video_y;
        shell->layout_w = shell->saved_video_w;
        shell->layout_h = shell->saved_video_h;
        SDL_UnlockMutex(shell->pos_mutex);

        shell->pseudo_maximized = 0;
    }

    return FALSE;
}

static int gtk_thread_main(void *arg) {
    GtkShell *shell = arg;

    int argc = 0;
    gtk_init(&argc, NULL);

    shell->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(shell->window), "Capture2Cloud — control");
    gtk_window_set_default_size(GTK_WINDOW(shell->window), 260, -1);
    /* Normal decoration (title bar with fully functional
     * minimize/maximize/close): only the video window is borderless. */
    /* "Utility" window, absent from the taskbar/alt-tab list: it only
     * makes sense docked to the video window, not as a window in its own
     * right. */
    gtk_window_set_type_hint(GTK_WINDOW(shell->window), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(shell->window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(shell->window), TRUE);
    /* "Always on top" is only enabled while the video window has focus
     * (see gtk_shell_set_keep_above): this avoids staying stuck on top of
     * an unrelated app once the user has switched away. */
    g_signal_connect(shell->window, "delete-event", G_CALLBACK(on_delete_event), shell);
    g_signal_connect(shell->window, "configure-event", G_CALLBACK(on_configure_event), shell);
    g_signal_connect(shell->window, "window-state-event", G_CALLBACK(on_window_state_event), shell);

    shell->menubar = gtk_menu_bar_new();
    GtkWidget *stream_menu = gtk_menu_new();
    GtkWidget *stream_menu_item = gtk_menu_item_new_with_mnemonic("_Stream");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(stream_menu_item), stream_menu);

    shell->toggle_item = gtk_check_menu_item_new_with_label("Stream to browser");
    GtkWidget *port_item = gtk_menu_item_new_with_label("Port...");
    GtkWidget *wake_item = gtk_menu_item_new_with_label("Wake console");
    GtkWidget *sep_item = gtk_separator_menu_item_new();
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");

    gtk_menu_shell_append(GTK_MENU_SHELL(stream_menu), shell->toggle_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(stream_menu), port_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(stream_menu), wake_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(stream_menu), sep_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(stream_menu), quit_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(shell->menubar), stream_menu_item);

    g_signal_connect(shell->toggle_item, "toggled", G_CALLBACK(on_toggle_toggled), shell);
    g_signal_connect(port_item, "activate", G_CALLBACK(on_port_activate), shell);
    g_signal_connect(wake_item, "activate", G_CALLBACK(on_wake_console_activate), shell);
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_activate), shell);

    gtk_container_add(GTK_CONTAINER(shell->window), shell->menubar);

    gtk_widget_show_all(shell->window);
    /* Let the WM apply decoration and map the window before measuring its
     * actual size / setting the hint. */
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    GdkWindow *gdkwin = gtk_widget_get_window(shell->window);
    if (gdkwin) {
        if (shell->video_xid != 0) {
            XSetTransientForHint(GDK_WINDOW_XDISPLAY(gdkwin), GDK_WINDOW_XID(gdkwin), (Window)shell->video_xid);
        }

        GdkRectangle frame;
        gdk_window_get_frame_extents(gdkwin, &frame);
        shell->bar_height = frame.height > 0 ? frame.height : 0;
    }
    if (shell->bar_height <= 0) {
        GtkRequisition req;
        gtk_widget_get_preferred_size(shell->menubar, NULL, &req);
        shell->bar_height = req.height > 0 ? req.height : 32;
    }

    gtk_main();
    return 0;
}

GtkShell *gtk_shell_start(int default_port, unsigned long video_xid, const GtkShellCallbacks *callbacks) {
    GtkShell *shell = calloc(1, sizeof(*shell));
    if (!shell) {
        return NULL;
    }
    shell->callbacks = *callbacks;
    shell->current_port = default_port;
    shell->video_xid = video_xid;
    shell->last_docked_x = INT_MIN;
    shell->last_docked_y = INT_MIN;
    shell->pos_mutex = SDL_CreateMutex();
    shell->dock_mutex = SDL_CreateMutex();
    if (!shell->pos_mutex || !shell->dock_mutex) {
        if (shell->pos_mutex) SDL_DestroyMutex(shell->pos_mutex);
        if (shell->dock_mutex) SDL_DestroyMutex(shell->dock_mutex);
        free(shell);
        return NULL;
    }

    shell->thread = SDL_CreateThread(gtk_thread_main, "gtk-shell", shell);
    if (!shell->thread) {
        fprintf(stderr, "gtk_shell_start: SDL_CreateThread failed\n");
        SDL_DestroyMutex(shell->pos_mutex);
        SDL_DestroyMutex(shell->dock_mutex);
        free(shell);
        return NULL;
    }
    return shell;
}

struct status_ctx {
    GtkShell *shell;
    int enabled;
    int port;
};

static gboolean set_stream_status_idle_cb(gpointer data) {
    struct status_ctx *ctx = data;
    GtkShell *shell = ctx->shell;
    g_signal_handlers_block_by_func(shell->toggle_item, G_CALLBACK(on_toggle_toggled), shell);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(shell->toggle_item), ctx->enabled);
    g_signal_handlers_unblock_by_func(shell->toggle_item, G_CALLBACK(on_toggle_toggled), shell);
    shell->current_port = ctx->port;
    free(ctx);
    return G_SOURCE_REMOVE;
}

void gtk_shell_set_stream_status(GtkShell *shell, int enabled, int port) {
    struct status_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        return;
    }
    ctx->shell = shell;
    ctx->enabled = enabled;
    ctx->port = port;
    /* Thread-safe via g_idle_add: callable from any thread (e.g.
     * automatic startup via the config file, even before the GTK thread
     * has finished building its menu -- the idle will only run once
     * gtk_main() has started, menu already built). */
    g_idle_add(set_stream_status_idle_cb, ctx);
}

struct error_ctx {
    GtkShell *shell;
    char *message;
};

static gboolean show_error_idle_cb(gpointer data) {
    struct error_ctx *ctx = data;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(ctx->shell->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_OK, "%s", ctx->message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    free(ctx->message);
    free(ctx);
    return G_SOURCE_REMOVE;
}

void gtk_shell_show_error(GtkShell *shell, const char *message) {
    struct error_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        return;
    }
    ctx->shell = shell;
    ctx->message = strdup(message);
    /* Thread-safe via g_idle_add, for the same reasons as
     * gtk_shell_set_stream_status above. */
    g_idle_add(show_error_idle_cb, ctx);
}

static gboolean dock_idle_cb(gpointer data) {
    GtkShell *shell = data;

    SDL_LockMutex(shell->dock_mutex);
    int x = shell->dock_x, video_y = shell->dock_y, width = shell->dock_width;
    shell->dock_idle_scheduled = 0;
    SDL_UnlockMutex(shell->dock_mutex);

    int bar_height = shell->bar_height > 0 ? shell->bar_height : 32;
    int y = video_y - bar_height;

    shell->last_docked_x = x;
    shell->last_docked_y = y;
    shell->last_docked_width = width;

    shell->ignore_next_configure += 2;
    gtk_window_resize(GTK_WINDOW(shell->window), width, bar_height);
    gtk_window_move(GTK_WINDOW(shell->window), x, y);

    return G_SOURCE_REMOVE;
}

void gtk_shell_dock_above(GtkShell *shell, int video_x, int video_y, int video_width, int video_height) {
    if (!shell) {
        return;
    }
    shell->last_video_height = video_height;

    SDL_LockMutex(shell->dock_mutex);
    shell->dock_x = video_x;
    shell->dock_y = video_y;
    shell->dock_width = video_width;
    int need_schedule = !shell->dock_idle_scheduled;
    shell->dock_idle_scheduled = 1;
    SDL_UnlockMutex(shell->dock_mutex);

    /* Only one idle at a time: if a dock_idle_cb is already pending, it
     * will read the freshest position anyway by the time it runs --
     * scheduling one per move event would be pointless (and a source of
     * delay). */
    if (need_schedule) {
        g_idle_add(dock_idle_cb, shell);
    }
}

int gtk_shell_poll_layout(GtkShell *shell, int *video_x, int *video_y, int *video_w, int *video_h) {
    if (!shell) {
        return 0;
    }
    SDL_LockMutex(shell->pos_mutex);
    int pending = shell->layout_pending;
    if (pending) {
        *video_x = shell->layout_x;
        *video_y = shell->layout_y;
        *video_w = shell->layout_w;
        *video_h = shell->layout_h;
        shell->layout_pending = 0;
    }
    SDL_UnlockMutex(shell->pos_mutex);
    return pending;
}

struct bool_ctx {
    GtkShell *shell;
    int value;
};

static gboolean keep_above_idle_cb(gpointer data) {
    struct bool_ctx *ctx = data;
    if (ctx->shell->window) {
        gtk_window_set_keep_above(GTK_WINDOW(ctx->shell->window), ctx->value);
    }
    free(ctx);
    return G_SOURCE_REMOVE;
}

void gtk_shell_set_keep_above(GtkShell *shell, int keep_above) {
    if (!shell) {
        return;
    }
    struct bool_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        return;
    }
    ctx->shell = shell;
    ctx->value = keep_above;
    g_idle_add(keep_above_idle_cb, ctx);
}

static gboolean iconify_idle_cb(gpointer data) {
    struct bool_ctx *ctx = data;
    GtkShell *shell = ctx->shell;
    if (shell->window) {
        /* Marked as self-imposed BEFORE acting, so the resulting
         * "window-state-event" is recognized as our own in
         * on_window_state_event and doesn't get forwarded back to the
         * video window. */
        shell->last_set_iconified = ctx->value;
        if (ctx->value) {
            gtk_window_iconify(GTK_WINDOW(shell->window));
        } else {
            gtk_window_deiconify(GTK_WINDOW(shell->window));
            gtk_widget_show(shell->window);
        }
    }
    free(ctx);
    return G_SOURCE_REMOVE;
}

void gtk_shell_set_iconified(GtkShell *shell, int iconified) {
    if (!shell) {
        return;
    }
    struct bool_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        return;
    }
    ctx->shell = shell;
    ctx->value = iconified;
    g_idle_add(iconify_idle_cb, ctx);
}

int gtk_shell_poll_iconify(GtkShell *shell, int *iconify) {
    if (!shell) {
        return 0;
    }
    SDL_LockMutex(shell->pos_mutex);
    int pending = shell->iconify_pending;
    if (pending) {
        *iconify = shell->iconify_value;
        shell->iconify_pending = 0;
    }
    SDL_UnlockMutex(shell->pos_mutex);
    return pending;
}

struct stop_ctx {
    GtkShell *shell;
};

static gboolean stop_idle_cb(gpointer data) {
    struct stop_ctx *ctx = data;
    if (ctx->shell->window) {
        gtk_widget_destroy(ctx->shell->window);
    }
    gtk_main_quit();
    free(ctx);
    return G_SOURCE_REMOVE;
}

void gtk_shell_stop(GtkShell *shell) {
    if (!shell) {
        return;
    }
    /* g_idle_add is documented as callable from any thread: it's the safe
     * way to run code on the GTK thread from the outside. */
    struct stop_ctx *ctx = malloc(sizeof(*ctx));
    ctx->shell = shell;
    g_idle_add(stop_idle_cb, ctx);

    if (shell->thread) {
        SDL_WaitThread(shell->thread, NULL);
    }
    SDL_DestroyMutex(shell->pos_mutex);
    SDL_DestroyMutex(shell->dock_mutex);
    free(shell);
}
