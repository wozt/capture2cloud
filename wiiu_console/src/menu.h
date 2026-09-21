#ifndef CAPTURE2WIIU_MENU_H
#define CAPTURE2WIIU_MENU_H

#include "settings.h"
#include "ui.h"

typedef enum {
    MENU_PAGE_CONNECTION,
    MENU_PAGE_CONSOLE
} MenuPage;

typedef enum {
    MENU_ACTION_NONE,
    MENU_ACTION_HOST,
    MENU_ACTION_NATIVE_PORT,
    MENU_ACTION_WEB_PORT,
    MENU_ACTION_PASSWORD,
    MENU_ACTION_CONNECT,
    MENU_ACTION_REMOTE_HOME,
    MENU_ACTION_WAKE,
    MENU_ACTION_RESET_DONGLE,
    MENU_ACTION_RESTART_HOST,
    MENU_ACTION_EXIT_HELP
} MenuAction;

typedef struct {
    unsigned rx_fps;
    unsigned decode_fps;
    unsigned display_fps;
    unsigned loop_fps;
    unsigned net_kbps;
    unsigned video_queue;
    unsigned video_dropped;
} MenuPerf;

typedef struct {
    MenuPage page;
    int open;
    unsigned capture_pending;
    unsigned capture_index;
} MenuState;

typedef struct {
    const Settings *settings;
    const char *password;
    const char *note;
    const char *decoder_why;
    int decoder_ok;
    int streaming;
    const MenuPerf *perf;
} MenuView;

void menu_init(MenuState *menu);
void menu_force_open(MenuState *menu, int open);
int menu_is_open(const MenuState *menu);

/* Handles one released touch and returns the requested application action. */
MenuAction menu_input(MenuState *menu,
                      const UiInput *input,
                      int streaming);

/* Draws either the menu or its small persistent corner marker. */
void menu_draw(const MenuState *menu,
               const MenuView *view);

/* First settings frame and first two stream-menu openings request a capture. */
int menu_take_capture(MenuState *menu,
                      unsigned *index);

#endif
