#ifndef CAPTURE2WIIU_SETTINGS_H
#define CAPTURE2WIIU_SETTINGS_H

#include <stdint.h>

#include "c2s_protocol.h"

/*
 * Persistent Wii U client settings.
 *
 * The PLAYER_PASSWORD itself is deliberately NEVER stored here.
 * /login exchanges it for a temporary session token; only that token
 * is written to the SD card.
 */
typedef struct {
    uint8_t  host[4];
    uint16_t port;       /* native Wii U stream, normally 5083 */
    uint16_t web_port;   /* HTTP /login, normally 5080 */

    char token[C2S_MAX_TOKEN_LEN + 1];
} Settings;

void settings_load(Settings *out);

int settings_save(const Settings *s,
                  char *why,
                  unsigned why_size);

void settings_host_string(const Settings *s,
                          char *out,
                          unsigned out_size);

int settings_set_host_string(Settings *s,
                             const char *text);

#endif
