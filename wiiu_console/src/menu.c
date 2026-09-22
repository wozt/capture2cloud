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

static const MenuRect R_MENU = { 64, 38, 1152, 644 };
static const MenuRect R_SIDEBAR = { 84, 92, 218, 548 };
static const MenuRect R_CONNECTION = { 100, 160, 186, 40 };
static const MenuRect R_STREAM = { 100, 208, 186, 40 };
static const MenuRect R_CONTROLS = { 100, 256, 186, 40 };
static const MenuRect R_BINDINGS = { 100, 304, 186, 40 };
static const MenuRect R_INTERFACE = { 100, 352, 186, 40 };
static const MenuRect R_CONSOLE = { 100, 400, 186, 40 };

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

static const MenuRect R_RESOLUTION = { 348, 148, 390, 64 };
static const MenuRect R_FRAME_RATE = { 756, 148, 414, 64 };
static const MenuRect R_BITRATE = { 348, 230, 390, 64 };
static const MenuRect R_OUTPUT = { 756, 230, 414, 64 };

static const MenuRect R_LEFT_DEAD = { 348, 148, 390, 58 };
static const MenuRect R_LEFT_RANGE = { 756, 148, 414, 58 };
static const MenuRect R_RIGHT_DEAD = { 348, 218, 390, 58 };
static const MenuRect R_RIGHT_RANGE = { 756, 218, 414, 58 };
static const MenuRect R_INVERT_Y = { 348, 288, 390, 58 };
static const MenuRect R_MARKER_CORNER = { 348, 148, 390, 64 };
static const MenuRect R_MARKER_COLOUR = { 756, 148, 414, 64 };
static const MenuRect R_BIND_RESET = { 348, 532, 822, 50 };

static const int BINDING_TARGETS[INPUT_BUTTON_COUNT] = {
    PAD_A, PAD_B, PAD_X, PAD_Y,
    PAD_LS, PAD_RS, PAD_LB, PAD_RB,
    PAD_LT, PAD_RT, PAD_START, PAD_BACK,
    PAD_LEFT, PAD_UP, PAD_RIGHT, PAD_DOWN
};

static const UiColour MARKER_COLOURS[MARKER_COLOUR_COUNT] = {
    { 0xff, 0x00, 0xff, 0xff },
    { 0x00, 0xf0, 0xff, 0xff },
    { 0x6f, 0xff, 0x3f, 0xff },
    { 0xff, 0xff, 0xff, 0xff },
    { 0xff, 0x9f, 0x20, 0xff },
    { 0xff, 0x30, 0x50, 0xff }
};

typedef struct {
    unsigned width;
    unsigned height;
    const char *label;
} Resolution;

static const Resolution RESOLUTIONS[] = {
    { 848, 480, "480p" },
    { 1280, 720, "720p" }
};

static const unsigned FRAME_RATES[] = { 30, 60 };
static const unsigned BITRATES[] = { 2500, 4000, 6000, 10000, 15000 };

static int hit(const MenuRect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w &&
           y >= r->y && y < r->y + r->h;
}

static MenuRect marker_rect(const Settings *settings,
                            int hit_area)
{
    const int size = hit_area ? 10 : 5;
    const int corner = settings &&
                       settings->marker_corner < MARKER_CORNER_COUNT
        ? settings->marker_corner
        : MARKER_TOP_RIGHT;

    MenuRect r = { 0, 0, size, size };

    if (corner == MARKER_TOP_RIGHT ||
        corner == MARKER_BOTTOM_RIGHT) {
        r.x = UI_WIDTH - size;
    }

    if (corner == MARKER_BOTTOM_LEFT ||
        corner == MARKER_BOTTOM_RIGHT) {
        r.y = UI_HEIGHT - size;
    }

    return r;
}

static UiColour marker_colour(const Settings *settings)
{
    const unsigned index = settings &&
                           settings->marker_colour < MARKER_COLOUR_COUNT
        ? settings->marker_colour
        : MARKER_FUCHSIA;

    return MARKER_COLOURS[index];
}

static const char *marker_corner_name(unsigned corner)
{
    static const char *const names[MARKER_CORNER_COUNT] = {
        "Top left", "Top right", "Bottom left", "Bottom right"
    };

    return corner < MARKER_CORNER_COUNT ? names[corner] : names[MARKER_TOP_RIGHT];
}

static const char *marker_colour_name(unsigned colour)
{
    static const char *const names[MARKER_COLOUR_COUNT] = {
        "Fuchsia", "Cyan", "Lime", "White", "Orange", "Red"
    };

    return colour < MARKER_COLOUR_COUNT ? names[colour] : names[MARKER_FUCHSIA];
}

static MenuRect binding_rect(unsigned index)
{
    const int column = index >= 8 ? 1 : 0;
    const int row = index % 8;

    return (MenuRect){
        column ? 756 : 348,
        142 + row * 47,
        column ? 414 : 390,
        40
    };
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
    button(canvas, R_STREAM, "Stream",
           menu->page == MENU_PAGE_STREAM
               ? UI_ACCENT : UI_PANEL,
           UI_TEXT);
    button(canvas, R_CONTROLS, "Controls",
           menu->page == MENU_PAGE_CONTROLS
               ? UI_ACCENT : UI_PANEL,
           UI_TEXT);
    button(canvas, R_BINDINGS, "Bindings",
           menu->page == MENU_PAGE_BINDINGS
               ? UI_ACCENT : UI_PANEL,
           UI_TEXT);
    button(canvas, R_INTERFACE, "Interface",
           menu->page == MENU_PAGE_INTERFACE
               ? UI_ACCENT : UI_PANEL,
           UI_TEXT);
    button(canvas, R_CONSOLE, "Console",
           menu->page == MENU_PAGE_CONSOLE
               ? UI_ACCENT : UI_PANEL,
           UI_TEXT);

    text_at(canvas, 100, 548, UI_SIZE_BODY, UI_DIM,
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

static void add_stream(MenuCanvas *canvas,
                       const MenuState *menu,
                       const MenuView *view)
{
    char fps[24];
    char bitrate[24];

    snprintf(fps, sizeof(fps), "%u fps",
             FRAME_RATES[menu->fps_index]);
    snprintf(bitrate, sizeof(bitrate), "%u kbps",
             BITRATES[menu->bitrate_index]);

    text_at(canvas, 348, 74, UI_SIZE_TITLE,
            UI_TEXT, "Stream");
    text_at(canvas, 348, 112, UI_SIZE_BODY,
            UI_DIM, "Changes are sent to the dedicated Wii U encoder");

    field(canvas, R_RESOLUTION, "Resolution",
          RESOLUTIONS[menu->resolution_index].label, UI_TEXT);
    field(canvas, R_FRAME_RATE, "Frame rate", fps, UI_TEXT);
    field(canvas, R_BITRATE, "Bitrate", bitrate, UI_TEXT);
    field(canvas, R_OUTPUT, "Display",
          view->settings->output_mode == OUTPUT_GAMEPAD_ONLY
              ? "GamePad only"
              : "TV + GamePad",
          UI_TEXT);

    text_at(canvas, 348, 326, UI_SIZE_BODY, MENU_GREEN,
            "720p60 is the validated path");
    text_at(canvas, 348, 362, UI_SIZE_BODY, UI_DIM,
            "1080p stays hidden until the decoder is allocated and tested for it");
    text_at(canvas, 348, 398, UI_SIZE_BODY, UI_DIM,
            "GamePad-only disables TV output and is saved on SD");
}

static void add_controls(MenuCanvas *canvas,
                         const MenuView *view)
{
    char left_dead[16];
    char left_range[16];
    char right_dead[16];
    char right_range[16];

    const InputConfig *input = &view->settings->input;

    snprintf(left_dead, sizeof(left_dead), "%u%%", input->deadzone[0]);
    snprintf(left_range, sizeof(left_range), "%u%%", input->range[0]);
    snprintf(right_dead, sizeof(right_dead), "%u%%", input->deadzone[1]);
    snprintf(right_range, sizeof(right_range), "%u%%", input->range[1]);

    text_at(canvas, 348, 74, UI_SIZE_TITLE,
            UI_TEXT, "Controls");
    text_at(canvas, 348, 112, UI_SIZE_BODY,
            UI_DIM, "Applied immediately to Wii U GamePad input");

    field(canvas, R_LEFT_DEAD, "Left deadzone", left_dead, UI_TEXT);
    field(canvas, R_LEFT_RANGE, "Left full range", left_range, UI_TEXT);
    field(canvas, R_RIGHT_DEAD, "Right deadzone", right_dead, UI_TEXT);
    field(canvas, R_RIGHT_RANGE, "Right full range", right_range, UI_TEXT);
    field(canvas, R_INVERT_Y, "Invert stick Y",
          input->invert_y ? "On" : "Off",
          input->invert_y ? MENU_GREEN : UI_TEXT);

    text_at(canvas, 348, 364, UI_SIZE_BODY, UI_DIM,
            "All digital buttons can be changed in the Bindings tab");
    text_at(canvas, 348, 400, UI_SIZE_BODY, UI_DIM,
            "ZL / ZR are digital on this SDL Wii U path; no fake threshold is shown");
}

static void add_bindings(MenuCanvas *canvas,
                         const MenuState *menu,
                         const MenuView *view)
{
    text_at(canvas, 348, 66, UI_SIZE_TITLE,
            UI_TEXT, "Bindings");
    text_at(canvas, 348, 108, UI_SIZE_BODY,
            menu->binding_target_slot >= 0 ? MENU_GREEN : UI_DIM,
            "%s",
            menu->binding_target_slot >= 0
                ? "Press the physical Wii U button for the highlighted output"
                : "Tap an output, then press its physical Wii U button");

    for (unsigned i = 0; i < INPUT_BUTTON_COUNT; ++i) {
        const int slot = BINDING_TARGETS[i];
        const int physical =
            input_config_physical_for_slot(
                &view->settings->input,
                slot);

        const MenuRect r = binding_rect(i);
        const int selected =
            menu->binding_target_slot == slot;

        box(canvas, r,
            selected ? UI_ACCENT : MENU_CARD,
            selected ? MENU_GREEN : UI_DIM);

        text_at(canvas, r.x + 14, r.y + 5,
                UI_SIZE_BODY, UI_DIM,
                "%s", input_pad_slot_name(slot));

        text_at(canvas, r.x + 180, r.y + 5,
                UI_SIZE_BODY, selected ? UI_TEXT : MENU_GREEN,
                "%s", input_physical_button_name(physical));
    }

    button(canvas, R_BIND_RESET, "RESET XBOX-POSITION DEFAULTS",
           UI_PANEL, UI_TEXT);

    if (view->note && view->note[0]) {
        text_at(canvas, 348, 594, UI_SIZE_BODY,
                MENU_GREEN, "%s", view->note);
    } else {
        text_at(canvas, 348, 594, UI_SIZE_BODY,
                UI_DIM, "Wii U HOME remains reserved for the local system menu");
    }
}

static void add_interface(MenuCanvas *canvas,
                          const MenuView *view)
{
    text_at(canvas, 348, 74, UI_SIZE_TITLE,
            UI_TEXT, "Interface");
    text_at(canvas, 348, 112, UI_SIZE_BODY,
            UI_DIM, "Menu opener appearance and position");

    field(canvas, R_MARKER_CORNER, "Corner",
          marker_corner_name(view->settings->marker_corner), UI_TEXT);
    field(canvas, R_MARKER_COLOUR, "Colour",
          marker_colour_name(view->settings->marker_colour),
          marker_colour(view->settings));

    text_at(canvas, 348, 248, UI_SIZE_BODY, UI_DIM,
            "Visible marker: 5 x 5 pixels at the exact screen edge");
    text_at(canvas, 348, 284, UI_SIZE_BODY, UI_DIM,
            "Touch target: invisible 10 x 10 pixels in the selected corner");

    box(canvas, (MenuRect){ 348, 336, 822, 82 },
        MENU_CARD, UI_PANEL);
    text_at(canvas, 372, 352, UI_SIZE_BODY, UI_DIM,
            "Preview");
    box(canvas, (MenuRect){ 1100, 366, 5, 5 },
        marker_colour(view->settings), marker_colour(view->settings));
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

    text_at(canvas, 348, 450, UI_SIZE_BODY, UI_DIM,
            "L3 + R3 sends remote HOME without opening the Wii U HOME menu");
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
    menu->resolution_index = 1;
    menu->fps_index = 1;
    menu->bitrate_index = 2;
    menu->binding_target_slot = -1;
}

static void stream_values(const MenuState *menu,
                          unsigned *width,
                          unsigned *height,
                          unsigned *fps,
                          unsigned *bitrate_kbps)
{
    if (width) *width = RESOLUTIONS[menu->resolution_index].width;
    if (height) *height = RESOLUTIONS[menu->resolution_index].height;
    if (fps) *fps = FRAME_RATES[menu->fps_index];
    if (bitrate_kbps) *bitrate_kbps = BITRATES[menu->bitrate_index];
}

void menu_adopt_stream(MenuState *menu,
                       unsigned width,
                       unsigned height,
                       unsigned fps,
                       unsigned bitrate_kbps)
{
    if (!menu) return;
    if (menu->profile_dirty) return;

    unsigned best = 0;
    unsigned best_cost = ~0u;
    for (unsigned i = 0; i < sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0]); ++i) {
        unsigned cost =
            (RESOLUTIONS[i].height > height
                 ? RESOLUTIONS[i].height - height
                 : height - RESOLUTIONS[i].height) * 10 +
            (RESOLUTIONS[i].width > width
                 ? RESOLUTIONS[i].width - width
                 : width - RESOLUTIONS[i].width);
        if (cost < best_cost) {
            best = i;
            best_cost = cost;
        }
    }
    menu->resolution_index = best;

    menu->fps_index = fps >= 45 ? 1 : 0;

    best = 0;
    best_cost = ~0u;
    for (unsigned i = 0; i < sizeof(BITRATES) / sizeof(BITRATES[0]); ++i) {
        unsigned cost = BITRATES[i] > bitrate_kbps
            ? BITRATES[i] - bitrate_kbps
            : bitrate_kbps - BITRATES[i];
        if (cost < best_cost) {
            best = i;
            best_cost = cost;
        }
    }
    menu->bitrate_index = best;
}

int menu_change_stream(MenuState *menu,
                       MenuAction action,
                       unsigned *width,
                       unsigned *height,
                       unsigned *fps,
                       unsigned *bitrate_kbps)
{
    if (!menu) return 0;

    if (action == MENU_ACTION_RESOLUTION) {
        menu->resolution_index =
            (menu->resolution_index + 1) %
            (sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0]));
    } else if (action == MENU_ACTION_FRAME_RATE) {
        menu->fps_index =
            (menu->fps_index + 1) %
            (sizeof(FRAME_RATES) / sizeof(FRAME_RATES[0]));
    } else if (action == MENU_ACTION_BITRATE) {
        menu->bitrate_index =
            (menu->bitrate_index + 1) %
            (sizeof(BITRATES) / sizeof(BITRATES[0]));
    } else {
        return 0;
    }

    menu->profile_dirty = 1;
    stream_values(menu, width, height, fps, bitrate_kbps);
    return 1;
}

int menu_take_profile_dirty(MenuState *menu,
                            unsigned *width,
                            unsigned *height,
                            unsigned *fps,
                            unsigned *bitrate_kbps)
{
    if (!menu || !menu->profile_dirty) return 0;

    menu->profile_dirty = 0;
    stream_values(menu, width, height, fps, bitrate_kbps);
    return 1;
}

void menu_force_open(MenuState *menu, int open)
{
    menu->open = open ? 1 : 0;
}

int menu_is_open(const MenuState *menu)
{
    return menu && menu->open;
}

int menu_binding_target(const MenuState *menu)
{
    return menu ? menu->binding_target_slot : -1;
}

void menu_binding_finish(MenuState *menu)
{
    if (menu) {
        menu->binding_target_slot = -1;
    }
}

MenuAction menu_input(MenuState *menu,
                      const UiInput *input,
                      int streaming,
                      const Settings *settings)
{
    if (!menu || !input || !input->tapped) {
        return MENU_ACTION_NONE;
    }

    const MenuRect marker_hit =
        marker_rect(settings, 1);

    if (streaming &&
        hit(&marker_hit, input->touch_x, input->touch_y)) {
        menu->open = !menu->open;

        return MENU_ACTION_NONE;
    }

    if (streaming && !menu->open) {
        return MENU_ACTION_NONE;
    }

    if (hit(&R_CONNECTION, input->touch_x, input->touch_y)) {
        menu->page = MENU_PAGE_CONNECTION;
        menu->binding_target_slot = -1;
        return MENU_ACTION_NONE;
    }

    if (hit(&R_STREAM, input->touch_x, input->touch_y)) {
        menu->page = MENU_PAGE_STREAM;
        menu->binding_target_slot = -1;
        return MENU_ACTION_NONE;
    }

    if (hit(&R_CONTROLS, input->touch_x, input->touch_y)) {
        menu->page = MENU_PAGE_CONTROLS;
        menu->binding_target_slot = -1;
        return MENU_ACTION_NONE;
    }

    if (hit(&R_BINDINGS, input->touch_x, input->touch_y)) {
        menu->page = MENU_PAGE_BINDINGS;
        menu->binding_target_slot = -1;
        return MENU_ACTION_NONE;
    }

    if (hit(&R_INTERFACE, input->touch_x, input->touch_y)) {
        menu->page = MENU_PAGE_INTERFACE;
        menu->binding_target_slot = -1;
        return MENU_ACTION_NONE;
    }

    if (hit(&R_CONSOLE, input->touch_x, input->touch_y)) {
        menu->page = MENU_PAGE_CONSOLE;
        menu->binding_target_slot = -1;
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
    } else if (menu->page == MENU_PAGE_STREAM) {
        if (hit(&R_RESOLUTION, input->touch_x, input->touch_y))
            return MENU_ACTION_RESOLUTION;
        if (hit(&R_FRAME_RATE, input->touch_x, input->touch_y))
            return MENU_ACTION_FRAME_RATE;
        if (hit(&R_BITRATE, input->touch_x, input->touch_y))
            return MENU_ACTION_BITRATE;
        if (hit(&R_OUTPUT, input->touch_x, input->touch_y))
            return MENU_ACTION_OUTPUT;
    } else if (menu->page == MENU_PAGE_CONTROLS) {
        if (hit(&R_LEFT_DEAD, input->touch_x, input->touch_y))
            return MENU_ACTION_LEFT_DEADZONE;
        if (hit(&R_LEFT_RANGE, input->touch_x, input->touch_y))
            return MENU_ACTION_LEFT_RANGE;
        if (hit(&R_RIGHT_DEAD, input->touch_x, input->touch_y))
            return MENU_ACTION_RIGHT_DEADZONE;
        if (hit(&R_RIGHT_RANGE, input->touch_x, input->touch_y))
            return MENU_ACTION_RIGHT_RANGE;
        if (hit(&R_INVERT_Y, input->touch_x, input->touch_y))
            return MENU_ACTION_INVERT_Y;
    } else if (menu->page == MENU_PAGE_BINDINGS) {
        for (unsigned i = 0; i < INPUT_BUTTON_COUNT; ++i) {
            const MenuRect r = binding_rect(i);

            if (hit(&r, input->touch_x, input->touch_y)) {
                menu->binding_target_slot = BINDING_TARGETS[i];
                return MENU_ACTION_BIND_SELECT;
            }
        }

        if (hit(&R_BIND_RESET, input->touch_x, input->touch_y))
            return MENU_ACTION_BIND_RESET;
    } else if (menu->page == MENU_PAGE_INTERFACE) {
        if (hit(&R_MARKER_CORNER, input->touch_x, input->touch_y))
            return MENU_ACTION_MARKER_CORNER;
        if (hit(&R_MARKER_COLOUR, input->touch_x, input->touch_y))
            return MENU_ACTION_MARKER_COLOUR;
    } else if (menu->page == MENU_PAGE_CONSOLE) {
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
        const MenuRect marker =
            marker_rect(view->settings, 0);

        ui_fill(marker.x, marker.y,
                marker.w, marker.h,
                marker_colour(view->settings));
        ui_flush();
        return;
    }

    MenuCanvas canvas;
    memset(&canvas, 0, sizeof(canvas));

    add_shell(&canvas, menu, view);

    if (menu->page == MENU_PAGE_CONNECTION) {
        add_connection(&canvas, view);
    } else if (menu->page == MENU_PAGE_STREAM) {
        add_stream(&canvas, menu, view);
    } else if (menu->page == MENU_PAGE_CONTROLS) {
        add_controls(&canvas, view);
    } else if (menu->page == MENU_PAGE_BINDINGS) {
        add_bindings(&canvas, menu, view);
    } else if (menu->page == MENU_PAGE_INTERFACE) {
        add_interface(&canvas, view);
    } else {
        add_console(&canvas, view);
    }

    if (menu->page != MENU_PAGE_BINDINGS &&
        menu->page != MENU_PAGE_INTERFACE) {
        add_diagnostics(&canvas, view);
    }
    render_canvas(&canvas);

    if (view->streaming) {
        const MenuRect marker =
            marker_rect(view->settings, 0);

        ui_fill(marker.x, marker.y,
                marker.w, marker.h,
                marker_colour(view->settings));
        ui_flush();
    }
}
