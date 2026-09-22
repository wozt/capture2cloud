#ifndef CAPTURE2WIIU_MENU_H
#define CAPTURE2WIIU_MENU_H

#include "settings.h"
#include "ui.h"

typedef enum {
    MENU_PAGE_CONNECTION,
    MENU_PAGE_STREAM,
    MENU_PAGE_CONTROLS,
    MENU_PAGE_BINDINGS,
    MENU_PAGE_INTERFACE,
    MENU_PAGE_CONSOLE
} MenuPage;

typedef enum {
    MENU_ACTION_NONE,
    MENU_ACTION_HOST,
    MENU_ACTION_NATIVE_PORT,
    MENU_ACTION_WEB_PORT,
    MENU_ACTION_PASSWORD,
    MENU_ACTION_CONNECT,
    MENU_ACTION_RESOLUTION,
    MENU_ACTION_FRAME_RATE,
    MENU_ACTION_BITRATE,
    MENU_ACTION_OUTPUT,
    MENU_ACTION_LEFT_DEADZONE,
    MENU_ACTION_LEFT_RANGE,
    MENU_ACTION_RIGHT_DEADZONE,
    MENU_ACTION_RIGHT_RANGE,
    MENU_ACTION_INVERT_Y,
    MENU_ACTION_BIND_SELECT,
    MENU_ACTION_BIND_RESET,
    MENU_ACTION_MARKER_CORNER,
    MENU_ACTION_MARKER_COLOUR,
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
    unsigned resolution_index;
    unsigned fps_index;
    unsigned bitrate_index;
    int profile_dirty;
    int binding_target_slot;
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
                      int streaming,
                      const Settings *settings);

/* Draws either the menu or its small persistent corner marker. */
void menu_draw(const MenuState *menu,
               const MenuView *view);

void menu_adopt_stream(MenuState *menu,
                       unsigned width,
                       unsigned height,
                       unsigned fps,
                       unsigned bitrate_kbps);

int menu_change_stream(MenuState *menu,
                       MenuAction action,
                       unsigned *width,
                       unsigned *height,
                       unsigned *fps,
                       unsigned *bitrate_kbps);

int menu_take_profile_dirty(MenuState *menu,
                            unsigned *width,
                            unsigned *height,
                            unsigned *fps,
                            unsigned *bitrate_kbps);

int  menu_binding_target(const MenuState *menu);
void menu_binding_finish(MenuState *menu);

#endif
