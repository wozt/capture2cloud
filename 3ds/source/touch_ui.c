#include "touch_ui.h"
#include "version.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t *g_framebuffer;
static u16 g_fb_width;
static u16 g_fb_height;
static int g_active;

static const char *codec_name(uint8_t codec)
{
    if (codec == C2S_CODEC_OLD3DS_MPEG1) return "MPEG1";
    if (codec == C2S_CODEC_OLD3DS_MPEG2) return "MPEG2";
    if (codec == C2S_CODEC_OLD3DS_MPEG4) return "MPEG4";
    return "JPEG";
}

enum {
    TOUCH_NONE,
    TOUCH_ZL,
    TOUCH_L3,
    TOUCH_R3,
    TOUCH_ZR,
    TOUCH_HOME,
    TOUCH_CAPTURE,
    TOUCH_STICK,
    TOUCH_CONFIG,
    TOUCH_STATS,
    TOUCH_CODEC,
    TOUCH_ASPECT
};

static uint16_t color(int red, int green, int blue)
{
    return (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

static void begin_draw(void)
{
    g_framebuffer = (uint16_t *)gfxGetFramebuffer(
        GFX_BOTTOM, GFX_LEFT, &g_fb_width, &g_fb_height);
}

static void pixel(int x, int y, uint16_t value)
{
    if (!g_framebuffer || x < 0 || x >= 320 || y < 0 || y >= 240) return;
    g_framebuffer[(size_t)x * g_fb_width + (239 - y)] = value;
}

static void rectangle(int x, int y, int width, int height, uint16_t value)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > 320) width = 320 - x;
    if (y + height > 240) height = 240 - y;
    for (int px = x; px < x + width; px++) {
        uint16_t *dst = g_framebuffer + (size_t)px * g_fb_width + (240 - y - height);
        for (int py = 0; py < height; py++) dst[py] = value;
    }
}

static void outline(int x, int y, int width, int height, uint16_t value)
{
    rectangle(x, y, width, 2, value);
    rectangle(x, y + height - 2, width, 2, value);
    rectangle(x, y, 2, height, value);
    rectangle(x + width - 2, y, 2, height, value);
}

static void circle(int cx, int cy, int radius, uint16_t value)
{
    int radius_squared = radius * radius;
    for (int x = -radius; x <= radius; x++) {
        int y_extent = (int)sqrtf((float)(radius_squared - x * x));
        for (int y = -y_extent; y <= y_extent; y++) pixel(cx + x, cy + y, value);
    }
}

static const char *glyph(char character)
{
    if (character >= 'a' && character <= 'z') character -= 32;
    switch (character) {
        case 'A': return "01110100011000111111100011000110001";
        case 'B': return "11110100011000111110100011000111110";
        case 'C': return "01111100001000010000100001000001111";
        case 'D': return "11110100011000110001100011000111110";
        case 'E': return "11111100001000011110100001000011111";
        case 'F': return "11111100001000011110100001000010000";
        case 'G': return "01111100001000010111100011000101111";
        case 'H': return "10001100011000111111100011000110001";
        case 'I': return "11111001000010000100001000010011111";
        case 'J': return "00111000100001000010000101001001100";
        case 'K': return "10001100101010011000101001001010001";
        case 'L': return "10000100001000010000100001000011111";
        case 'M': return "10001110111010110101100011000110001";
        case 'N': return "10001110011010110011100011000110001";
        case 'O': return "01110100011000110001100011000101110";
        case 'P': return "11110100011000111110100001000010000";
        case 'Q': return "01110100011000110001101011001001101";
        case 'R': return "11110100011000111110101001001010001";
        case 'S': return "01111100001000001110000010000111110";
        case 'T': return "11111001000010000100001000010000100";
        case 'U': return "10001100011000110001100011000101110";
        case 'V': return "10001100011000110001100010101000100";
        case 'W': return "10001100011000110101101011101110001";
        case 'X': return "10001100010101000100010101000110001";
        case 'Y': return "10001100010101000100001000010000100";
        case 'Z': return "11111000010001000100010001000011111";
        case '0': return "01110100011001110101110011000101110";
        case '1': return "00100011000010000100001000010001110";
        case '2': return "01110100010000100010001000100011111";
        case '3': return "11110000010000101110000010000111110";
        case '4': return "00010001100101010010111110001000010";
        case '5': return "11111100001000011110000010000111110";
        case '6': return "01110100001000011110100011000101110";
        case '7': return "11111000010001000100010000100001000";
        case '8': return "01110100011000101110100011000101110";
        case '9': return "01110100011000101111000010000101110";
        case '-': return "00000000000000011111000000000000000";
        case '+': return "00000001000010011111001000010000000";
        case '.': return "00000000000000000000000000110001100";
        case ':': return "00000011000110000000011000110000000";
        case '/': return "00001000100001000100010001000010000";
        case '%': return "11001110100001000100010001011110011";
        case '=': return "00000111110000011111000000000000000";
        case '(': return "00010001000100001000010000010000010";
        case ')': return "01000001000001000010000100010001000";
        case '?': return "01110100010000100010001000000000100";
        case '_': return "00000000000000000000000000000011111";
        default: return "00000000000000000000000000000000000";
    }
}

static void text(int x, int y, const char *value, int scale, uint16_t value_color)
{
    for (; *value; value++, x += 6 * scale) {
        const char *shape = glyph(*value);
        for (int row = 0; row < 7; row++) {
            for (int column = 0; column < 5; column++) {
                if (shape[row * 5 + column] == '1') {
                    rectangle(x + column * scale, y + row * scale,
                              scale, scale, value_color);
                }
            }
        }
    }
}

static bool inside(const touchPosition *position, int x, int y, int width, int height)
{
    return position && position->px >= x && position->px < x + width &&
           position->py >= y && position->py < y + height;
}

static void button(int x, int y, int width, int height, const char *label, bool pressed)
{
    uint16_t fill = pressed ? color(38, 158, 197) : color(33, 47, 64);
    uint16_t border = pressed ? color(153, 231, 255) : color(82, 111, 137);
    rectangle(x, y, width, height, fill);
    outline(x, y, width, height, border);
    int label_width = (int)strlen(label) * 6 - 1;
    text(x + (width - label_width) / 2, y + (height - 7) / 2,
         label, 1, color(242, 247, 250));
}

void touch_ui_update_controller(u32 keys_down, u32 keys_held, u32 keys_up,
                                const touchPosition *position, int deadzone,
                                TouchUiEvent *event)
{
    event->home_pressed = false;
    event->capture_pressed = false;
    event->open_config = false;
    event->toggle_stats = false;
    event->toggle_codec = false;
    event->toggle_aspect = false;
    memset(&event->controls, 0, sizeof(event->controls));

    if (keys_down & KEY_TOUCH) {
        if (inside(position, 8, 12, 57, 36)) g_active = TOUCH_ZL;
        else if (inside(position, 72, 12, 57, 36)) g_active = TOUCH_L3;
        else if (inside(position, 191, 12, 57, 36)) g_active = TOUCH_R3;
        else if (inside(position, 255, 12, 57, 36)) g_active = TOUCH_ZR;
        else if (inside(position, 8, 146, 88, 34)) {
            g_active = TOUCH_HOME;
            event->home_pressed = true;
        } else if (inside(position, 102, 146, 88, 34)) {
            g_active = TOUCH_CAPTURE;
            event->capture_pressed = true;
        } else if (inside(position, 198, 104, 118, 124)) {
            g_active = TOUCH_STICK;
        } else if (inside(position, 8, 184, 88, 24)) {
            g_active = TOUCH_ASPECT;
            event->toggle_aspect = true;
        } else if (inside(position, 102, 184, 88, 24)) {
            g_active = TOUCH_STATS;
            event->toggle_stats = true;
        } else if (inside(position, 8, 212, 88, 24)) {
            g_active = TOUCH_CONFIG;
            event->open_config = true;
        } else if (inside(position, 102, 212, 88, 24)) {
            g_active = TOUCH_CODEC;
            event->toggle_codec = true;
        }
    }
    if (keys_up & KEY_TOUCH) g_active = TOUCH_NONE;
    if (!(keys_held & KEY_TOUCH)) return;

    switch (g_active) {
        case TOUCH_ZL: event->controls.zl = true; break;
        case TOUCH_ZR: event->controls.zr = true; break;
        case TOUCH_L3: event->controls.l3 = true; break;
        case TOUCH_R3: event->controls.r3 = true; break;
        case TOUCH_STICK: {
            int dx = (int)position->px - 257;
            int dy = (int)position->py - 164;
            float length = sqrtf((float)(dx * dx + dy * dy));
            const float radius = 54.0f;
            if (length > radius && length > 0.0f) {
                dx = (int)(dx * radius / length);
                dy = (int)(dy * radius / length);
                length = radius;
            }
            int x = dx * 100 / (int)radius;
            int y = dy * 100 / (int)radius;
            if (abs(x) < deadzone) x = 0;
            if (abs(y) < deadzone) y = 0;
            event->controls.rx = (int8_t)x;
            event->controls.ry = (int8_t)y;
            break;
        }
        default: break;
    }
}

ConfigAction touch_ui_config_action(u32 keys_down, const touchPosition *position)
{
    if (!(keys_down & KEY_TOUCH)) return CONFIG_NONE;
    if (inside(position, 8, 20, 194, 28)) return CONFIG_EDIT_HOST;
    if (inside(position, 206, 20, 106, 28)) return CONFIG_EDIT_STREAM_PORT;
    if (inside(position, 8, 52, 194, 28)) return CONFIG_EDIT_PASSWORD;
    if (inside(position, 206, 52, 106, 28)) return CONFIG_EDIT_WEB_PORT;

    if (inside(position, 8, 88, 72, 25)) return CONFIG_TOGGLE_AUTO;
    if (inside(position, 84, 88, 82, 25)) return CONFIG_TOGGLE_AUDIO;
    if (inside(position, 170, 88, 82, 25)) return CONFIG_TOGGLE_CODEC;
    if (inside(position, 256, 88, 56, 25)) return CONFIG_TOGGLE_ASPECT;

    if (inside(position, 8, 121, 58, 29)) return CONFIG_SAVE;
    if (inside(position, 70, 121, 80, 29)) return CONFIG_CONNECT;
    if (inside(position, 154, 121, 78, 29)) return CONFIG_RECONNECT;
    if (inside(position, 236, 121, 76, 29)) return CONFIG_DISCONNECT;

    if (inside(position, 8, 207, 304, 29)) return CONFIG_CONTROLLER;
    return CONFIG_NONE;
}

void touch_ui_draw_controller(const TouchUiEvent *event,
                              const NetworkStats *network,
                              const VideoStats *video,
                              const AudioStats *audio,
                              bool show_stats,
                              uint8_t video_codec,
                              bool aspect_16_9)
{
    begin_draw();
    rectangle(0, 0, 320, 240, color(14, 20, 29));

    text(6, 2, "CAPTURE2CLOUD OLD3DS v" C2C_VERSION, 1,
         color(127, 205, 235));

    /* Missing Switch controls. */
    button(8, 12, 57, 36, "ZL", event->controls.zl);
    button(72, 12, 57, 36, "L3", event->controls.l3);
    button(191, 12, 57, 36, "R3", event->controls.r3);
    button(255, 12, 57, 36, "ZR", event->controls.zr);

    uint16_t status_color = network->state == NETWORK_CONNECTED
        ? color(78, 214, 135) : color(244, 175, 73);
    text(6, 55, network->status, 1, status_color);

    /*
     * Keep diagnostics in the upper-left area.  The complete lower-right
     * quadrant belongs to the virtual right stick.
     */
    if (show_stats) {
        char line[64];

        snprintf(line, sizeof(line), "RX %.1F DEC %.1F DSP %.1F",
                 network->video_fps,
                 video->decode_fps,
                 video->display_fps);
        text(6, 67, line, 1, color(214, 224, 231));

        snprintf(line, sizeof(line), "%.0F KBPS Q %lu DROP %lu",
                 network->video_kbps,
                 (unsigned long)network->queue_depth,
                 (unsigned long)network->video_dropped);
        text(6, 77, line, 1, color(214, 224, 231));

        snprintf(line, sizeof(line), "NET %.1F DEC %.1F UP %.1FMS",
                 network->receive_ms,
                 video->decode_ms,
                 video->upload_ms);
        text(6, 87, line, 1, color(214, 224, 231));

        const char *audio_name =
            network->audio_codec == C2S_CODEC_PCM_S16LE
                ? "PCM"
                : network->audio_codec == C2S_CODEC_OLD3DS_ADPCM
                    ? "ADPCM" : "OPUS";

        snprintf(line, sizeof(line), "%s Q%lu D%lu U%lu",
                 audio_name,
                 (unsigned long)audio->queued_buffers,
                 (unsigned long)(audio->dropped_packets +
                                 network->audio_dropped),
                 (unsigned long)audio->underruns);
        text(6, 97, line, 1, color(214, 224, 231));

        snprintf(line, sizeof(line), "%s%s C%u ERR %lu",
                 codec_name(video_codec),
                 c2s_old3ds_predictive_codec(video_codec) &&
                         video->hardware_conversion
                     ? "/Y2R" : "",
                 network->worker_core,
                 (unsigned long)video->decode_errors);
        text(6, 107, line, 1, color(214, 224, 231));

        snprintf(line, sizeof(line), "LOCAL LAG %luMS",
                 (unsigned long)video->local_latency_ms);
        text(6, 117, line, 1, color(214, 224, 231));
    }

    /*
     * HOME/CAPTURE replace the redundant virtual +/- controls.
     * START and SELECT already provide +/- physically.
     */
    button(8, 146, 88, 34, "HOME", g_active == TOUCH_HOME);
    button(102, 146, 88, 34, "CAPTURE", g_active == TOUCH_CAPTURE);

    /*
     * Display controls now form a separate row above the main menu row.
     * Use full names rather than the old abbreviated labels.
     */
    button(8, 184, 88, 24,
           aspect_16_9 ? "ASPECT 16:9" : "ASPECT FULL",
           g_active == TOUCH_ASPECT);
    button(102, 184, 88, 24,
           show_stats ? "STATS ON" : "STATS OFF",
           g_active == TOUCH_STATS);

    button(8, 212, 88, 24, "SETTINGS", g_active == TOUCH_CONFIG);
    button(102, 212, 88, 24, codec_name(video_codec),
           g_active == TOUCH_CODEC);

    /*
     * Right stick lives completely in the bottom-right corner and no longer
     * overlaps the aspect/stat controls.
     */
    text(225, 101, "RIGHT STICK", 1, color(160, 177, 191));
    circle(257, 164, 56, color(31, 44, 58));
    circle(257, 164, 52, color(19, 29, 41));

    int knob_x = 257 + event->controls.rx * 38 / 100;
    int knob_y = 164 + event->controls.ry * 38 / 100;
    circle(knob_x, knob_y, 11, color(56, 178, 215));
}

void touch_ui_draw_config(const AppConfig *config, const NetworkStats *network,
                          const char *message)
{
    begin_draw();
    rectangle(0, 0, 320, 240, color(14, 20, 29));

    text(8, 5, "CAPTURE2CLOUD v" C2C_VERSION " SETTINGS", 1,
         color(127, 205, 235));

    char label[96];

    /*
     * Network configuration in two rows:
     *
     *   HOST / STREAM PORT
     *   PASSWORD / LOGIN PORT
     */
    snprintf(label, sizeof(label), "HOST %s", config->host);
    button(8, 20, 194, 28, label, false);

    snprintf(label, sizeof(label), "STREAM %u", config->stream_port);
    button(206, 20, 106, 28, label, false);

    snprintf(label, sizeof(label), "PASSWORD %s",
             config->password[0] ? "********" : "NONE");
    button(8, 52, 194, 28, label, false);

    snprintf(label, sizeof(label), "LOGIN %u", config->web_port);
    button(206, 52, 106, 28, label, false);

    /* Runtime options. */
    snprintf(label, sizeof(label), "AUTO %s",
             config->auto_connect ? "ON" : "OFF");
    button(8, 88, 72, 25, label, config->auto_connect);

    snprintf(label, sizeof(label), "AUDIO %s",
             config->audio_enabled ? "ON" : "OFF");
    button(84, 88, 82, 25, label, config->audio_enabled);

    button(170, 88, 82, 25, codec_name(config->video_codec), false);

    button(256, 88, 56, 25,
           config->aspect_16_9 ? "16:9" : "FULL",
           config->aspect_16_9);

    /* Connection actions. */
    button(8, 121, 58, 29, "SAVE", false);
    button(70, 121, 80, 29, "CONNECT", false);
    button(154, 121, 78, 29, "RECONNECT", false);
    button(236, 121, 76, 29, "DISCONNECT", false);

    text(8, 160, "STATUS", 1, color(160, 177, 191));
    text(8, 171, network->status, 1,
         network->state == NETWORK_CONNECTED
             ? color(78, 214, 135)
             : color(244, 175, 73));

    if (message && message[0])
        text(8, 183, message, 1, color(214, 224, 231));

    button(8, 207, 304, 29, "BACK TO CONTROLLER", false);
}
