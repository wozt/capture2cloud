/*
 * capture2cloud client -- the Switch 1 client for capture2cloud.
 *
 * Starts on the menu rather than on the stream: connecting is something
 * you ask for, and until you do there is nothing to show. The stream
 * takes over the whole screen once it is running, and the menu comes
 * back over it on L3+R3 or a tap at the top of the screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <switch.h>

#include "audio.h"
#include "input.h"
#include "net.h"
#include "ui.h"
#include "vpad.h"
#include "video.h"

#define SCREEN_W 1280
#define SCREEN_H 720

/* Bumped when the MEANING of a stored value changes, so the old one can
 * be converted rather than misread. */
#define CONFIG_VERSION 3

#define CONFIG_PATH "sdmc:/switch/capture2switch.cfg"

typedef enum { SCREEN_MENU, SCREEN_STREAM, SCREEN_PADTEST } Screen;

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static Screen g_screen = SCREEN_MENU;
static int g_running = 1;
static int g_decoders_ready = 0;

static char g_host[64] = "192.168.2.100";
static uint16_t g_port = C2S_DEFAULT_PORT;
static char g_token[80] = "";
/* The host password, kept so it is typed once rather than every launch.
 * See save_config() for what that means and what it costs. */
static char g_password[64] = "";
/* Whether a saved password is used the moment the stream connects.
 *
 * Off by default: connecting is one thing and taking control of someone
 * else's console is another, and a remembered password should save the
 * typing rather than make the decision. On for anyone who would rather
 * not press it every launch. */
static int g_autologin = 0;
/* The web server's port, where /login lives -- the native transport is a
 * different one. */
static uint16_t g_web_port = 5080;
static char g_login_message[96] = "";

static unsigned long g_video_frames = 0, g_audio_frames = 0;
/* Frames thrown away for being late. Worth showing: a picture that looks
 * fine while this climbs means the link cannot carry the profile chosen,
 * which is not something the picture itself would tell you. */
static unsigned long g_video_dropped = 0;

/* How many pictures one pass of the frame loop will decode before
 * deciding it is not catching up. A burst of two or three is ordinary --
 * that is just TCP -- so this is generous; only real overload reaches
 * it. */
#define MAX_DECODE_PER_PASS 8

/* Bytes received in the current measurement window, and what that came
 * to. What the link actually delivers is the one number that says
 * whether a bitrate is achievable here, and there is no other way to
 * find out from this console. */
static unsigned long long g_window_bytes = 0;
static Uint32 g_window_since = 0;
static double g_link_mbps = 0.0;
static double g_draw_fps = 0.0;
static unsigned long g_window_draws = 0;

/* Whether the diagnostics panel is drawn at all. With it off the menu
 * gets the whole screen and nothing above is measured. */
/* Five at a time over a range of a hundred: twenty presses end to end,
 * and small enough to settle on a level rather than step over it. */
#define VOLUME_STEP 5

static int g_show_diagnostics = 1;

/* Whether anything is written over the picture -- the notice that says
 * the controller is not being sent, and the passing messages. Some
 * people would rather have a clean picture than be told. */
static int g_show_info = 1;
/* Set whenever the decoder is rebuilt: see where it is tested. */
static int g_await_keyframe = 1;
static unsigned long long g_video_bytes = 0;

static const SDL_Color COL_TEXT     = {235, 235, 240, 255};
static const SDL_Color COL_DIM      = {150, 150, 160, 255};
static const SDL_Color COL_ACCENT   = {120, 190, 255, 255};
static const SDL_Color COL_GOOD     = { 90, 210, 130, 255};
static const SDL_Color COL_BAD      = {235, 110, 110, 255};
static const SDL_Color COL_SELECTED = { 45,  85, 140, 255};
static const SDL_Color COL_ROW      = { 38,  40,  48, 170};

/* The colour behind everything when there is no picture to show.
 *
 * It used to be very nearly black, which on a console is the same thing
 * as a screen that has gone to sleep -- and there are several moments
 * where nothing is being decoded and the only question worth answering
 * is "is this thing still on". A mid grey answers it at a glance. */
static const SDL_Color COL_BACKDROP = {104, 110, 122, 255};

/* The menu over a running stream. Light enough to read against, thin
 * enough to keep watching through: the console carries on playing while
 * the menu is open, and seeing that is half the point of opening it. */
static const SDL_Color COL_SCRIM    = {  8,  10,  16, 128};

/* --- menu ------------------------------------------------------------ */

typedef enum {
    MENU_CONNECT = 0,
    MENU_HOST,
    MENU_PORT,
    MENU_LOGIN,
    MENU_FORGET_PASSWORD,
    MENU_AUTOLOGIN,
    MENU_PROFILE,
    MENU_BITRATE,
    MENU_CODEC,
    MENU_LSTICK_DEAD,
    MENU_LSTICK_RANGE,
    MENU_LSTICK_DIAG,
    MENU_RSTICK_DEAD,
    MENU_RSTICK_RANGE,
    MENU_RSTICK_DIAG,
    MENU_PADTEST,
    MENU_TOUCHPAD,
    MENU_TOUCH_EDIT,
    MENU_TOUCH_COLOUR,
    MENU_TOUCH_OPACITY,
    MENU_TOUCH_RESET,
    MENU_BRIGHTNESS,
    MENU_CONTRAST,
    MENU_SATURATION,
    MENU_HUE,
    MENU_PICTURE_RESET,
    MENU_DIAGNOSTICS,
    MENU_INFO_LINE,
    MENU_HOME,
    MENU_WAKE,
    MENU_RESET_DONGLE,
    MENU_RESTART_HOST,
    MENU_MUTE,
    MENU_VOLUME_DOWN,
    MENU_VOLUME_UP,
    MENU_QUIT,
    MENU_COUNT
} MenuAction;

/* The menu, in groups.
 *
 * It was one flat list of twenty-odd entries that had to be scrolled
 * through to reach anything, and the things reached most often -- the
 * stream, the sound -- were somewhere in the middle of it. Grouped the
 * same way the browser page groups them, so the two are learned once.
 * The category list is short enough to fit without scrolling, and each
 * group is short enough to read at a glance. */
typedef struct {
    const char *title;
    const MenuAction *items;
    int count;
} MenuCategory;

/* Logging in is not in here: it is pinned at the top of the root list,
 * because it is the one thing that has to be found without looking for
 * it. Everything about the controller stops at it -- the sticks and the
 * on-screen pad alike -- and buried three rows inside a category it was
 * simply forgotten, which reads as the controls being broken. */
static const MenuAction CAT_CONNECTION_ITEMS[] = {
    MENU_CONNECT, MENU_HOST, MENU_PORT, MENU_AUTOLOGIN, MENU_FORGET_PASSWORD};
static const MenuAction CAT_STREAM_ITEMS[] = {MENU_PROFILE, MENU_BITRATE, MENU_CODEC};
static const MenuAction CAT_TOUCH_ITEMS[] = {
    MENU_TOUCHPAD, MENU_TOUCH_EDIT, MENU_TOUCH_COLOUR, MENU_TOUCH_OPACITY, MENU_TOUCH_RESET};
static const MenuAction CAT_STICKS_ITEMS[] = {
    MENU_LSTICK_DEAD, MENU_LSTICK_RANGE, MENU_LSTICK_DIAG,
    MENU_RSTICK_DEAD, MENU_RSTICK_RANGE, MENU_RSTICK_DIAG, MENU_PADTEST};
static const MenuAction CAT_PICTURE_ITEMS[] = {
    MENU_BRIGHTNESS, MENU_CONTRAST, MENU_SATURATION, MENU_HUE, MENU_PICTURE_RESET};
static const MenuAction CAT_SOUND_ITEMS[] = {MENU_MUTE, MENU_VOLUME_DOWN, MENU_VOLUME_UP};
static const MenuAction CAT_CONSOLE_ITEMS[] = {
    MENU_HOME, MENU_WAKE, MENU_RESET_DONGLE, MENU_RESTART_HOST};
static const MenuAction CAT_SYSTEM_ITEMS[] = {MENU_DIAGNOSTICS, MENU_INFO_LINE, MENU_QUIT};

#define CAT(name, items) {name, items, (int)(sizeof(items) / sizeof((items)[0]))}
static const MenuCategory CATEGORIES[] = {
    CAT("connection", CAT_CONNECTION_ITEMS),
    CAT("stream",     CAT_STREAM_ITEMS),
    CAT("picture",    CAT_PICTURE_ITEMS),
    CAT("touch pad",  CAT_TOUCH_ITEMS),
    CAT("controls",   CAT_STICKS_ITEMS),
    CAT("sound",      CAT_SOUND_ITEMS),
    CAT("console",    CAT_CONSOLE_ITEMS),
    CAT("system",     CAT_SYSTEM_ITEMS),
};
#undef CAT
#define CATEGORY_COUNT ((int)(sizeof(CATEGORIES) / sizeof(CATEGORIES[0])))

/* -1 while the category list is showing, otherwise which one is open. */
static int g_category = -1;

/* The root list is the pinned entry followed by the categories. */
#define PINNED_ROWS 1

/* What the host is asked to encode. Software VP8 on this hardware does
 * not reach 720p60 -- the console then skips frames, which on a
 * predictive codec smears the picture until the next keyframe. Picking
 * something decodable is the fix; skipping is the symptom.
 *
 * 480p60 first because it is the one that certainly works, including in
 * applet mode where the HOME menu is still resident and taking its
 * share. */
static const struct { int w, h, fps, kbps; const char *label; } PROFILES[] = {
    {640, 360, 30, 1500, "360p30"},
    {854, 480, 30, 2500, "480p30"},
    {854, 480, 60, 4000, "480p60"},
    {1280, 720, 30, 4000, "720p30"},
    {1280, 720, 60, 6000, "720p60"},
};
#define PROFILE_COUNT ((int)(sizeof(PROFILES) / sizeof(PROFILES[0])))
static int g_profile = 1;   /* 480p30: what certainly works, including in applet mode */
static int g_adapt_enabled = 1;

/* Bitrate, chosen separately from the size and rate. They interact --
 * more pixels need more bits -- but which one to give up first is a
 * judgement about the picture, not something to be decided here: a
 * sharp 30 fps and a soft 60 are both reasonable answers. 0 means "use
 * whatever the profile suggests". */
static const int BITRATES[] = {0, 1000, 2000, 4000, 6000, 10000};
#define BITRATE_COUNT ((int)(sizeof(BITRATES) / sizeof(BITRATES[0])))
static int g_bitrate_index = 0;

/* H.264 first: it is what the console's video engine decodes, and that
 * is the whole point of offering the choice. VP8 stays because it needs
 * no second encoder on the host and works with the software fallback. */
static int g_codec = VIDEO_CODEC_H264;

/* The values the two stick settings step through. Cycling a short list
 * beats a slider on a screen driven by the very stick being adjusted. */
static const int DEADZONES[] = {0, 2, 5, 8, 12, 16, 20, 25};
#define DEADZONE_COUNT ((int)(sizeof(DEADZONES) / sizeof(DEADZONES[0])))
/* Down to 45, because a stick that no longer reaches far -- especially
 * into a corner, where it reaches least -- needs an outer limit set to
 * what it actually does reach. */
static const int SATURATIONS[] = {100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45};
#define SATURATION_COUNT ((int)(sizeof(SATURATIONS) / sizeof(SATURATIONS[0])))

/* The entry after `current`, wrapping. Falls to the first when the
 * current value is not in the list -- which is what a hand-edited config
 * file produces. */
static int next_in(const int *values, int count, int current) {
    for (int i = 0; i < count; i++) {
        if (values[i] == current) {
            return values[(i + 1) % count];
        }
    }
    return values[0];
}

/* Read from the config file before the audio module exists, and applied
 * once it does. */
static int g_saved_volume = 13; /* about the stream's own level; see audio.h */
static int g_saved_muted = 0;

/* --- configuration --------------------------------------------------- */

/* Read from the SD card: there is no keyboard here worth typing an IP
 * address on, and the host's address changes more often than this
 * program does. */
static void load_config(void) {
    int cfg_ldead = 2, cfg_lsat = 100, cfg_ldiag = 100;
    int cfg_rdead = 2, cfg_rsat = 100, cfg_rdiag = 100;
    int cfg_touchpad = 0, cfg_colour = 0, cfg_opacity = 100;
    int cfg_version = 1;
    int cfg_bright = 100, cfg_contrast = 100, cfg_sat = 100, cfg_hue = 0;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        printf("no %s, using %s:%u\n", CONFIG_PATH, g_host, g_port);
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *value = eq + 1;
        value[strcspn(value, "\r\n")] = '\0';
        if (strcmp(key, "host") == 0) snprintf(g_host, sizeof(g_host), "%s", value);
        else if (strcmp(key, "port") == 0) g_port = (uint16_t)atoi(value);
        else if (strcmp(key, "web_port") == 0) g_web_port = (uint16_t)atoi(value);
        else if (strcmp(key, "password") == 0) snprintf(g_password, sizeof(g_password), "%s", value);
        else if (strcmp(key, "touchpad") == 0) cfg_touchpad = atoi(value) != 0;
        else if (strcmp(key, "touch_layout") == 0) vpad_layout_from_string(value);
        else if (strcmp(key, "touch_colour") == 0) cfg_colour = atoi(value);
        else if (strcmp(key, "touch_opacity") == 0) cfg_opacity = atoi(value);
        else if (strcmp(key, "brightness") == 0) cfg_bright = atoi(value);
        else if (strcmp(key, "contrast") == 0) cfg_contrast = atoi(value);
        else if (strcmp(key, "saturation") == 0) cfg_sat = atoi(value);
        else if (strcmp(key, "hue") == 0) cfg_hue = atoi(value);
        else if (strcmp(key, "info_line") == 0) g_show_info = atoi(value) != 0;
        else if (strcmp(key, "autologin") == 0) g_autologin = atoi(value) != 0;
        else if (strcmp(key, "diagnostics") == 0) g_show_diagnostics = atoi(value) != 0;
        else if (strcmp(key, "lstick_deadzone") == 0) cfg_ldead = atoi(value);
        else if (strcmp(key, "lstick_range") == 0) cfg_lsat = atoi(value);
        else if (strcmp(key, "rstick_deadzone") == 0) cfg_rdead = atoi(value);
        else if (strcmp(key, "rstick_range") == 0) cfg_rsat = atoi(value);
        else if (strcmp(key, "lstick_diagonal") == 0) cfg_ldiag = atoi(value);
        else if (strcmp(key, "rstick_diagonal") == 0) cfg_rdiag = atoi(value);
        else if (strcmp(key, "profile") == 0) g_profile = atoi(value);
        else if (strcmp(key, "bitrate") == 0) g_bitrate_index = atoi(value);
        else if (strcmp(key, "codec") == 0) g_codec = atoi(value);
        else if (strcmp(key, "adapt") == 0) g_adapt_enabled = atoi(value) != 0;
        else if (strcmp(key, "version") == 0) cfg_version = atoi(value);
        else if (strcmp(key, "volume") == 0) g_saved_volume = atoi(value);
        else if (strcmp(key, "muted") == 0) g_saved_muted = atoi(value) != 0;
    }
    fclose(f);

    /* Clamped, not trusted: this file is hand-edited on an SD card, and
     * an index out of range would read past the end of a table. */
    if (g_profile < 0 || g_profile >= (int)(sizeof(PROFILES) / sizeof(*PROFILES))) g_profile = 1;
    if (g_bitrate_index < 0 || g_bitrate_index >= (int)(sizeof(BITRATES) / sizeof(*BITRATES))) g_bitrate_index = 0;
    if (g_codec != VIDEO_CODEC_VP8 && g_codec != VIDEO_CODEC_H264) g_codec = VIDEO_CODEC_H264;
    /* Version 1 wrote the volume on a 0-200 scale where 100 was the
     * stream's own level; it now runs 0-100 with that level at 25, for
     * the same range of loudness. Left alone, everyone's sound would
     * have jumped to four times what they had chosen. */
    if (cfg_version < 2) {
        g_saved_volume /= 4;
    }
    /* Version 2's scale topped out at four times the stream's own level;
     * it now tops out at eight, so the same position is twice as loud.
     * Halved, or everyone's sound would double on the next launch. */
    if (cfg_version < 3) {
        g_saved_volume /= 2;
    }
    if (g_saved_volume < 0) g_saved_volume = 0;
    if (g_saved_volume > 100) g_saved_volume = 100;

    video_set_adjust(cfg_bright, cfg_contrast, cfg_sat, cfg_hue);
    vpad_set_enabled(cfg_touchpad);
    vpad_set_colour(cfg_colour < 0 ? 0 : cfg_colour % vpad_colour_count());
    vpad_set_opacity(cfg_opacity);
    input_set_stick_limits(0, cfg_ldead, cfg_lsat, cfg_ldiag);
    input_set_stick_limits(1, cfg_rdead, cfg_rsat, cfg_rdiag);

    printf("config: %s:%u%s\n", g_host, g_port,
           g_password[0] ? " (password saved)" : " (no password)");
}

/* Written back whenever a menu entry changes something, so a choice made
 * once survives the next launch. There is no keyboard here and no shell:
 * re-picking the profile, the codec and the volume on every start is a
 * chore nobody should be asked to repeat.
 *
 * The whole file is rewritten rather than patched -- it is six lines --
 * and the address lines are carried through so saving a volume never
 * loses the host you typed. */
/* Settings are written on their own, a moment after the last change.
 *
 * Every entry used to save as it was activated, which missed the one
 * thing that is not a menu entry: dragging the on-screen buttons happens
 * on the stream, and the arrangement was lost on the way out of the very
 * mode built to change it. Watching for change instead of being told
 * cannot miss anything, and the delay means holding a volume button does
 * not write the file ten times.
 *
 * Two seconds is short enough that closing the console loses nothing
 * anyone would notice, and long enough that a burst of changes is one
 * write. */
#define CONFIG_AUTOSAVE_DELAY_MS 2000

static int g_config_dirty = 0;
static Uint32 g_config_dirty_since = 0;

static void settings_changed(void) {
    if (!g_config_dirty) {
        g_config_dirty = 1;
        g_config_dirty_since = SDL_GetTicks();
    }
}

static void save_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        printf("could not write %s\n", CONFIG_PATH);
        return;
    }
    fprintf(f, "version=%d\n", CONFIG_VERSION);
    fprintf(f, "host=%s\n", g_host);
    fprintf(f, "port=%u\n", g_port);
    fprintf(f, "web_port=%u\n", g_web_port);
    /* The password, not the session token.
     *
     * The token expires and would be dead weight next time; the password
     * is what saves the typing, which on a console with no keyboard is
     * the whole point. It is stored in the clear on the SD card, which
     * is what a file on an SD card is -- anyone holding the console can
     * read it. It is never used on its own: connecting is always as a
     * viewer, and becoming a player is a thing you press. */
    fprintf(f, "password=%s\n", g_password);
    fprintf(f, "autologin=%d\n", g_autologin);
    fprintf(f, "diagnostics=%d\n", g_show_diagnostics);
    fprintf(f, "info_line=%d\n", g_show_info);
    {
        int b = 0, c = 0, sat = 0, hue = 0;
        video_get_adjust(&b, &c, &sat, &hue);
        fprintf(f, "brightness=%d\ncontrast=%d\nsaturation=%d\nhue=%d\n", b, c, sat, hue);
    }
    fprintf(f, "touchpad=%d\n", vpad_enabled());
    fprintf(f, "touch_colour=%d\n", vpad_colour());
    fprintf(f, "touch_opacity=%d\n", vpad_opacity());
    {
        char layout[512];
        vpad_layout_to_string(layout, sizeof(layout));
        fprintf(f, "touch_layout=%s\n", layout);
    }
    {
        int d = 0, sat = 0, diag = 0;
        input_get_stick_limits(0, &d, &sat, &diag);
        fprintf(f, "lstick_deadzone=%d\nlstick_range=%d\nlstick_diagonal=%d\n", d, sat, diag);
        input_get_stick_limits(1, &d, &sat, &diag);
        fprintf(f, "rstick_deadzone=%d\nrstick_range=%d\nrstick_diagonal=%d\n", d, sat, diag);
    }
    fprintf(f, "profile=%d\n", g_profile);
    fprintf(f, "bitrate=%d\n", g_bitrate_index);
    fprintf(f, "codec=%d\n", g_codec);
    fprintf(f, "adapt=%d\n", g_adapt_enabled);
    fprintf(f, "volume=%d\n", audio_volume());
    fprintf(f, "muted=%d\n", audio_is_muted());
    fclose(f);
    g_config_dirty = 0;
}

/* Writes if anything changed and the change has settled. Called from the
 * frame loop; the file is a handful of lines, so the write itself is not
 * worth scheduling around. */
static void save_config_if_needed(void) {
    if (g_config_dirty && SDL_GetTicks() - g_config_dirty_since >= CONFIG_AUTOSAVE_DELAY_MS) {
        save_config();
    }
}


static int current_bitrate(void) {
    return BITRATES[g_bitrate_index] ? BITRATES[g_bitrate_index] : PROFILES[g_profile].kbps;
}
static double g_measured_fps = 0;

/* Long enough that one slow frame does not move anything. */
#define ADAPT_WINDOW_MS 3000

/* Applet mode is the album-launched one: much less RAM, and the HOME
 * menu still running behind. Worth saying out loud, because it is the
 * difference between "this homebrew is slow" and "it was never given
 * the machine". */
static int g_applet_mode = 0;

static int g_menu_index = 0;

/* Labels are built each frame because most of them say what state they
 * would put things in, and that changes. */
static void menu_label(int index, char *out, size_t out_size, char *detail, size_t detail_size) {
    const NetInfo *n = net_info();
    detail[0] = '\0';
    switch (index) {
        case MENU_CONNECT:
            if (n->state == NET_CONNECTED) {
                snprintf(out, out_size, "Disconnect");
                snprintf(detail, detail_size, "connected to %s", g_host);
            } else if (n->state == NET_CONNECTING) {
                snprintf(out, out_size, "Connecting...");
                snprintf(detail, detail_size, "%s", n->status);
            } else {
                snprintf(out, out_size, "Connect");
                snprintf(detail, detail_size, "%s:%u", g_host, g_port);
            }
            break;
        case MENU_HOST:
            snprintf(out, out_size, "Host address");
            snprintf(detail, detail_size, "%s", g_host);
            break;
        case MENU_PORT:
            snprintf(out, out_size, "Host port");
            snprintf(detail, detail_size, "%u   (web %u)", g_port, g_web_port);
            break;
        case MENU_LOGIN:
            if (net_info()->may_control) {
                snprintf(out, out_size, "Log out");
                snprintf(detail, detail_size, "back to watching only");
            } else {
                snprintf(out, out_size, "Log in as player");
                snprintf(detail, detail_size, "%s",
                         g_password[0] ? "password saved" : "asks for the password");
            }
            break;
        case MENU_AUTOLOGIN:
            snprintf(out, out_size, "Log in on connect");
            snprintf(detail, detail_size, "%s",
                     g_autologin ? "on -- player as soon as it connects"
                                 : "off -- arrives watching, log in by hand");
            break;
        case MENU_FORGET_PASSWORD:
            snprintf(out, out_size, "Forget the saved password");
            snprintf(detail, detail_size, "%s",
                     g_password[0] ? "stored in the clear on the SD card" : "nothing saved");
            break;
        case MENU_PROFILE:
            snprintf(out, out_size, "Stream quality");
            snprintf(detail, detail_size, "%s%s  (%.0f fps decoded)",
                     PROFILES[g_profile].label,
                     g_adapt_enabled ? ", auto step-down" : "", g_measured_fps);
            break;
        case MENU_BITRATE:
            snprintf(out, out_size, "Bitrate");
            if (BITRATES[g_bitrate_index]) {
                snprintf(detail, detail_size, "%d kbps", BITRATES[g_bitrate_index]);
            } else {
                snprintf(detail, detail_size, "auto (%d kbps)", PROFILES[g_profile].kbps);
            }
            break;
        case MENU_CODEC:
            snprintf(out, out_size, "Video codec");
            snprintf(detail, detail_size, "%s",
                     g_codec == VIDEO_CODEC_H264 ? "H.264 (hardware decode)" : "VP8");
            break;
        case MENU_LSTICK_DIAG:
        case MENU_RSTICK_DIAG: {
            const int stick = (index == MENU_LSTICK_DIAG) ? 0 : 1;
            int diag = 0;
            input_get_stick_limits(stick, NULL, NULL, &diag);
            snprintf(out, out_size, "%s stick diagonals", stick == 0 ? "Left" : "Right");
            snprintf(detail, detail_size, "%d%% counts as a full corner", diag);
            break;
        }
        case MENU_LSTICK_DEAD:
        case MENU_RSTICK_DEAD: {
            const int stick = (index == MENU_LSTICK_DEAD) ? 0 : 1;
            int dead = 0;
            input_get_stick_limits(stick, &dead, NULL, NULL);
            snprintf(out, out_size, "%s stick deadzone", stick == 0 ? "Left" : "Right");
            snprintf(detail, detail_size, "%d%% -- ignored around centre", dead);
            break;
        }
        case MENU_LSTICK_RANGE:
        case MENU_RSTICK_RANGE: {
            const int stick = (index == MENU_LSTICK_RANGE) ? 0 : 1;
            int sat = 0;
            input_get_stick_limits(stick, NULL, &sat, NULL);
            snprintf(out, out_size, "%s stick range", stick == 0 ? "Left" : "Right");
            snprintf(detail, detail_size, "%d%% counts as fully pushed", sat);
            break;
        }
        case MENU_PADTEST:
            snprintf(out, out_size, "Controller test");
            snprintf(detail, detail_size, "check every button and stick");
            break;
        case MENU_TOUCHPAD:
            snprintf(out, out_size, vpad_enabled() ? "On-screen pad: on" : "On-screen pad: off");
            snprintf(detail, detail_size, "%s",
                     vpad_enabled() ? "play with the screen, alongside the sticks"
                                    : "nothing drawn, nothing read");
            break;
        case MENU_TOUCH_EDIT:
            snprintf(out, out_size, vpad_editing() ? "Done -- save the positions"
                                                   : "Move the buttons");
            snprintf(detail, detail_size, "%s",
                     vpad_editing() ? "press to finish and keep them"
                                    : "drag each one where your thumb reaches");
            break;
        case MENU_TOUCH_COLOUR:
            snprintf(out, out_size, "Button colour");
            snprintf(detail, detail_size, "%s", vpad_colour_name());
            break;
        case MENU_TOUCH_OPACITY:
            snprintf(out, out_size, "Button opacity");
            snprintf(detail, detail_size, "%d%%", vpad_opacity());
            break;
        case MENU_TOUCH_RESET:
            snprintf(out, out_size, "Reset the button positions");
            snprintf(detail, detail_size, "back to the default layout");
            break;
        case MENU_INFO_LINE:
            snprintf(out, out_size, g_show_info ? "Hide the on-screen notices"
                                                : "Show the on-screen notices");
            snprintf(detail, detail_size, "%s",
                     g_show_info ? "warns when the controller is not being sent"
                                 : "nothing written over the picture");
            break;
        case MENU_BRIGHTNESS:
        case MENU_CONTRAST:
        case MENU_SATURATION:
        case MENU_HUE: {
            int b = 0, c = 0, sat = 0, hue = 0;
            video_get_adjust(&b, &c, &sat, &hue);
            if (index == MENU_BRIGHTNESS) {
                snprintf(out, out_size, "Brightness");
                snprintf(detail, detail_size, "%d%%   (free)", b);
            } else if (index == MENU_CONTRAST) {
                snprintf(out, out_size, "Contrast");
                snprintf(detail, detail_size, "%d%%   (free)", c);
            } else if (index == MENU_SATURATION) {
                snprintf(out, out_size, "Saturation");
                snprintf(detail, detail_size, "%d%%%s", sat,
                         (sat == 100 && hue == 0) ? "   (free)" : "   (costs a little)");
            } else {
                snprintf(out, out_size, "Hue");
                snprintf(detail, detail_size, "%d deg%s", hue,
                         (sat == 100 && hue == 0) ? "   (free)" : "   (costs a little)");
            }
            break;
        }
        case MENU_PICTURE_RESET:
            snprintf(out, out_size, "Reset the picture");
            snprintf(detail, detail_size, "back to untouched");
            break;
        case MENU_DIAGNOSTICS:
            snprintf(out, out_size, g_show_diagnostics ? "Hide the diagnostics"
                                                       : "Show the diagnostics");
            snprintf(detail, detail_size, "%s",
                     g_show_diagnostics ? "measuring the link and the frame rate"
                                        : "nothing measured, full-height menu");
            break;
        case MENU_HOME:
            snprintf(out, out_size, "Send HOME to the console");
            snprintf(detail, detail_size, "or hold Start+Select for 1s");
            break;

        case MENU_WAKE:
            snprintf(out, out_size, "Wake the console");
            snprintf(detail, detail_size, "%s",
                     net_info()->may_control ? "powers it on and waits for a picture"
                                             : "players only");
            break;
        case MENU_RESET_DONGLE:
            snprintf(out, out_size, "Reset the adapter");
            snprintf(detail, detail_size, "%s",
                     net_info()->may_control ? "fixes input lag after a wake"
                                             : "players only");
            break;
        case MENU_RESTART_HOST:
            snprintf(out, out_size, "Restart the host");
            snprintf(detail, detail_size, "%s",
                     net_info()->may_control ? "brings the sound back when it stops"
                                             : "players only");
            break;
        case MENU_MUTE:
            snprintf(out, out_size, audio_is_muted() ? "Unmute" : "Mute");
            break;
        case MENU_VOLUME_DOWN:
            snprintf(out, out_size, "Volume down");
            snprintf(detail, detail_size, "%d%%", audio_volume());
            break;
        case MENU_VOLUME_UP:
            snprintf(out, out_size, "Volume up");
            snprintf(detail, detail_size, "%d%%", audio_volume());
            break;
        case MENU_QUIT:
            snprintf(out, out_size, "Quit");
            snprintf(detail, detail_size, "or hold Start+Select for 5s");
            break;
        default:
            snprintf(out, out_size, "?");
            break;
    }
}

/* Logs in the moment the stream connects, when that has been asked for.
 * Once per connection: a failure clears the saved password, and a
 * success reconnects with the token, so neither outcome loops. */
static void maybe_autologin(void) {
    const NetInfo *n = net_info();
    if (!g_autologin || !g_password[0] || n->may_control) {
        return;
    }
    char error[96];
    if (net_login(g_host, g_web_port, g_password, g_token, sizeof(g_token), error, sizeof(error))) {
        snprintf(g_login_message, sizeof(g_login_message), "logged in");
        net_connect(g_host, g_port, g_token);
    } else {
        snprintf(g_login_message, sizeof(g_login_message), "%s", error);
        /* A saved password the host no longer accepts would fail on every
         * connection with no way to type a new one. */
        g_password[0] = '\0';
        settings_changed();
    }
}

static void start_decoders_if_needed(void) {
    const NetInfo *n = net_info();
    if (n->state != NET_CONNECTED || g_decoders_ready) {
        return;
    }
    /* Only now is the stream's shape known: the handshake carries it,
     * codec included. Opening the decoder for the codec chosen in the
     * menu rather than the one actually arriving would decode an H.264
     * stream as VP8 whenever the two disagreed; the preference is asked
     * for below instead, and applied when the host announces it. */
    if (video_init(g_renderer, n->width, n->height,
                   n->video_codec ? n->video_codec : VIDEO_CODEC_VP8) == 0 &&
        audio_init(n->audio_rate ? n->audio_rate : 48000,
                   n->audio_channels ? n->audio_channels : 2) == 0) {
        g_decoders_ready = 1;
        g_await_keyframe = 1;
        /* The audio module only exists now, so the level read from the
         * config file is applied here rather than at load time. */
        audio_set_volume(g_saved_volume);
        audio_set_muted(g_saved_muted);
        printf("decoders ready: %ux%u codec %u, %u Hz x%u\n", n->width, n->height,
               n->video_codec, n->audio_rate, n->audio_channels);
        /* The codec is asked for, and nothing else.
         *
         * It is this console's own choice: the host runs a VP8 chain
         * and an H.264 one, and this says which of the two to send
         * here. Nobody else's picture moves.
         *
         * The size, the rate and the bitrate are NOT asked for. Those
         * belong to everyone on the same codec, and asking on
         * connection meant that starting this client changed the stream
         * for people already watching it, to whatever was in this
         * console's config file. The host announces the group's
         * settings instead (C2S_MSG_SHARED) and the menu moves to them;
         * changing one by hand afterwards still changes it for that
         * group. */
        if (n->video_codec != g_codec) {
            net_send_codec(g_codec);
        }
    } else {
        printf("decoders failed to start\n");
    }
}

/* Steps the stream down when this console cannot decode what it asked
 * for.
 *
 * A client that cannot keep up falls behind, the host starts skipping
 * frames for it, and skipping a predictive codec is far worse than
 * simply receiving fewer -- so the answer is to receive fewer. Measured
 * over a window rather than reacted to per frame, since a single slow
 * frame means nothing. Only ever steps DOWN on its own: stepping back up
 * would oscillate, and the menu is there to try a higher one. */
static void adapt_profile(void) {
    if (!g_decoders_ready || !g_adapt_enabled) {
        return;
    }
    static Uint32 window_started = 0;
    static unsigned long decoded_at_start = 0;

    Uint32 now = SDL_GetTicks();
    unsigned long decoded = 0, failed = 0;
    video_stats(&decoded, &failed);

    if (window_started == 0) {
        window_started = now;
        decoded_at_start = decoded;
        return;
    }
    if (now - window_started < ADAPT_WINDOW_MS) {
        return;
    }

    double achieved = (decoded - decoded_at_start) * 1000.0 / (now - window_started);
    window_started = now;
    decoded_at_start = decoded;

    int wanted = PROFILES[g_profile].fps;
    if (achieved < wanted * 0.7 && g_profile > 0) {
        g_profile--;
        snprintf(g_login_message, sizeof(g_login_message),
                 "only %.0f of %d fps: dropping to %s", achieved, wanted,
                 PROFILES[g_profile].label);
        printf("adapt: %s\n", g_login_message);
        net_send_profile(PROFILES[g_profile].w, PROFILES[g_profile].h,
                         PROFILES[g_profile].fps, current_bitrate());
        net_send_codec(g_codec);
    }
    g_measured_fps = achieved;
}

/* Moves this console's menu to the settings the host says everyone is
 * sharing.
 *
 * The entries here are indices into fixed tables rather than free
 * numbers, so "adopt" means picking the closest one -- which is honest:
 * the menu then shows the nearest thing it can express to what is
 * actually being received, instead of a value nobody is watching.
 *
 * Applied to the variables directly and never by way of the menu's own
 * actions, which send the value back to the host: a value echoed back
 * is one two clients can bounce between them indefinitely. */
static void adopt_shared(const C2sShared *sh) {
    if (!sh) return;

    if (sh->video_codec == VIDEO_CODEC_VP8 || sh->video_codec == VIDEO_CODEC_H264) {
        g_codec = sh->video_codec;
    }

    if (sh->width > 0 && sh->height > 0 && sh->fps > 0) {
        int best = -1, best_cost = 0;
        for (int i = 0; i < PROFILE_COUNT; i++) {
            /* Height first, frame rate second: a 720p menu entry
             * describes a 720p stream better than a 480p one does,
             * whatever the rate says. */
            int cost = abs(PROFILES[i].h - (int)sh->height) * 10
                     + abs(PROFILES[i].fps - (int)sh->fps);
            if (best < 0 || cost < best_cost) { best = i; best_cost = cost; }
        }
        if (best >= 0) g_profile = best;
    }

    if (sh->bitrate_kbps > 0) {
        int best = 0, best_cost = -1;
        for (int i = 0; i < BITRATE_COUNT; i++) {
            if (BITRATES[i] == 0) continue; /* "whatever the profile says" */
            int cost = abs(BITRATES[i] - (int)sh->bitrate_kbps);
            if (best_cost < 0 || cost < best_cost) { best = i; best_cost = cost; }
        }
        g_bitrate_index = best;
    }
}

static void stop_decoders(void) {
    if (g_decoders_ready) {
        audio_exit();
        video_exit();
        g_decoders_ready = 0;
    }
}

static void menu_activate(int index) {
    const NetInfo *n = net_info();
    /* Which entries change something worth remembering. Written once at
     * the end rather than in each case, so a new setting cannot be added
     * and quietly not persisted. */
    int persists = (index == MENU_HOST || index == MENU_PORT || index == MENU_PROFILE || index == MENU_BITRATE || index == MENU_CODEC ||
                    index == MENU_MUTE || index == MENU_VOLUME_DOWN || index == MENU_VOLUME_UP ||
                    index == MENU_LOGIN || index == MENU_FORGET_PASSWORD ||
                    index == MENU_AUTOLOGIN || index == MENU_DIAGNOSTICS ||
                    index == MENU_BRIGHTNESS || index == MENU_CONTRAST ||
                    index == MENU_SATURATION || index == MENU_HUE ||
                    index == MENU_PICTURE_RESET ||
                    index == MENU_TOUCHPAD || index == MENU_TOUCH_EDIT ||
                    index == MENU_TOUCH_COLOUR || index == MENU_TOUCH_OPACITY ||
                    index == MENU_TOUCH_RESET || index == MENU_INFO_LINE ||
                    index == MENU_LSTICK_DEAD || index == MENU_LSTICK_RANGE ||
                    index == MENU_LSTICK_DIAG || index == MENU_RSTICK_DEAD ||
                    index == MENU_RSTICK_RANGE || index == MENU_RSTICK_DIAG);
    switch (index) {
        case MENU_CONNECT:
            if (n->state == NET_CONNECTED || n->state == NET_CONNECTING) {
                net_disconnect();
                stop_decoders();
            } else {
                /* Always as a viewer. Being a player is something you
                 * press, not something that happens because a password
                 * was remembered. */
                net_connect(g_host, g_port, NULL);
            }
            break;
        case MENU_HOST:
        case MENU_PORT: {
            /* Typed on the console's own keyboard. The address and the
             * port were only editable by taking the SD card out and
             * opening a text file, which is a poor way to move a stream
             * to another port. */
            char typed[64] = {0};
            SwkbdConfig kbd;
            if (R_FAILED(swkbdCreate(&kbd, 0))) {
                snprintf(g_login_message, sizeof(g_login_message), "keyboard unavailable");
                break;
            }
            swkbdConfigMakePresetDefault(&kbd);
            if (index == MENU_PORT) {
                swkbdConfigSetType(&kbd, SwkbdType_NumPad);
                swkbdConfigSetHeaderText(&kbd, "Host port");
                swkbdConfigSetGuideText(&kbd, "SWITCH_PORT on the host; 5081 by default");
                snprintf(typed, sizeof(typed), "%u", g_port);
            } else {
                swkbdConfigSetHeaderText(&kbd, "Host address");
                swkbdConfigSetGuideText(&kbd, "the IP of the machine running capture2cloud");
                snprintf(typed, sizeof(typed), "%s", g_host);
            }
            swkbdConfigSetInitialText(&kbd, typed);
            swkbdConfigSetStringLenMax(&kbd, index == MENU_PORT ? 5 : (int)sizeof(g_host) - 1);
            Result rc = swkbdShow(&kbd, typed, sizeof(typed));
            swkbdClose(&kbd);
            if (R_FAILED(rc) || typed[0] == '\0') {
                break; /* cancelled */
            }

            if (index == MENU_PORT) {
                const int p = atoi(typed);
                if (p < 1 || p > 65535) {
                    snprintf(g_login_message, sizeof(g_login_message), "port out of range");
                    break;
                }
                g_port = (uint16_t)p;
            } else {
                snprintf(g_host, sizeof(g_host), "%s", typed);
            }
            /* Reconnected straight away: changing where to knock and
             * then having to remember to knock is a step nobody wants
             * for its own sake. */
            if (n->state != NET_IDLE) {
                net_connect(g_host, g_port, NULL);
            }
            snprintf(g_login_message, sizeof(g_login_message), "%s:%u", g_host, g_port);
            break;
        }
        case MENU_LOGIN: {
            /* Connecting never carries the token, so this is the only
             * way to become a player: asked for, not assumed. The
             * password being remembered saves the typing, not the
             * decision. */
            if (n->may_control) {
                g_token[0] = '\0';
                net_connect(g_host, g_port, NULL);
                snprintf(g_login_message, sizeof(g_login_message), "watching only");
                break;
            }

            if (g_password[0] == '\0') {
                /* The console's own keyboard, in password mode. Typing a
                 * 64-character token on it would be miserable, and the
                 * password is what the user actually knows. */
                SwkbdConfig kbd;
                if (R_FAILED(swkbdCreate(&kbd, 0))) {
                    snprintf(g_login_message, sizeof(g_login_message), "keyboard unavailable");
                    break;
                }
                swkbdConfigMakePresetDefault(&kbd);
                swkbdConfigSetHeaderText(&kbd, "capture2cloud password");
                swkbdConfigSetGuideText(&kbd, "PLAYER_PASSWORD from the host's scripts/.env");
                swkbdConfigSetStringLenMax(&kbd, sizeof(g_password) - 1);
                swkbdConfigSetPasswordFlag(&kbd, 1);
                Result rc = swkbdShow(&kbd, g_password, sizeof(g_password));
                swkbdClose(&kbd);
                if (R_FAILED(rc) || g_password[0] == '\0') {
                    g_password[0] = '\0';
                    break; /* cancelled */
                }
            }

            char error[96];
            if (net_login(g_host, g_web_port, g_password, g_token, sizeof(g_token),
                          error, sizeof(error))) {
                snprintf(g_login_message, sizeof(g_login_message), "logged in");
                /* Reconnect so the host re-reads who we are: control is
                 * decided once, in the handshake. */
                net_connect(g_host, g_port, g_token);
            } else {
                /* A saved password the host no longer accepts is worse
                 * than none: it would fail silently every time with no
                 * way to type a new one. */
                snprintf(g_login_message, sizeof(g_login_message), "%s", error);
                g_password[0] = '\0';
            }
            break;
        }
        case MENU_AUTOLOGIN:
            g_autologin = !g_autologin;
            break;
        case MENU_FORGET_PASSWORD:
            memset(g_password, 0, sizeof(g_password));
            snprintf(g_login_message, sizeof(g_login_message), "password forgotten");
            break;
        case MENU_PROFILE:
            /* Choosing by hand turns the automatic step-down off: it
             * would otherwise undo the choice within seconds and the
             * menu would appear not to work. */
            g_adapt_enabled = 0;
            g_profile = (g_profile + 1) % PROFILE_COUNT;
            net_send_profile(PROFILES[g_profile].w, PROFILES[g_profile].h,
                             PROFILES[g_profile].fps, current_bitrate());
            break;
        case MENU_BITRATE:
            g_bitrate_index = (g_bitrate_index + 1) % BITRATE_COUNT;
            net_send_profile(PROFILES[g_profile].w, PROFILES[g_profile].h,
                             PROFILES[g_profile].fps, current_bitrate());
            break;
        case MENU_CODEC:
            g_codec = (g_codec == VIDEO_CODEC_H264) ? VIDEO_CODEC_VP8 : VIDEO_CODEC_H264;
            net_send_codec(g_codec);
            /* The decoder is rebuilt when the host announces the change,
             * not here: the switch takes effect some frames later. */
            break;
        case MENU_LSTICK_DEAD:
        case MENU_RSTICK_DEAD: {
            const int stick = (index == MENU_LSTICK_DEAD) ? 0 : 1;
            int dead = 0, sat = 0, diag = 0;
            input_get_stick_limits(stick, &dead, &sat, &diag);
            dead = next_in(DEADZONES, DEADZONE_COUNT, dead);
            input_set_stick_limits(stick, dead, sat, diag);
            break;
        }
        case MENU_LSTICK_RANGE:
        case MENU_RSTICK_RANGE: {
            const int stick = (index == MENU_LSTICK_RANGE) ? 0 : 1;
            int dead = 0, sat = 0, diag = 0;
            input_get_stick_limits(stick, &dead, &sat, &diag);
            sat = next_in(SATURATIONS, SATURATION_COUNT, sat);
            input_set_stick_limits(stick, dead, sat, diag);
            break;
        }
        case MENU_LSTICK_DIAG:
        case MENU_RSTICK_DIAG: {
            const int stick = (index == MENU_LSTICK_DIAG) ? 0 : 1;
            int dead = 0, sat = 0, diag = 0;
            input_get_stick_limits(stick, &dead, &sat, &diag);
            diag = next_in(SATURATIONS, SATURATION_COUNT, diag);
            input_set_stick_limits(stick, dead, sat, diag);
            break;
        }
        case MENU_PADTEST:
            /* Straight to the test: this is where the effect of the two
             * settings above is visible, and tuning them blind is the
             * only other option. */
            g_screen = SCREEN_PADTEST;
            break;
        case MENU_TOUCHPAD:
            vpad_set_enabled(!vpad_enabled());
            break;
        case MENU_TOUCH_EDIT:
            if (vpad_editing()) {
                /* Pressed a second time: done, and kept. Written now
                 * rather than left to the delayed save, because "I have
                 * finished arranging these" deserves to be true the
                 * moment it is said. */
                vpad_set_editing(0);
                save_config();
                snprintf(g_login_message, sizeof(g_login_message), "button positions saved");
                break;
            }
            /* Moving them only makes sense while they are on screen, and
             * turning the pad on is one press away. */
            if (!vpad_enabled()) {
                vpad_set_enabled(1);
            }
            vpad_set_editing(1);
            /* Out of the way: the buttons being dragged are under the
             * menu. */
            g_screen = SCREEN_STREAM;
            snprintf(g_login_message, sizeof(g_login_message),
                     "drag the buttons; L3+R3 then \"Done\" when finished");
            break;
        case MENU_TOUCH_COLOUR:
            /* Cycled rather than picked: white is invisible on a bright
             * scene and there is no colour that works against every
             * one, so the useful thing is to try the next one. */
            vpad_set_colour(vpad_colour() + 1);
            if (!vpad_enabled()) vpad_set_enabled(1);
            break;
        case MENU_TOUCH_OPACITY: {
            int next = vpad_opacity() - 20;
            if (next < 20) next = 100;
            vpad_set_opacity(next);
            if (!vpad_enabled()) vpad_set_enabled(1);
            break;
        }
        case MENU_TOUCH_RESET:
            vpad_reset_layout();
            snprintf(g_login_message, sizeof(g_login_message), "button positions reset");
            break;
        case MENU_INFO_LINE:
            g_show_info = !g_show_info;
            break;
        case MENU_BRIGHTNESS:
        case MENU_CONTRAST:
        case MENU_SATURATION:
        case MENU_HUE: {
            int b = 0, c = 0, sat = 0, hue = 0;
            video_get_adjust(&b, &c, &sat, &hue);
            /* Cycled in steps, wrapping at the end of the range: there
             * is no slider to drag on a screen driven by a d-pad, and
             * wrapping is one press away from the other end. */
            if (index == MENU_BRIGHTNESS)      b = (b >= 150) ? 50 : b + 10;
            else if (index == MENU_CONTRAST)   c = (c >= 150) ? 50 : c + 10;
            else if (index == MENU_SATURATION) sat = (sat >= 200) ? 0 : sat + 20;
            else                               hue = (hue >= 150) ? -180 : hue + 30;
            video_set_adjust(b, c, sat, hue);
            /* Out of the way, since the point is to look at the
             * picture. */
            g_screen = SCREEN_STREAM;
            break;
        }
        case MENU_PICTURE_RESET:
            video_set_adjust(100, 100, 100, 0);
            snprintf(g_login_message, sizeof(g_login_message), "picture reset");
            break;
        case MENU_DIAGNOSTICS:
            /* Hiding them stops the measuring as well. A panel nobody is
             * looking at should not be costing anything to fill in. */
            g_show_diagnostics = !g_show_diagnostics;
            g_link_mbps = 0.0;
            g_draw_fps = 0.0;
            g_window_bytes = 0;
            g_window_draws = 0;
            g_window_since = 0;
            break;
        /* Quit is handled below rather than here so the reason it is
         * leaving reaches the log before anything is torn down. */
        case MENU_HOME:
            net_send_home();
            /* The point of pressing it was to reach the console, so get
             * out of the way and show what happened. */
            if (n->state == NET_CONNECTED) g_screen = SCREEN_STREAM;
            break;
        case MENU_WAKE:
            /* The console this client exists to show may well be asleep,
             * and there is no browser here to press the page's button. */
            net_send_wake();
            snprintf(g_login_message, sizeof(g_login_message),
                     n->may_control ? "waking the console..." : "log in to wake the console");
            break;
        case MENU_RESET_DONGLE:
            net_send_reset_dongle();
            snprintf(g_login_message, sizeof(g_login_message),
                     n->may_control ? "resetting the adapter..." : "log in to reset the adapter");
            break;
        case MENU_RESTART_HOST:
            /* The stream goes away and comes back on its own: the
             * reconnect loop is already there for a host that was
             * switched off, and this is the same thing for a few
             * seconds. */
            net_send_restart();
            snprintf(g_login_message, sizeof(g_login_message),
                     n->may_control ? "restarting the host..." : "log in to restart the host");
            break;
        case MENU_MUTE:
            audio_set_muted(!audio_is_muted());
            break;
        case MENU_VOLUME_DOWN:
            audio_set_volume(audio_volume() - VOLUME_STEP);
            break;
        case MENU_VOLUME_UP:
            audio_set_volume(audio_volume() + VOLUME_STEP);
            break;
        case MENU_QUIT:
            /* Saved before leaving: whatever was changed this session
             * would otherwise be lost by the one action that guarantees
             * there is no later chance to write it. */
            save_config();
            printf("menu: quit requested\n");
            g_running = 0;
            break;
        default:
            break;
    }

    if (persists) {
        settings_changed();
    }
}

/* --- drawing --------------------------------------------------------- */

/* The menu draws the stream underneath itself and veils it. */
static void draw_stream(void);

#define MENU_TOP 104
#define MENU_ROW_H 44
#define DIAG_HEIGHT 132
#define MENU_LEFT 56
#define MENU_WIDTH 620


static void draw_status_line(void) {
    const NetInfo *n = net_info();
    SDL_Color colour = COL_DIM;
    char text[160];

    switch (n->state) {
        case NET_CONNECTED: {
            unsigned long dec = 0, failed = 0;
            video_stats(&dec, &failed);
            colour = n->may_control ? COL_GOOD : COL_ACCENT;
            snprintf(text, sizeof(text), "%ux%u  %s  %lu frames%s",
                     n->width, n->height,
                     n->may_control ? "player" : "viewer (input ignored)",
                     dec, failed ? "  (some dropped)" : "");
            break;
        }
        case NET_CONNECTING:
            colour = COL_ACCENT;
            snprintf(text, sizeof(text), "%s", n->status);
            break;
        case NET_FAILED:
            colour = COL_BAD;
            snprintf(text, sizeof(text), "%s", n->status);
            break;
        default:
            snprintf(text, sizeof(text), "not connected");
            break;
    }
    ui_text(g_renderer, UI_FONT_SMALL, 60, SCREEN_H - 52, colour, text);

    /* Said plainly, over the picture, because the alternative is
     * pressing buttons at a console that is ignoring them with nothing
     * on screen to explain why. Connecting is always as a viewer -- the
     * password being remembered saves the typing, not the decision -- so
     * this is the normal state on every launch until someone asks for
     * control. */
    if (g_show_info && n->state == NET_CONNECTED && !n->may_control &&
        g_screen == SCREEN_STREAM) {
        const char *how = g_password[0] ? "L3+R3, then \"Log in as player\""
                                        : "L3+R3, then \"Log in as player\" (asks for the password)";
        ui_fill(g_renderer, 0, SCREEN_H - 96, SCREEN_W, 34, (SDL_Color){10, 11, 15, 200});
        char hint[160];
        snprintf(hint, sizeof(hint), "watching only -- the controller is not being sent.  %s", how);
        ui_text(g_renderer, UI_FONT_SMALL, 60, SCREEN_H - 88, COL_ACCENT, hint);
    }
}

/* Everything needed to work out why a connection is not happening,
 * without a shell on this console: how far it got, what the kernel said,
 * and whether any bytes moved in either direction. */
static void draw_diagnostics(void) {
    const NetInfo *n = net_info();
    char line[320];
    const int y = SCREEN_H - DIAG_HEIGHT + 8;

    /* Its own panel: the strip below the list belongs to it, so nothing
     * can grow into it. */
    ui_fill(g_renderer, 0, SCREEN_H - DIAG_HEIGHT, SCREEN_W, DIAG_HEIGHT,
            (SDL_Color){10, 11, 15, 230});

    if (g_login_message[0]) {
        ui_text(g_renderer, UI_FONT_SMALL, MENU_LEFT, y, COL_ACCENT, g_login_message);
    }

    char pads[64];
    input_describe_pads(pads, sizeof(pads));
    snprintf(line, sizeof(line), "host %s:%u   pad: %s  role: %s  input: %s%s",
             g_host, g_port, pads,
             n->may_control ? "player" : "viewer",
             (n->state == NET_CONNECTED && n->may_control) ? "sent to the console"
                                                           : "NOT sent",
             g_applet_mode ? "   [APPLET MODE: hold R on a game for the full machine]" : "");
    ui_text(g_renderer, UI_FONT_SMALL, MENU_LEFT, y + 24, COL_DIM, line);

    snprintf(line, sizeof(line), "decoder: %s", video_decoder_name());
    ui_text(g_renderer, UI_FONT_SMALL, MENU_LEFT, y + 48, COL_DIM, line);

    unsigned long vdec = 0, vfail = 0, adec = 0, afail = 0;
    video_stats(&vdec, &vfail);
    audio_stats(&adec, &afail);
    snprintf(line, sizeof(line),
             "link %.1f Mbit/s   drawing %.0f fps   video %lu ok / %lu bad / %lu late   audio %lu/%lu",
             g_link_mbps, g_draw_fps, vdec, vfail, g_video_dropped, adec, afail);
    ui_text(g_renderer, UI_FONT_SMALL, MENU_LEFT, y + 72, COL_DIM, line);

    ui_text(g_renderer, UI_FONT_SMALL, MENU_LEFT, y + 96, COL_DIM,
            "A: choose   B: stream   |   L3+R3: menu   |   Start+Select   1s: HOME   5s: quit");
}

/* The menu scrolls, and the diagnostics own the bottom of the screen.
 *
 * Entries were drawn from a fixed top with a fixed row height, so adding
 * any made the list grow down through the diagnostics and off the
 * screen. The list now lives in a region whose height is what is left
 * after the header and that reserved strip, and scrolls within it.
 *
 * Rows are shorter and the text smaller than they were: the list is in
 * front of something you are watching, and it used to cover most of it. */
static int menu_visible_rows(void) {
    int space = SCREEN_H - (g_show_diagnostics ? DIAG_HEIGHT : 24) - MENU_TOP;
    int rows = space / MENU_ROW_H;
    return rows < 1 ? 1 : rows;
}

/* How many entries the current level has. */
static int menu_length(void) {
    return (g_category < 0) ? CATEGORY_COUNT + PINNED_ROWS : CATEGORIES[g_category].count;
}

/* Keeps the selection on screen, scrolling only when it would otherwise
 * fall outside -- so the list stays still while moving through the
 * middle of it. */
static int menu_scroll_offset(void) {
    static int offset = 0;
    const int rows = menu_visible_rows();
    const int count = menu_length();
    if (g_menu_index < offset) {
        offset = g_menu_index;
    } else if (g_menu_index >= offset + rows) {
        offset = g_menu_index - rows + 1;
    }
    if (offset > count - rows) offset = count - rows;
    if (offset < 0) offset = 0;
    return offset;
}

/* Where a row is drawn, so a finger can be tested against the same
 * rectangle the eye is aiming at. */
static SDL_Rect menu_row_rect(int row) {
    SDL_Rect rect = {MENU_LEFT, MENU_TOP + row * MENU_ROW_H, MENU_WIDTH, MENU_ROW_H - 6};
    return rect;
}

/* The button beside the title. Inside a category it goes back to the
 * category list; at the root it closes the menu and returns to the
 * stream -- which by finger there was otherwise no way to do at all,
 * since the only way out was a button combination. */
static const char *menu_back_label(void) {
    return g_category < 0 ? "< back to the game" : "< back";
}

/* Sized from its own text rather than fixed, because the two labels are
 * not the same length and the longer one ran out of its box. Measured in
 * one place so the drawing and the finger agree on where it is, and the
 * title is placed after whatever it comes to. */
static SDL_Rect menu_back_rect(void) {
    const int padding = 28;
    SDL_Rect rect = {MENU_LEFT, 40, ui_text_width(UI_FONT_SMALL, menu_back_label()) + padding, 40};
    return rect;
}

/* What the row at this level says. */
static void menu_row_text(int i, char *label, size_t label_size, char *detail, size_t detail_size) {
    detail[0] = '\0';
    if (g_category < 0) {
        if (i < PINNED_ROWS) {
            menu_label(MENU_LOGIN, label, label_size, detail, detail_size);
            return;
        }
        snprintf(label, label_size, "%s", CATEGORIES[i - PINNED_ROWS].title);
        snprintf(detail, detail_size, "%d", CATEGORIES[i - PINNED_ROWS].count);
        return;
    }
    menu_label(CATEGORIES[g_category].items[i], label, label_size, detail, detail_size);
}

/* Entering a row: a category to open, or an action to take. */
static void menu_enter(int i) {
    if (g_category >= 0) {
        menu_activate(CATEGORIES[g_category].items[i]);
        return;
    }
    if (i < PINNED_ROWS) {
        menu_activate(MENU_LOGIN);
        return;
    }
    g_category = i - PINNED_ROWS;
    g_menu_index = 0;
}

/* Back out of a category, landing on the row that opened it. */
static void menu_leave_category(void) {
    if (g_category >= 0) {
        g_menu_index = g_category + PINNED_ROWS;
        g_category = -1;
    }
}

static void draw_menu(int over_stream) {
    if (over_stream) {
        /* The picture is left running underneath and only veiled: what
         * the console is doing while the menu is open is exactly what
         * most of these settings are being changed for. */
        draw_stream();
        ui_fill(g_renderer, 0, 0, SCREEN_W, SCREEN_H, COL_SCRIM);
    } else {
        SDL_SetRenderDrawColor(g_renderer, COL_BACKDROP.r, COL_BACKDROP.g, COL_BACKDROP.b, 255);
        SDL_RenderClear(g_renderer);
    }

    {
        const SDL_Rect back = menu_back_rect();
        ui_fill(g_renderer, back.x, back.y, back.w, back.h, COL_ROW);
        ui_outline(g_renderer, back.x, back.y, back.w, back.h, 2, COL_DIM);
        ui_text(g_renderer, UI_FONT_SMALL, back.x + 14, back.y + 11, COL_TEXT,
                menu_back_label());
        ui_text(g_renderer, UI_FONT_TITLE, back.x + back.w + 20, 34, COL_TEXT,
                g_category < 0 ? "Capture2Cloud client" : CATEGORIES[g_category].title);
    }

    const int rows = menu_visible_rows();
    const int offset = menu_scroll_offset();
    const int count = menu_length();

    for (int row = 0; row < rows && offset + row < count; row++) {
        const int i = offset + row;
        char label[96], detail[96];
        menu_row_text(i, label, sizeof(label), detail, sizeof(detail));

        const SDL_Rect rect = menu_row_rect(row);
        const int selected = (i == g_menu_index);
        const int pinned = (g_category < 0 && i < PINNED_ROWS);

        /* Green while it would make you a player, and plain once you
         * are one: the colour is there to be noticed when it matters,
         * not to stay loud afterwards. */
        const int offers_control = pinned && !net_info()->may_control;
        SDL_Color row_colour = selected ? COL_SELECTED : COL_ROW;
        if (offers_control) {
            row_colour = selected ? (SDL_Color){40, 130, 80, 235}
                                  : (SDL_Color){32, 96, 62, 200};
        }
        ui_fill(g_renderer, rect.x, rect.y, rect.w, rect.h, row_colour);
        if (pinned) {
            ui_outline(g_renderer, rect.x, rect.y, rect.w, rect.h, 2,
                       offers_control ? COL_GOOD : COL_DIM);
        }
        ui_text(g_renderer, UI_FONT_BODY, rect.x + 18, rect.y + 9,
                offers_control ? COL_GOOD : COL_TEXT, label);
        if (detail[0]) {
            const int w = ui_text_width(UI_FONT_SMALL, detail);
            ui_text(g_renderer, UI_FONT_SMALL, rect.x + rect.w - 18 - w, rect.y + 13,
                    selected ? COL_TEXT : COL_DIM, detail);
        }
    }

    /* Says there is more in each direction, so a list taller than the
     * screen does not simply appear to end. */
    if (offset > 0) {
        ui_text(g_renderer, UI_FONT_SMALL, MENU_LEFT + MENU_WIDTH / 2, MENU_TOP - 20, COL_DIM, "^");
    }
    if (offset + rows < count) {
        ui_text(g_renderer, UI_FONT_SMALL, MENU_LEFT + MENU_WIDTH / 2,
                MENU_TOP + rows * MENU_ROW_H, COL_DIM, "v");
    }

    if (g_show_diagnostics) {
        draw_diagnostics();
    }
}

/* A tap while the menu is open. Returns 1 when it landed on something.
 *
 * The same rectangles the drawing uses, so what is aimed at is what is
 * hit -- and the console is a tablet, which makes reaching for the glass
 * the obvious thing to do long before reaching for a stick. */
static int menu_touch(float nx, float ny) {
    const int x = (int)(nx * SCREEN_W), y = (int)(ny * SCREEN_H);

    {
        const SDL_Rect back = menu_back_rect();
        if (x >= back.x && x < back.x + back.w && y >= back.y && y < back.y + back.h) {
            if (g_category >= 0) {
                menu_leave_category();
            } else {
                g_screen = SCREEN_STREAM;
            }
            return 1;
        }
    }

    const int rows = menu_visible_rows();
    const int offset = menu_scroll_offset();
    const int count = menu_length();
    for (int row = 0; row < rows && offset + row < count; row++) {
        const SDL_Rect rect = menu_row_rect(row);
        if (x < rect.x || x >= rect.x + rect.w || y < rect.y || y >= rect.y + rect.h) {
            continue;
        }
        const int i = offset + row;
        /* Tapping a row selects it and acts on it in one go: a
         * touchscreen has no separate "confirm". */
        g_menu_index = i;
        menu_enter(i);
        return 1;
    }
    return 0;
}

static void draw_stream(void) {
    SDL_SetRenderDrawColor(g_renderer, COL_BACKDROP.r, COL_BACKDROP.g, COL_BACKDROP.b, 255);
    SDL_RenderClear(g_renderer);

    if (g_decoders_ready) {
        SDL_Rect full = {0, 0, SCREEN_W, SCREEN_H};
        video_draw(g_renderer, &full);
        return;
    }

    /* Connected but nothing decoded yet, or not connected at all: say
     * which, rather than showing a black screen that could be either. */
    const NetInfo *n = net_info();
    ui_text_centered(g_renderer, UI_FONT_BODY, SCREEN_W / 2, SCREEN_H / 2 - 20,
                     (SDL_Color){25, 27, 33, 255},
                     n->state == NET_CONNECTED ? "waiting for the first frame..." : n->status);
}

/* Every slot the client can send, drawn as a labelled bar. This is what
 * tells you whether a button reaches the console at all, so the labels
 * are the wire slot names rather than the Switch's own. */
static const char *SLOT_NAME[PAD_SLOT_COUNT] = {
    "GUIDE", "BACK", "START", "RB", "RT", "RS", "LB", "LT", "LS",
    "RX", "RY", "LX", "LY", "UP", "DOWN", "LEFT", "RIGHT", "Y", "B", "A", "X"
};

static void draw_padtest(const PadState21 pad) {
    SDL_SetRenderDrawColor(g_renderer, COL_BACKDROP.r, COL_BACKDROP.g, COL_BACKDROP.b, 255);
    SDL_RenderClear(g_renderer);

    ui_text(g_renderer, UI_FONT_TITLE, 60, 34, COL_TEXT, "Controller test");
    {
        /* This screen reads the controller and shows it, and sends
         * nothing. So a button that lights up here and does nothing in
         * the game is not a controller problem -- it is the host
         * refusing input from a viewer. Saying which is which here is
         * the difference between a five-second answer and an evening. */
        const NetInfo *n = net_info();
        char pads[64];
        input_describe_pads(pads, sizeof(pads));
        char line[200];
        snprintf(line, sizeof(line),
                 "reading: %s   |   in the game these would be %s",
                 pads,
                 (n->state == NET_CONNECTED && n->may_control)
                     ? "sent to the console"
                     : "IGNORED -- log in as player first");
        ui_text(g_renderer, UI_FONT_SMALL, 60, 76,
                (n->state == NET_CONNECTED && n->may_control) ? COL_GOOD : COL_ACCENT, line);
    }
    ui_text(g_renderer, UI_FONT_SMALL, 60, 100, COL_DIM,
            "Press anything. Values are what would be sent to the console. B returns to the menu.");

    const int cols = 7;
    const int cell_w = (SCREEN_W - 120) / cols;
    const int cell_h = 92;
    const int top = 150;

    for (int i = 0; i < PAD_SLOT_COUNT; i++) {
        int cx = 60 + (i % cols) * cell_w;
        int cy = top + (i / cols) * cell_h;
        int v = pad[i];
        int magnitude = v < 0 ? -v : v;

        ui_fill(g_renderer, cx, cy, cell_w - 12, cell_h - 14, COL_ROW);
        if (magnitude) {
            /* Width follows the value, so a stick reads as a sweep and a
             * button as a full bar. */
            int w = (cell_w - 12) * magnitude / 100;
            ui_fill(g_renderer, cx, cy, w, cell_h - 14, COL_SELECTED);
        }
        ui_text(g_renderer, UI_FONT_SMALL, cx + 10, cy + 8,
                magnitude ? COL_TEXT : COL_DIM, SLOT_NAME[i]);

        char value[16];
        snprintf(value, sizeof(value), "%d", v);
        ui_text(g_renderer, UI_FONT_BODY, cx + 10, cy + 34,
                magnitude ? COL_ACCENT : COL_DIM, value);
    }

    draw_status_line();
}

/* --- setup ----------------------------------------------------------- */

static int display_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    g_window = SDL_CreateWindow("capture2cloud client", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                SCREEN_W, SCREEN_H, 0);
    if (!g_window) {
        printf("SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        printf("SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    return 0;
}

static void display_exit(void) {
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

/* One frame, copied out of the receive buffer so the newest can be held
 * while the rest of what arrived is drained past it. Grows to whatever
 * the stream turns out to need rather than reserving for the worst
 * case. */
static uint8_t *g_keep = NULL;
static uint32_t g_keep_len = 0, g_keep_cap = 0;

static int keep_frame(const uint8_t *data, uint32_t size) {
    if (size > g_keep_cap) {
        uint8_t *bigger = realloc(g_keep, size);
        if (!bigger) {
            return 0;
        }
        g_keep = bigger;
        g_keep_cap = size;
    }
    memcpy(g_keep, data, size);
    g_keep_len = size;
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    socketInitializeDefault();
    nxlinkStdio(); /* stdout reaches nxlink when one is listening */

    if (display_init() != 0) {
        socketExit();
        return 1;
    }
    if (ui_init(g_renderer) != 0) {
        display_exit();
        socketExit();
        return 1;
    }
    if (input_init() != 0 || net_init() != 0) {
        ui_exit();
        display_exit();
        socketExit();
        return 1;
    }

    vpad_init();
    /* The glass is read wherever the sticks are read, at their rate. */
    input_set_poll_hook(vpad_poll_touches);
    input_set_merge_hook(vpad_merge);
    load_config();

    /* Album-launched homebrew runs as an applet: much less RAM, and the
     * HOME menu still resident behind it. Both matter for a software
     * video decoder, so it is reported rather than left to be guessed
     * at from the symptoms. */
    g_applet_mode = (appletGetAppletType() != AppletType_Application &&
                     appletGetAppletType() != AppletType_SystemApplication);
    printf("capture2cloud client: started%s\n",
           g_applet_mode ? " (APPLET MODE -- hold R on a game for the full machine)" : "");

    PadState21 pad;
    int was_connected = 0;

    while (g_running && appletMainLoop()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                g_running = 0;
            } else if (event.type == SDL_FINGERDOWN) {
                /* Only the menu is driven by events: a tap is an
                 * instant, and a missed release cannot leave a menu
                 * stuck. The on-screen pad reads the screen itself,
                 * where a finger that has gone is simply absent. */
                if (g_screen == SCREEN_MENU) {
                    menu_touch(event.tfinger.x, event.tfinger.y);
                } else if (!vpad_near_control(event.tfinger.x, event.tfinger.y)) {
                    /* Not near a control. The shoulders, triggers, minus,
                     * plus and HOME all sit along the top edge, which is
                     * also where a tap opens the menu -- so a thumb that
                     * missed one of them was opening the menu instead,
                     * which is a poor thing to get mid-game. */
                    input_feed_touch(event.tfinger.y, 1);
                }
            }
        }

        net_poll();

        /* On the edge into "connected", not while connected: this makes
         * one blocking HTTP request, and repeating it every frame would
         * freeze the picture. */
        const int now_connected = (net_info()->state == NET_CONNECTED);
        if (now_connected && !was_connected) {
            maybe_autologin();
        }
        was_connected = now_connected;

        start_decoders_if_needed();

        /* Everything that has arrived is decoded, and only the last
         * picture is the one left on screen.
         *
         * Keeping just the newest frame and throwing the rest away was
         * wrong, and wrong in a way that looked like a network problem:
         * TCP delivers in bursts, so two or three frames land in the
         * same pass even at a steady 60, and all but the last were
         * dropped. Both codecs predict from what came before, so every
         * one of those drops broke every frame after it until the next
         * keyframe a second later. What you saw was colour smearing out
         * of moving things while anything standing still slowly became
         * sharp again -- the picture rebuilding itself block by block
         * from a reference it never received.
         *
         * Decoding all of them is affordable now in a way it was not
         * when this ran on libvpx and three CPU cores: the video engine
         * does it. What is NOT affordable is falling behind and staying
         * there, so a pass that would decode an unreasonable number of
         * frames stops and jumps forward to the next keyframe instead --
         * a clean restart rather than a long tail of stale pictures.
         *
         * Audio is decoded as it comes: its packets are 5 ms, they cost
         * almost nothing, and a gap in sound is far more noticeable than
         * a late picture. */
        const uint8_t *payload;
        uint32_t payload_size;
        uint8_t payload_flags;
        int msg;
        int decoded_here = 0, skipped_here = 0, have_key = 0;
        while ((msg = net_take_frame(&payload, &payload_size, &payload_flags)) != 0) {
            if (msg == C2S_MSG_VIDEO) {
                const int is_key = (payload_flags & C2S_FLAG_KEYFRAME) != 0;
                g_video_frames++;
                g_video_bytes += payload_size;
                if (g_show_diagnostics) g_window_bytes += payload_size;

                /* A decoder that has just been built has nothing to
                 * predict from, so everything before the first keyframe
                 * is noise on screen. */
                if (g_await_keyframe) {
                    if (!is_key) {
                        continue;
                    }
                    g_await_keyframe = 0;
                }

                if (decoded_here < MAX_DECODE_PER_PASS) {
                    if (g_decoders_ready) {
                        video_decode(payload, payload_size);
                    }
                    decoded_here++;
                } else {
                    /* Genuinely behind. Nothing more is decoded this
                     * pass; the newest keyframe in what is left is kept
                     * and everything before it discarded. */
                    skipped_here++;
                    g_video_dropped++;
                    if (is_key && keep_frame(payload, payload_size)) {
                        have_key = 1;
                    }
                }
            } else if (msg == C2S_MSG_AUDIO) {
                g_audio_frames++;
                if (g_show_diagnostics) g_window_bytes += payload_size;
                if (g_decoders_ready) audio_decode(payload, payload_size);
            } else if (msg == C2S_MSG_SHARED && payload_size == sizeof(C2sShared)) {
                C2sShared sh;
                memcpy(&sh, payload, sizeof(sh));
                adopt_shared(&sh);
            } else if (msg == C2S_MSG_STREAM_INFO && payload_size == sizeof(C2sStreamInfo)) {
                /* The host saying what it is sending now. Rebuilding the
                 * decoder here rather than when the change was requested
                 * is what keeps it from decoding the tail of the old
                 * stream as though it were the new one. */
                C2sStreamInfo info;
                memcpy(&info, payload, sizeof(info));
                printf("stream: %ux%u codec %u\n", info.width, info.height, info.video_codec);
                video_exit();
                g_await_keyframe = 1;
                decoded_here = skipped_here = have_key = 0;
                if (video_init(g_renderer, info.width, info.height, info.video_codec) != 0) {
                    g_decoders_ready = 0;
                    snprintf(g_login_message, sizeof(g_login_message),
                             "no decoder for this stream");
                }
            }
        }
        if (skipped_here) {
            if (have_key && g_decoders_ready) {
                video_decode(g_keep, g_keep_len);
            } else {
                /* Nothing clean to resume from. Showing the last good
                 * picture until one arrives beats showing the smear. */
                net_send_keyframe_request();
                g_await_keyframe = 1;
            }
        }

        adapt_profile();

        /* Nothing binds to the pad while a local screen owns the
         * glass. */
        vpad_set_accepting(g_screen != SCREEN_MENU);

        input_read(pad);

        /* The gesture toggles the menu from anywhere, including the
         * controller test -- otherwise a stuck test screen would need a
         * button that the test itself is meant to be checking. */
        /* A tap at the top of the screen toggles the menu. Available
         * from anywhere, including the controller test -- a stuck test
         * screen must not depend on a button the test itself is meant
         * to be checking. */
        if (input_take_menu_gesture()) {
            g_screen = (g_screen == SCREEN_MENU) ? SCREEN_STREAM : SCREEN_MENU;
            g_menu_index = 0;
            g_category = -1;
            /* Whatever was pressed while playing is not a menu choice. */
            input_clear_presses();
            /* Editing is NOT cancelled here. Opening the menu is how
             * you get back to the entry that ends it, and cancelling on
             * the way would mean that entry could never be pressed a
             * second time -- so the mode would have no way out that
             * says "keep this". */
            vpad_touch_clear();
        }
        if (input_take_home_gesture()) {
            printf("Start+Select (1s) -> HOME on the remote console\n");
            net_send_home();
        }
        if (input_take_quit_gesture()) {
            save_config();
            printf("Start+Select (5s) -> quitting\n");
            g_running = 0;
        }

        /* Presses are counted by the sampling thread and drained here,
         * rather than found by comparing this frame with the last.
         *
         * The comparison only ever saw what a button was doing at the
         * two instants the frame loop happened to look, so a press and
         * release between two frames was invisible -- and the frame loop
         * slows down whenever there is more to decode. The menu stopped
         * answering exactly when the picture got busy, which is what
         * "the confirm button no longer works" was.
         *
         * The face buttons are named for what the player presses, not
         * for what a positional layout calls them: the bottom button is
         * cancel on this console, the right one is confirm. */
        const int confirm_presses = input_take_press(PAD_B);
        const int cancel_presses = input_take_press(PAD_A);
        const int up_presses = input_take_press(PAD_UP);
        const int down_presses = input_take_press(PAD_DOWN);

        if (g_screen == SCREEN_MENU) {
            const int count = menu_length();
            for (int i = 0; i < down_presses; i++) g_menu_index = (g_menu_index + 1) % count;
            for (int i = 0; i < up_presses; i++) g_menu_index = (g_menu_index + count - 1) % count;
            if (confirm_presses) {
                menu_enter(g_menu_index);
            }
            if (cancel_presses) {
                /* Out of the group first, then out of the menu: one
                 * button, and it always undoes the last step. */
                if (g_category >= 0) {
                    menu_leave_category();
                } else {
                    g_screen = SCREEN_STREAM;
                }
            }
            /* Navigating the menu must not also reach the game. */
            memset(pad, 0, sizeof(pad));
        } else if (g_screen == SCREEN_PADTEST) {
            if (cancel_presses) g_screen = SCREEN_MENU;
            /* The test shows what WOULD be sent; it does not send it, so
             * mashing buttons here never disturbs the console. */
        }

        if (g_screen == SCREEN_STREAM) {
            /* Not a menu: nothing is queued up waiting to be acted on
             * the next time one opens. */
            input_clear_presses();
        }

        /* The polling thread does the sending, at its own rate. This
         * only says whether it should: while a local screen is up, what
         * the player presses is for this console, not the remote one. */
        input_set_forwarding(g_screen == SCREEN_STREAM);

        switch (g_screen) {
            case SCREEN_PADTEST:
                draw_padtest(pad);
                break;
            case SCREEN_MENU:
                draw_menu(g_decoders_ready);
                break;
            default:
                draw_stream();
                vpad_draw(g_renderer, pad);
                break;
        }
        SDL_RenderPresent(g_renderer);

        /* Dragging a button is not a menu entry, so nothing tells the
         * settings it happened: the layout is watched instead. */
        {
            static unsigned last_layout = 0;
            const unsigned now_layout = vpad_layout_revision();
            if (now_layout != last_layout) {
                last_layout = now_layout;
                settings_changed();
            }
        }
        save_config_if_needed();

        /* Once a second: what the link actually delivered, and how many
         * pictures were actually drawn. Both are measured rather than
         * assumed -- a profile that is being asked for and a profile
         * that is arriving are different things, and the gap between
         * them is the whole diagnosis. */
        if (!g_show_diagnostics) {
            continue; /* nothing is displayed, so nothing is measured */
        }
        g_window_draws++;
        Uint32 now_ms = SDL_GetTicks();
        if (g_window_since == 0) {
            g_window_since = now_ms;
        } else if (now_ms - g_window_since >= 1000) {
            double secs = (now_ms - g_window_since) / 1000.0;
            g_link_mbps = (double)g_window_bytes * 8.0 / secs / 1e6;
            g_draw_fps = g_window_draws / secs;
            g_window_bytes = 0;
            g_window_draws = 0;
            g_window_since = now_ms;
        }
    }

    printf("capture2cloud client: exiting (video %lu frames / %llu bytes, audio %lu)\n",
           g_video_frames, g_video_bytes, g_audio_frames);
    stop_decoders();
    net_exit();
    input_exit();
    ui_exit();
    display_exit();
    socketExit();
    return 0;
}
