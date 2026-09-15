#ifndef CAPTURE2WIIU_SETTINGS_H
#define CAPTURE2WIIU_SETTINGS_H

#include <stdint.h>

/*
 * Where the host is, kept on the SD card beside the program.
 *
 * It was a #define until the first run on hardware, which is exactly how
 * long that lasted: the address was compiled in as 192.168.1.10 and the
 * machine was on 192.168.2.100, and there was nothing to be done about
 * it from the sofa. It is a setting now, and it is edited on the pad's
 * screen before anything is dialled.
 *
 * The file is plain "key = value" text, one per line, so it can also be
 * fixed over FTP without rebuilding anything -- ftpiiu serves the SD
 * card whether or not system files are allowed.
 */

typedef struct {
    uint8_t  host[4];   /* IPv4, one octet per entry */
    uint16_t port;
} Settings;

/* Loads the file, or fills in the defaults when there is none. Always
 * leaves `out` usable. */
void settings_load(Settings *out);

/* Writes it back. Returns 0, or -1 with the reason in `why`. Failing to
 * save is not fatal -- the address still applies to this session. */
int  settings_save(const Settings *s, char *why, unsigned why_size);

/* "192.168.2.100", for passing to net_connect and for the screen. */
void settings_host_string(const Settings *s, char *out, unsigned out_size);

/* The other direction, for what somebody typed. Returns 0 when the text
 * was four numbers in range, -1 otherwise -- and on -1 the settings are
 * left exactly as they were, so a typo cannot quietly become 0.0.0.0. */
int settings_set_host_string(Settings *s, const char *text);

#endif /* CAPTURE2WIIU_SETTINGS_H */
