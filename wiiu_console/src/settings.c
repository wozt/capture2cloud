#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <whb/sdcard.h>

/* Beside the .wuhb, so the program and its settings travel together. */
#define SETTINGS_LEAF "/wiiu/apps/capture2cloud/capture2cloud.cfg"

/* What it is before anybody has said otherwise. Deliberately not a
 * plausible-looking address: 0.0.0.0 shows up on screen as obviously
 * unset, where a wrong-but-real address looks like it should work and
 * sends you looking at the host instead. */
static const Settings DEFAULTS = { .host = { 0, 0, 0, 0 }, .port = 5083 };

/* The SD card has to be mounted before any of this, and the mount point
 * is not a constant -- it is whatever libwhb chose. */
static int settings_path(char *out, unsigned out_size)
{
    if (!WHBMountSdCard()) {
        return -1;
    }
    const char *root = WHBGetSdCardMountPath();
    if (!root) {
        return -1;
    }
    snprintf(out, out_size, "%s%s", root, SETTINGS_LEAF);
    return 0;
}

void settings_load(Settings *out)
{
    *out = DEFAULTS;

    char path[256];
    if (settings_path(path, sizeof(path)) != 0) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned a, b, c, d, port;
        if (sscanf(line, " host = %u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
            a < 256 && b < 256 && c < 256 && d < 256) {
            out->host[0] = (uint8_t)a;
            out->host[1] = (uint8_t)b;
            out->host[2] = (uint8_t)c;
            out->host[3] = (uint8_t)d;
        } else if (sscanf(line, " port = %u", &port) == 1 && port > 0 && port < 65536) {
            out->port = (uint16_t)port;
        }
    }
    fclose(f);
}

int settings_save(const Settings *s, char *why, unsigned why_size)
{
    char path[256];
    if (settings_path(path, sizeof(path)) != 0) {
        snprintf(why, why_size, "no SD card");
        return -1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(why, why_size, "cannot write %s", SETTINGS_LEAF);
        return -1;
    }
    fprintf(f,
            "# capture2cloud on a Wii U console.\n"
            "# Editable here, on the pad's screen, or over FTP.\n"
            "host = %u.%u.%u.%u\n"
            "port = %u\n",
            s->host[0], s->host[1], s->host[2], s->host[3], s->port);
    fclose(f);
    return 0;
}

void settings_host_string(const Settings *s, char *out, unsigned out_size)
{
    snprintf(out, out_size, "%u.%u.%u.%u", s->host[0], s->host[1], s->host[2], s->host[3]);
}

int settings_set_host_string(Settings *s, const char *text)
{
    unsigned a, b, c, d;
    char tail = 0;
    /* The trailing %c catches "1.2.3.4.5" and "1.2.3.4x", which sscanf
     * would otherwise accept by simply stopping early. */
    if (!text || sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return -1;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return -1;
    }
    s->host[0] = (uint8_t)a;
    s->host[1] = (uint8_t)b;
    s->host[2] = (uint8_t)c;
    s->host[3] = (uint8_t)d;
    return 0;
}
