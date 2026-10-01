#include "config.h"

#include "c2s_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void config_defaults(AppConfig *config)
{
    memset(config, 0, sizeof(*config));
    snprintf(config->host, sizeof(config->host), "192.168.1.2");
    config->stream_port = C2S_OLD3DS_PORT;
    config->web_port = 5080;
    config->auto_connect = false;
    config->audio_enabled = true;
    config->stats_enabled = false;
    config->aspect_16_9 = false;
    config->video_codec = C2S_CODEC_OLD3DS_MPEG1;
    config->right_stick_deadzone = 12;
}

static void strip_newline(char *value)
{
    value[strcspn(value, "\r\n")] = '\0';
}

bool config_load(AppConfig *config)
{
    config_defaults(config);
    FILE *file = fopen(OLD3DS_CONFIG_PATH, "r");
    if (!file) return false;

    char line[192];
    while (fgets(line, sizeof(line), file)) {
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals++ = '\0';
        strip_newline(equals);
        if (!strcmp(line, "host")) {
            snprintf(config->host, sizeof(config->host), "%s", equals);
        } else if (!strcmp(line, "stream_port")) {
            int port = atoi(equals);
            if (port > 0 && port <= 65535) config->stream_port = (uint16_t)port;
        } else if (!strcmp(line, "web_port")) {
            int port = atoi(equals);
            if (port > 0 && port <= 65535) config->web_port = (uint16_t)port;
        } else if (!strcmp(line, "password")) {
            snprintf(config->password, sizeof(config->password), "%s", equals);
        } else if (!strcmp(line, "auto_connect")) {
            config->auto_connect = atoi(equals) != 0;
        } else if (!strcmp(line, "audio")) {
            config->audio_enabled = atoi(equals) != 0;
        } else if (!strcmp(line, "stats")) {
            config->stats_enabled = atoi(equals) != 0;
        } else if (!strcmp(line, "aspect")) {
            config->aspect_16_9 = !strcmp(equals, "16:9");
        } else if (!strcmp(line, "codec")) {
            if (!strcmp(equals, "jpeg")) {
                config->video_codec = C2S_CODEC_OLD3DS_JPEG;
            } else if (!strcmp(equals, "mpeg1")) {
                config->video_codec = C2S_CODEC_OLD3DS_MPEG1;
            } else if (!strcmp(equals, "mpeg2")) {
                config->video_codec = C2S_CODEC_OLD3DS_MPEG2;
            } else {
                config->video_codec = C2S_CODEC_OLD3DS_MPEG4;
            }
        } else if (!strcmp(line, "right_stick_deadzone")) {
            int value = atoi(equals);
            if (value >= 0 && value <= 50) config->right_stick_deadzone = value;
        }
    }
    fclose(file);
    return true;
}

bool config_save(const AppConfig *config)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/capture2cloud-old3ds", 0777);
    FILE *file = fopen(OLD3DS_CONFIG_PATH, "w");
    if (!file) return false;
    fprintf(file, "host=%s\n", config->host);
    fprintf(file, "stream_port=%u\n", config->stream_port);
    fprintf(file, "web_port=%u\n", config->web_port);
    fprintf(file, "password=%s\n", config->password);
    fprintf(file, "auto_connect=%d\n", config->auto_connect ? 1 : 0);
    fprintf(file, "audio=%d\n", config->audio_enabled ? 1 : 0);
    fprintf(file, "stats=%d\n", config->stats_enabled ? 1 : 0);
    fprintf(file, "aspect=%s\n", config->aspect_16_9 ? "16:9" : "full");
    const char *codec = config->video_codec == C2S_CODEC_OLD3DS_JPEG
        ? "jpeg" : config->video_codec == C2S_CODEC_OLD3DS_MPEG1
            ? "mpeg1" : config->video_codec == C2S_CODEC_OLD3DS_MPEG2
                ? "mpeg2" : "mpeg4";
    fprintf(file, "codec=%s\n", codec);
    fprintf(file, "right_stick_deadzone=%d\n", config->right_stick_deadzone);
    bool ok = fclose(file) == 0;
    return ok;
}
