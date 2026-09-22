#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <whb/sdcard.h>

#define SETTINGS_LEAF \
    "/wiiu/apps/capture2cloud/capture2cloud.cfg"

static const Settings DEFAULTS = {
    .host = { 0, 0, 0, 0 },
    .port = C2S_WIIU_PORT,
    .web_port = 5080,
    .input = {
        .deadzone = { 3, 3 },
        .range = { 100, 100 },
        .invert_y = 0,
        .face_by_position = 1,
        .button_map = {
            PAD_B, PAD_A, PAD_Y, PAD_X,
            PAD_LS, PAD_RS, PAD_LB, PAD_RB,
            PAD_LT, PAD_RT, PAD_START, PAD_BACK,
            PAD_LEFT, PAD_UP, PAD_RIGHT, PAD_DOWN
        }
    },
    .output_mode = OUTPUT_TV_AND_GAMEPAD,
    .marker_corner = MARKER_TOP_RIGHT,
    .marker_colour = MARKER_FUCHSIA,
    .token = ""
};

static int settings_path(char *out,
                         unsigned out_size)
{
    if (!WHBMountSdCard()) {
        return -1;
    }

    const char *root =
        WHBGetSdCardMountPath();

    if (!root) {
        return -1;
    }

    snprintf(out,
             out_size,
             "%s%s",
             root,
             SETTINGS_LEAF);

    return 0;
}


void settings_load(Settings *out)
{
    *out = DEFAULTS;

    char path[256];

    if (settings_path(
            path,
            sizeof(path)) != 0) {
        return;
    }

    FILE *f =
        fopen(path, "r");

    if (!f) {
        return;
    }

    char line[192];
    uint32_t button_map_seen = 0;

    while (fgets(
               line,
               sizeof(line),
               f)) {

        unsigned a, b, c, d;
        unsigned port;
        unsigned value;
        unsigned button;

        if (sscanf(
                line,
                " host = %u.%u.%u.%u",
                &a, &b, &c, &d) == 4 &&
            a < 256 &&
            b < 256 &&
            c < 256 &&
            d < 256) {

            out->host[0] = (uint8_t)a;
            out->host[1] = (uint8_t)b;
            out->host[2] = (uint8_t)c;
            out->host[3] = (uint8_t)d;

        } else if (sscanf(
                       line,
                       " port = %u",
                       &port) == 1 &&
                   port > 0 &&
                   port < 65536) {

            out->port =
                (uint16_t)port;

        } else if (sscanf(
                       line,
                       " web_port = %u",
                       &port) == 1 &&
                   port > 0 &&
                   port < 65536) {

            out->web_port =
                (uint16_t)port;

        } else if (sscanf(line, " left_deadzone = %u", &value) == 1) {
            out->input.deadzone[0] = value > 40 ? 40 : value;
        } else if (sscanf(line, " right_deadzone = %u", &value) == 1) {
            out->input.deadzone[1] = value > 40 ? 40 : value;
        } else if (sscanf(line, " left_range = %u", &value) == 1) {
            out->input.range[0] = value < 45 ? 45 : value > 100 ? 100 : value;
        } else if (sscanf(line, " right_range = %u", &value) == 1) {
            out->input.range[1] = value < 45 ? 45 : value > 100 ? 100 : value;
        } else if (sscanf(line, " invert_y = %u", &value) == 1) {
            out->input.invert_y = value != 0;
        } else if (sscanf(line, " face_by_position = %u", &value) == 1) {
            out->input.face_by_position = value != 0;
        } else if (sscanf(line, " output_mode = %u", &value) == 1) {
            out->output_mode =
                value < OUTPUT_MODE_COUNT
                    ? (uint8_t)value
                    : OUTPUT_TV_AND_GAMEPAD;
        } else if (sscanf(line, " marker_corner = %u", &value) == 1) {
            out->marker_corner =
                value < MARKER_CORNER_COUNT
                    ? (uint8_t)value
                    : MARKER_TOP_RIGHT;
        } else if (sscanf(line, " marker_colour = %u", &value) == 1) {
            out->marker_colour =
                value < MARKER_COLOUR_COUNT
                    ? (uint8_t)value
                    : MARKER_FUCHSIA;
        } else if (sscanf(line, " button_%u = %u", &button, &value) == 2 &&
                   button < INPUT_BUTTON_COUNT &&
                   value < PAD_SLOT_COUNT) {
            out->input.button_map[button] = (uint8_t)value;
            button_map_seen |= 1u << button;
        } else {
            char token[
                C2S_MAX_TOKEN_LEN + 1];

            if (sscanf(
                    line,
                    " token = %64s",
                    token) == 1) {

                snprintf(
                    out->token,
                    sizeof(out->token),
                    "%s",
                    token);
            }
        }
    }

    fclose(f);

    if (button_map_seen == 0 &&
        !out->input.face_by_position) {

        out->input.button_map[0] = PAD_A;
        out->input.button_map[1] = PAD_B;
        out->input.button_map[2] = PAD_X;
        out->input.button_map[3] = PAD_Y;
    } else if (button_map_seen !=
               ((1u << INPUT_BUTTON_COUNT) - 1u)) {

        input_config_reset_bindings(&out->input);
    }

    input_config_sanitize(&out->input);
}


int settings_save(const Settings *s,
                  char *why,
                  unsigned why_size)
{
    char path[256];

    if (settings_path(
            path,
            sizeof(path)) != 0) {

        snprintf(
            why,
            why_size,
            "no SD card");

        return -1;
    }

    FILE *f =
        fopen(path, "w");

    if (!f) {
        snprintf(
            why,
            why_size,
            "cannot write %s",
            SETTINGS_LEAF);

        return -1;
    }

    fprintf(
        f,
        "# capture2cloud Wii U client\n"
        "# PLAYER_PASSWORD is intentionally never stored here.\n"
        "host = %u.%u.%u.%u\n"
        "port = %u\n"
        "web_port = %u\n"
        "left_deadzone = %u\n"
        "right_deadzone = %u\n"
        "left_range = %u\n"
        "right_range = %u\n"
        "invert_y = %u\n"
        "face_by_position = %u\n"
        "output_mode = %u\n"
        "marker_corner = %u\n"
        "marker_colour = %u\n"
        "token = %s\n",
        s->host[0],
        s->host[1],
        s->host[2],
        s->host[3],
        s->port,
        s->web_port,
        s->input.deadzone[0],
        s->input.deadzone[1],
        s->input.range[0],
        s->input.range[1],
        s->input.invert_y,
        s->input.face_by_position,
        s->output_mode,
        s->marker_corner,
        s->marker_colour,
        s->token);

    for (unsigned i = 0;
         i < INPUT_BUTTON_COUNT;
         ++i) {

        fprintf(
            f,
            "button_%u = %u\n",
            i,
            s->input.button_map[i]);
    }

    fclose(f);

    return 0;
}


void settings_host_string(
    const Settings *s,
    char *out,
    unsigned out_size)
{
    snprintf(
        out,
        out_size,
        "%u.%u.%u.%u",
        s->host[0],
        s->host[1],
        s->host[2],
        s->host[3]);
}


int settings_set_host_string(
    Settings *s,
    const char *text)
{
    unsigned a, b, c, d;
    char tail = 0;

    if (!text ||
        sscanf(
            text,
            "%u.%u.%u.%u%c",
            &a, &b, &c, &d,
            &tail) != 4) {

        return -1;
    }

    if (a > 255 ||
        b > 255 ||
        c > 255 ||
        d > 255) {

        return -1;
    }

    s->host[0] = (uint8_t)a;
    s->host[1] = (uint8_t)b;
    s->host[2] = (uint8_t)c;
    s->host[3] = (uint8_t)d;

    return 0;
}
