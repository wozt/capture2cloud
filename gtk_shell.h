#ifndef GTK_SHELL_H
#define GTK_SHELL_H

typedef struct GtkShell GtkShell;

typedef struct {
    /* "Stream to browser" checkbox toggled by the user. */
    void (*on_toggle_stream)(void *userdata, int enable);
    /* New port (1-65535) validated via the "Port..." dialog. */
    void (*on_set_port)(void *userdata, int port);
    /* "Quit" menu item or the control window being closed. */
    void (*on_quit)(void *userdata);
    void *userdata;
} GtkShellCallbacks;

/* Launches a small, independent GTK window ("Stream" menu bar) on its own
 * thread, with its own GTK loop. Has no link to the SDL video window: no
 * shared native handle, no embedding. Returns immediately, the window
 * appears asynchronously. */
/* video_xid: X11 id of the SDL video window (0 if unknown), to set a
 * WM_TRANSIENT_FOR hint telling the window manager the two windows belong
 * together (like a tool palette that follows its main window on
 * alt-tab). */
GtkShell *gtk_shell_start(int default_port, unsigned long video_xid, const GtkShellCallbacks *callbacks);

/* Syncs the checkbox and the control window's title with the web server's
 * actual state. Thread-safe, callable from any thread (including before
 * the GTK thread has finished building its menu, e.g. auto-start via the
 * config file). */
void gtk_shell_set_stream_status(GtkShell *shell, int enabled, int port);

/* Shows an error dialog. Thread-safe, callable from any thread. */
void gtk_shell_show_error(GtkShell *shell, const char *message);

/* Requests the GTK loop to stop (thread-safe, callable from any thread)
 * and waits for the thread to finish. */
void gtk_shell_stop(GtkShell *shell);

/* Repositions/resizes the control window so it stays docked right above
 * the video window (same width, same x, just above in y). video_height
 * is only used to remember the size so it can be restored after a
 * pseudo-fullscreen round trip (see gtk_shell_poll_layout). Thread-safe:
 * call on every move/resize of the video window. */
void gtk_shell_dock_above(GtkShell *shell, int video_x, int video_y, int video_width, int video_height);

/* Call on every main loop iteration (thread-safe). If the control window
 * requested a layout change (a simple move by the user, or toggling
 * pseudo-fullscreen via the maximize button), returns 1 and fills
 * video_x/video_y (position) and video_w/video_h (size, -1 if
 * unchanged); returns 0 otherwise. */
int gtk_shell_poll_layout(GtkShell *shell, int *video_x, int *video_y, int *video_w, int *video_h);

/* Enables/disables "always on top" for the control window. Thread-safe.
 * Enable it when the video window has focus, disable it as soon as it
 * loses focus, so it doesn't stay stuck on top of an unrelated
 * application once the user has switched away. */
void gtk_shell_set_keep_above(GtkShell *shell, int keep_above);

/* Minimizes/restores the control window -- to be used together with
 * minimizing/restoring the video window, so both behave like a single
 * window. Thread-safe. */
void gtk_shell_set_iconified(GtkShell *shell, int iconified);

/* Call on every main loop iteration (thread-safe). Returns 1 if the user
 * minimized/restored the control window itself (as opposed to the call
 * above), with *iconify set to 1 (minimize) or 0 (restore); returns 0
 * otherwise. */
int gtk_shell_poll_iconify(GtkShell *shell, int *iconify);

#endif
