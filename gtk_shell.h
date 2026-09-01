#ifndef GTK_SHELL_H
#define GTK_SHELL_H

#include "app_settings.h"

/*
 * The local interface: an icon in the notification area, and a settings
 * window behind it.
 *
 * It used to be a menu bar that chased the video window around the
 * screen -- docking above it, following its moves, mirroring its
 * minimise, faking a fullscreen by moving both. That was a great deal of
 * machinery to imitate one window out of two, and it fought the window
 * manager the whole way. The video window is now an ordinary window with
 * its own decorations, and everything else lives in the tray, where a
 * capture program that runs for hours belongs.
 */

typedef struct GtkShell GtkShell;

typedef enum {
    GTK_SHELL_ACTION_WAKE_CONSOLE,
    GTK_SHELL_ACTION_RESET_DONGLE,
    GTK_SHELL_ACTION_RESTART,
    GTK_SHELL_ACTION_QUIT
} GtkShellAction;

typedef struct {
    /* Something in the settings window changed. The whole set is passed,
     * not the one field: the caller applies what differs, which is one
     * place to look rather than twenty entry points. */
    void (*on_settings)(void *userdata, const AppSettings *settings);
    /* A button that does something once rather than setting a value. */
    void (*on_action)(void *userdata, GtkShellAction action);
    void *userdata;
} GtkShellCallbacks;

/* Starts the tray icon and its GTK loop on its own thread. Returns
 * immediately; the icon appears when the loop gets to it. `settings` is
 * the state the controls open on. */
GtkShell *gtk_shell_start(const AppSettings *settings, const GtkShellCallbacks *callbacks);

/* Tells the interface what actually happened, which is not always what
 * was asked for -- a port can be refused, a capture format can be
 * unavailable. Thread-safe, callable before the window exists. */
void gtk_shell_update(GtkShell *shell, const AppSettings *settings);

/* The controllers the program can see, for the settings window's list.
 * Thread-safe and callable before the window exists: the names are
 * copied, and the list is rebuilt on the GTK thread when it next looks.
 *
 * The program supplies them rather than the interface asking, because
 * enumerating them is SDL's business and SDL's joystick calls belong on
 * the thread that initialised it. */
void gtk_shell_set_controllers(GtkShell *shell, const char *const *names, int count);

/* A line under the tray icon's tooltip and in the settings window:
 * whether the stream is up, how many are watching. Thread-safe. */
void gtk_shell_set_status(GtkShell *shell, const char *text);

/* Shows an error dialog. Thread-safe, callable from any thread. */
void gtk_shell_show_error(GtkShell *shell, const char *message);

/* Requests the GTK loop to stop (thread-safe) and waits for the thread
 * to finish. */
void gtk_shell_stop(GtkShell *shell);

#endif
