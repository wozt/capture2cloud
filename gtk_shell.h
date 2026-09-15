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

/*
 * The client families, each with its own page in the settings window.
 *
 * They are separated because they are genuinely different servers with
 * different settings, not because it looks tidier: the browsers' size
 * and the console's are two encodes, the GamePad's is a third that no
 * ordinary decoder can read, and a Wii U console is a fourth on a port
 * of its own. One page each is the only arrangement in which "why did
 * the picture change" has one answer.
 */
typedef enum {
    GTK_SHELL_CLIENT_BROWSER = 0,
    GTK_SHELL_CLIENT_NATIVE,        /* Switch homebrew, Android app */
    GTK_SHELL_CLIENT_WIIU_PAD,      /* a real GamePad, over the radio */
    GTK_SHELL_CLIENT_WIIU_CONSOLE,  /* homebrew running on a Wii U */
    GTK_SHELL_CLIENT_COUNT
} GtkShellClient;

typedef enum {
    GTK_SHELL_ACTION_SHOW_CAPTURE,
    GTK_SHELL_ACTION_WAKE_CONSOLE,
    GTK_SHELL_ACTION_RESET_DONGLE,
    GTK_SHELL_ACTION_RESTART,
    /* The GamePad bridge, started and stopped by hand. It normally
     * starts itself with the host and waits for a pad, but a thing that
     * either works or does not, with no handle on it, is a thing you
     * cannot debug from the sofa. */
    GTK_SHELL_ACTION_WIIU_START,
    GTK_SHELL_ACTION_WIIU_STOP,
    /* The radio under it: the access point the pad associates to, and
     * the one gesture that makes an associated pad come back fresh. */
    GTK_SHELL_ACTION_WIIU_AP_START,
    GTK_SHELL_ACTION_WIIU_AP_STOP,
    GTK_SHELL_ACTION_WIIU_DEAUTH,
    /* The Wii U console client's chain, by hand, for the same reason the
     * pad's has buttons: a thing with no handle on it cannot be
     * debugged from the sofa. */
    GTK_SHELL_ACTION_WIIU_CONSOLE_KEYFRAME,
    GTK_SHELL_ACTION_QUIT
} GtkShellAction;

typedef struct {
    /* Something in the settings window changed. The whole set is passed,
     * not the one field: the caller applies what differs, which is one
     * place to look rather than twenty entry points. */
    void (*on_settings)(void *userdata, const AppSettings *settings);
    /* A button that does something once rather than setting a value. */
    void (*on_action)(void *userdata, GtkShellAction action);
    /*
     * Pair a GamePad, with the PIN the person chose.
     *
     * Its own callback rather than an action, because an action carries
     * nothing and this one has eight digits to hand over. They are
     * digits and not a secret: the pad shows the same four symbols on
     * its own screen and the whole exchange lasts about a minute.
     */
    void (*on_pair)(void *userdata, const char *pin);
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

/* What the GamePad bridge is doing, shown on its own line under the
 * setting that turns it on. Written from the host once a second. */
void gtk_shell_set_wiiu_status(GtkShell *shell, const char *text);

/* One line per client family, on that family's own page: how many are
 * connected and what they are being sent. Thread-safe, callable before
 * the window exists. */
void gtk_shell_set_client_status(GtkShell *shell, GtkShellClient client, const char *text);

/* Shows an error dialog. Thread-safe, callable from any thread. */
void gtk_shell_show_error(GtkShell *shell, const char *message);

/* Requests the GTK loop to stop (thread-safe) and waits for the thread
 * to finish. */
void gtk_shell_stop(GtkShell *shell);

#endif
