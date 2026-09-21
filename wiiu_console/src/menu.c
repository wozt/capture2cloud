#include "menu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "gx2_video.h"
#include "input.h"
#include "net.h"
#include "video.h"

typedef struct {
    int x, y, w, h;
} MenuRect;

typedef struct {
    MenuRect rect;
    UiColour fill;
    UiColour border;
} MenuBox;

typedef struct {
    int x, y, w, h;
    int size;
    int centred;
    UiColour colour;
    char text[96];
} MenuText;

#define MENU_BOX_CAP  40
#define MENU_TEXT_CAP 64

typedef struct {
    MenuBox boxes[MENU_BOX_CAP];
    MenuText texts[MENU_TEXT_CAP];
    unsigned box_count;
    unsigned text_count;
} MenuCanvas;

static const UiColour MENU_SCRIM = { 0x08, 0x0c, 0x12, 0xe8 };
static const UiColour MENU_CARD = { 0x18, 0x21, 0x2c, 0xf6 };
static const UiColour MENU_CARD_HOVER = { 0x22, 0x30, 0x3f, 0xf8 };
static const UiColour MENU_GREEN = { 0x39, 0x9a, 0x68, 0xff };
static const UiColour MENU_RED = { 0x9a, 0x43, 0x43, 0xff };

static const MenuRect R_MARKER = { 1238, 10, 32, 32 };
static const MenuRect R_MENU = { 64, 38, 1152, 644 };
static const MenuRect R_SIDEBAR = { 84, 92, 218, 548 };
static const MenuRect R_CONNECTION = { 100, 184, 186, 54 };
static const MenuRect R_CONSOLE = { 100, 250, 186, 54 };

static const MenuRect R_HOST = { 348, 148, 520, 60 };
static const MenuRect R_PASSWORD = { 348, 220, 520, 60 };
static const MenuRect R_PORT = { 884, 148, 286, 60 };
static const MenuRect R_WEB_PORT = { 884, 220, 286, 60 };
static const MenuRect R_CONNECT = { 348, 310, 390, 58 };
static const MenuRect R_EXIT = { 756, 310, 414, 58 };

static const MenuRect R_WAKE = { 348, 148, 390, 64 };
static const MenuRect R_REMOTE_HOME = { 756, 148, 414, 64 };
static const MenuRect R_RESET = { 348, 230, 390, 64 };
static const MenuRect R_RESTART = { 756, 230, 414, 64 };
static const MenuRect R_CONSOLE_EXIT = { 348, 326, 822, 58 };

static int hit(const MenuRect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w &&
           y >= r->y && y < r->y + r->h;
}

static void box(MenuCanvas *canvas,
                MenuRect rect,
                UiColour fill,
                UiColour border)
{
    if (canvas->box_count >= MENU_BOX_CAP) {
        return;
    }

    canvas->boxes[canvas->box_count++] =
        (MenuBox){ rect, fill, border };
}

static void text_at(MenuCanvas *canvas,
                    int x,
                    int y,
                    int size,
                    UiColour colour,
                    const char *fmt,
                    ...)
{
    if (canvas->text_count >= MENU_TEXT_CAP) {
        return;
    }

    MenuText *text =
        &canvas->texts[canvas->text_count++];

    memset(text, 0, sizeof(*text));
    text->x = x;
    text->y = y;
    text->size = size;
    text->colour = colour;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text->text, sizeof(text->text), fmt, ap);
    va_end(ap);
}

static void text_centred(MenuCanvas *canvas,
                         MenuRect rect,
                         int size,
                         UiColour colour,
                         const char *label)
{
    if (canvas->text_count >= MENU_TEXT_CAP) {
        return;
    }

    MenuText *text =
        &canvas->texts[canvas->text_count++];

    memset(text, 0, sizeof(*text));
    text->x = rect.x;
    text->y = rect.y;
    text->w = rect.w;
    text->h = rect.h;
    text->size = size;
    text->centred = 1;
    text->colour = colour;
    snprintf(text->text, sizeof(text->text), "%s", label);
}

static void button(MenuCanvas *canvas,
                   MenuRect rect,
                   const char *label,
                   UiColour fill,
                   UiColour colour)
{
    box(canvas, rect, fill, UI_DIM);
    text_centred(canvas, rect, UI_SIZE_BODY, colour, label);
}

static void field(MenuCanvas *canvas,
                  MenuRect rect,
                  const char *label,
                  const char *value,
                  UiColour value_colour)
{
    box(canvas, rect, MENU_CARD, UI_DIM);
    text_at(canvas, rect.x + 18, rect.y + 8,
            UI_SIZE_BODY, UI_DIM, "%s", label);
    text_at(canvas, rect.x + 190, rect.y + 8,
            UI_SIZE_BODY, value_colour, "%s", value);
}

/*
 * The Wii U SDL renderer has already shown that mixed colour and texture
 * commands can lose their intended order after the raw GX2 video pass.
 * Draw every rectangle first, execute it, then draw every text texture.
 * No later background can therefore cover an earlier label.
 */
static void render_canvas(const MenuCanvas *canvas)
{
    for (unsigned i = 0; i < canvas->box_count; ++i) {
        const MenuBox *b = &canvas->boxes[i];
        ui_box(b->rect.x, b->rect.y,
               b->rect.w, b->rect.h,
               b->fill, b->border);
    }

    ui_flush();

    for (unsigned i = 0; i < canvas->text_count; ++i) {
        const MenuText *t = &canvas->texts[i];

        if (t->centred) {
            ui_text_centred(t->x, t->y, t->w, t->h,
                            t->size, t->colour, t->text);
        } else {
            ui_text(t->x, t->y, t->size,
                    t->colour, "%s", t->text);
        }
    }

    ui_flush();
}

static void add_shell(MenuCanvas *canvas,
                      const MenuState *menu,
                      const MenuView *view)
{
    const NetInfo *net = net_info();
    const int control =
        net->state == NET_CONNECTED && net->may_control;

    box(canvas, R_MENU, MENU_SCRIM, UI_DIM);
    box(canvas, R_SIDEBAR, MENU_CARD, UI_PANEL);

    text_at(canvas, 100, 64, UI_SIZE_BODY,
            UI_TEXT, "Capture2Cloud");
    text_at(canvas, 100, 116, UI_SIZE_BODY,
            control ? MENU_GREEN : UI_DIM,
            "%s", control ? "CONTROL" : "VIEWER");

    button(canvas, R_CONNECTION, "Connection",
           menu->page == MENU_PAGE_CONNECTION
               ? UI_ACCENT : UI_PANEL,
           UI_TEXT);
    button(canvas, R_CONSOLE, "Console",
           menu->page == MENU_PAGE_CONSOLE
               ? UI_ACCENT : UI_PANEL,
           UI_TEXT);

    text_at(canvas, 100, 566, UI_SIZE_BODY, UI_DIM,
            "%s", view->streaming
                ? "Touch the corner to close"
                : "HOME opens the Wii U menu");
    text_at(canvas, 100, 600, UI_SIZE_BODY, UI_DIM,
            "%s",
            net->state == NET_CONNECTED ? "Connected" :
            net->state == NET_CONNECTING ? "Connecting" :
            net->state == NET_FAILED ? "Retrying" : "Offline");
}

static void add_connection(MenuCanvas *canvas,
                           const MenuView *view)
{
    char host[32];
    char native_port[16];
    char web_port[16];

    settings_host_string(view->settings, host, sizeof(host));
    snprintf(native_port, sizeof(native_port), "%u", view->settings->port);
    snprintf(web_port, sizeof(web_port), "%u", view->settings->web_port);

    const char *password = "Tap to enter";
    UiColour password_colour = UI_DIM;

    if (view->password && view->password[0]) {
        password = "********";
        password_colour = UI_TEXT;
    } else if (view->settings->token[0]) {
        password = "Session token saved";
        password_colour = MENU_GREEN;
    }

    text_at(canvas, 348, 74, UI_SIZE_TITLE,
            UI_TEXT, "Connection");
    text_at(canvas, 348, 112, UI_SIZE_BODY,
            UI_DIM, "Host and authentication");

    field(canvas, R_HOST, "Host", host, UI_TEXT);
    field(canvas, R_PASSWORD, "Password", password, password_colour);
    field(canvas, R_PORT, "Stream port", native_port, UI_TEXT);
    field(canvas, R_WEB_PORT, "Login port", web_port, UI_TEXT);

    button(canvas, R_CONNECT, "CONNECT / RECONNECT",
           UI_ACCENT, UI_TEXT);
    button(canvas, R_EXIT, "Wii U HOME menu -> Quitter",
           UI_PANEL, UI_TEXT);

    if (!view->decoder_ok) {
        text_at(canvas, 348, 400, UI_SIZE_BODY, MENU_RED,
                "Decoder: %s", view->decoder_why);
    } else if (view->note && view->note[0]) {
        text_at(canvas, 348, 400, UI_SIZE_BODY, MENU_RED,
                "%s", view->note);
    }

    text_at(canvas, 348, 438, UI_SIZE_BODY, UI_DIM,
            "%s", net_info()->status);
}

static void add_console(MenuCanvas *canvas,
                        const MenuView *view)
{
    const NetInfo *net = net_info();
    const int enabled =
        net->state == NET_CONNECTED && net->may_control;

    const UiColour active = enabled ? UI_ACCENT : UI_PANEL;
    const UiColour danger = enabled ? UI_DANGER : UI_PANEL;
    const UiColour text_colour = enabled ? UI_TEXT : UI_DIM;

    text_at(canvas, 348, 74, UI_SIZE_TITLE,
            UI_TEXT, "Remote console");
    text_at(canvas, 348, 112, UI_SIZE_BODY,
            UI_DIM, "Commands are enabled only for CONTROL sessions");

    button(canvas, R_WAKE, "WAKE", active, text_colour);
    button(canvas, R_REMOTE_HOME, "REMOTE HOME", active, text_colour);
    button(canvas, R_RESET, "RESET ADAPTER", danger, text_colour);
    button(canvas, R_RESTART, "RESTART HOST", danger, text_colour);
    button(canvas, R_CONSOLE_EXIT, "Wii U HOME menu -> Quitter",
           UI_PANEL, UI_TEXT);

    if (view->note && view->note[0]) {
        text_at(canvas, 348, 414, UI_SIZE_BODY,
                MENU_RED, "%s", view->note);
    } else {
        text_at(canvas, 348, 414, UI_SIZE_BODY,
                enabled ? MENU_GREEN : UI_DIM,
                "%s", enabled
                    ? "Remote commands ready"
                    : "Connect and authenticate to enable these commands");
    }
}

static void add_diagnostics(MenuCanvas *canvas,
                            const MenuView *view)
{
    if (!view->streaming || !view->perf) {
        return;
    }

    const NetInfo *net = net_info();
    const MenuPerf *perf = view->perf;

    VideoStats video;
    Gx2VideoStats gx2;
    video_stats_ex(&video);
    gx2_video_stats(&gx2);

    box(canvas, (MenuRect){ 348, 472, 822, 154 },
        MENU_CARD_HOVER, UI_PANEL);

    text_at(canvas, 368, 486, UI_SIZE_BODY, UI_TEXT,
            "%s  %ux%u  %u.%u Mb/s",
            ui_video_renderer_name(), net->width, net->height,
            perf->net_kbps / 1000,
            (perf->net_kbps % 1000) / 100);
    text_at(canvas, 368, 522, UI_SIZE_BODY, UI_DIM,
            "FPS  RX %u  decode %u  display %u  loop %u",
            perf->rx_fps, perf->decode_fps,
            perf->display_fps, perf->loop_fps);
    text_at(canvas, 368, 558, UI_SIZE_BODY, UI_DIM,
            "H264 %u.%u ms  upload %u.%u ms  queue %u  dropped %u",
            video.execute_avg_us / 1000,
            (video.execute_avg_us % 1000) / 100,
            gx2.copy_avg_us / 1000,
            (gx2.copy_avg_us % 1000) / 100,
            perf->video_queue, perf->video_dropped);
    text_at(canvas, 368, 594, UI_SIZE_BODY,
            net->may_control ? MENU_GREEN : MENU_RED,
            "INPUT: %s", net->may_control ? "CONTROL" : "VIEWER / ignored");
}

void menu_init(MenuState *menu)
{
    memset(menu, 0, sizeof(*menu));
    menu->page = MENU_PAGE_CONNECTION;
    menu->open = 1;
    menu->capture_pending = 1;
}

void menu_force_open(MenuState *menu, int open)
{
    menu->open = open ? 1 : 0;
}

int menu_is_open(const MenuState *menu)
{
    return menu && menu->open;
}

MenuAction menu_input(MenuState *menu,
                      const UiInput *input,
                      int streaming)
{
    if (!menu || !input || !input->tapped) {
        return MENU_ACTION_NONE;
    }

    if (streaming &&
        hit(&R_MARKER, input->touch_x, input->touch_y)) {
        menu->open = !menu->open;

        if (menu->open && menu->capture_index < 3) {
            menu->capture_pending = 1;
        }

        return MENU_ACTION_NONE;
    }

    if (streaming && !menu->open) {
        return MENU_ACTION_NONE;
    }

    if (hit(&R_CONNECTION, input->touch_x, input->touch_y)) {
        menu->page = MENU_PAGE_CONNECTION;
        return MENU_ACTION_NONE;
    }

    if (hit(&R_CONSOLE, input->touch_x, input->touch_y)) {
        menu->page = MENU_PAGE_CONSOLE;
        return MENU_ACTION_NONE;
    }

    if (menu->page == MENU_PAGE_CONNECTION) {
        if (hit(&R_HOST, input->touch_x, input->touch_y))
            return MENU_ACTION_HOST;
        if (hit(&R_PASSWORD, input->touch_x, input->touch_y))
            return MENU_ACTION_PASSWORD;
        if (hit(&R_PORT, input->touch_x, input->touch_y))
            return MENU_ACTION_NATIVE_PORT;
        if (hit(&R_WEB_PORT, input->touch_x, input->touch_y))
            return MENU_ACTION_WEB_PORT;
        if (hit(&R_CONNECT, input->touch_x, input->touch_y))
            return MENU_ACTION_CONNECT;
        if (hit(&R_EXIT, input->touch_x, input->touch_y))
            return MENU_ACTION_EXIT_HELP;
    } else {
        if (hit(&R_WAKE, input->touch_x, input->touch_y))
            return MENU_ACTION_WAKE;
        if (hit(&R_REMOTE_HOME, input->touch_x, input->touch_y))
            return MENU_ACTION_REMOTE_HOME;
        if (hit(&R_RESET, input->touch_x, input->touch_y))
            return MENU_ACTION_RESET_DONGLE;
        if (hit(&R_RESTART, input->touch_x, input->touch_y))
            return MENU_ACTION_RESTART_HOST;
        if (hit(&R_CONSOLE_EXIT, input->touch_x, input->touch_y))
            return MENU_ACTION_EXIT_HELP;
    }

    return MENU_ACTION_NONE;
}

void menu_draw(const MenuState *menu,
               const MenuView *view)
{
    if (!menu || !view || !view->settings) {
        return;
    }

    if (view->streaming && !menu->open) {
        ui_box(R_MARKER.x, R_MARKER.y,
               R_MARKER.w, R_MARKER.h,
               UI_PANEL, UI_DIM);
        ui_flush();
        return;
    }

    MenuCanvas canvas;
    memset(&canvas, 0, sizeof(canvas));

    add_shell(&canvas, menu, view);

    if (menu->page == MENU_PAGE_CONNECTION) {
        add_connection(&canvas, view);
    } else {
        add_console(&canvas, view);
    }

    add_diagnostics(&canvas, view);
    render_canvas(&canvas);

    if (view->streaming) {
        ui_box(R_MARKER.x, R_MARKER.y,
               R_MARKER.w, R_MARKER.h,
               UI_ACCENT, UI_DIM);
        ui_flush();
    }
}

int menu_take_capture(MenuState *menu,
                      unsigned *index)
{
    if (!menu || !menu->capture_pending) {
        return 0;
    }

    menu->capture_pending = 0;

    if (index) {
        *index = menu->capture_index;
    }

    menu->capture_index++;
    return 1;
}
