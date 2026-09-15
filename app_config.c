#define _GNU_SOURCE

#include "app_config.h"

#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_RELATIVE_PATH "scripts/.env"

const char *app_dir(void) {
    static char dir[PATH_MAX];
    static int resolved = 0;
    if (resolved) {
        return dir;
    }
    resolved = 1;

    /* /proc/self/exe is the running binary itself, symlinks already
     * followed -- unlike argv[0], which depends on how the process was
     * invoked and can be a bare name found via PATH. */
    ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
    if (n <= 0) {
        snprintf(dir, sizeof(dir), ".");
        return dir;
    }
    dir[n] = '\0';

    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
    } else if (slash) {
        dir[1] = '\0'; /* binary sitting directly in "/" */
    }
    return dir;
}

/* Built by hand rather than with snprintf("%s/%s", ...): the compiler
 * cannot prove a formatted concatenation of two runtime strings fits,
 * so that version warns under -Wformat-truncation even though snprintf
 * would truncate safely. Doing the length check explicitly is both
 * warning-free and clearer about what happens when it doesn't fit. */
void app_path(char *out, size_t out_size, const char *relative) {
    if (out_size == 0) {
        return;
    }
    const char *dir = app_dir();
    size_t dir_len = strlen(dir);
    size_t rel_len = strlen(relative);

    if (dir_len + 1 + rel_len + 1 > out_size) {
        fprintf(stderr, "config: path too long, truncated: %s/%s\n", dir, relative);
        out[0] = '\0';
        return;
    }
    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, relative, rel_len);
    out[dir_len + 1 + rel_len] = '\0';
}

void app_config_path(char *out, size_t out_size) {
    app_path(out, out_size, CONFIG_RELATIVE_PATH);
}

/* The whole file, held in memory, re-read only when it has actually
 * changed.
 *
 * A stat still happens on every lookup, so "edit the file and it
 * applies" keeps working exactly as before -- but it is a metadata check
 * served from the kernel's inode cache, not an open, read and parse of
 * the file. That is the difference that matters: the password is
 * consulted on every HTTP request and every native handshake, so the old
 * version re-read and re-parsed the whole file continuously while anyone
 * was connected.
 *
 * The timestamp is compared to the nanosecond. Whole seconds would miss
 * two edits inside the same second, which is not a thing a person does
 * but is exactly what a test does. */
static char *g_config_text = NULL;
static size_t g_config_len = 0;
static struct timespec g_config_mtime = {0, 0};
static off_t g_config_size = -1;

static void config_refresh(void) {
    char path[PATH_MAX];
    app_config_path(path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        free(g_config_text);
        g_config_text = NULL;
        g_config_len = 0;
        g_config_size = -1;
        return;
    }
    if (g_config_text && st.st_mtim.tv_sec == g_config_mtime.tv_sec &&
        st.st_mtim.tv_nsec == g_config_mtime.tv_nsec && st.st_size == g_config_size) {
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char *text = malloc((size_t)st.st_size + 1);
    if (!text) {
        fclose(f);
        return;
    }
    size_t got = fread(text, 1, (size_t)st.st_size, f);
    fclose(f);
    text[got] = '\0';

    free(g_config_text);
    g_config_text = text;
    g_config_len = got;
    g_config_mtime = st.st_mtim;
    g_config_size = st.st_size;
}

int config_get(const char *key, char *out, size_t out_size) {
    if (out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    config_refresh();
    if (!g_config_text) {
        return 0;
    }

    const size_t key_len = strlen(key);
    char line[1024];
    const char *cursor = g_config_text;
    const char *end = g_config_text + g_config_len;
    while (cursor < end) {
        const char *nl = memchr(cursor, '\n', (size_t)(end - cursor));
        size_t n = nl ? (size_t)(nl - cursor) : (size_t)(end - cursor);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, cursor, n);
        line[n] = '\0';
        cursor = nl ? nl + 1 : end;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
            continue;
        }
        if (strncmp(p, key, key_len) != 0 || p[key_len] != '=') {
            continue;
        }

        char *value = p + key_len + 1;
        value[strcspn(value, "\r\n")] = '\0';

        /* The same file is `source`d by the shell scripts, where quoting
         * a value is normal -- the quotes must not become part of it. */
        size_t vlen = strlen(value);
        if (vlen >= 2 && ((value[0] == '"' && value[vlen - 1] == '"') ||
                          (value[0] == '\'' && value[vlen - 1] == '\''))) {
            value[vlen - 1] = '\0';
            value++;
        }

        snprintf(out, out_size, "%s", value);
        /* Keep going: if a key appears twice, the last wins, matching
         * what sourcing the file in a shell would do. */
    }
    return out[0] != '\0';
}

void app_config_invalidate(void) {
    free(g_config_text);
    g_config_text = NULL;
    g_config_len = 0;
    g_config_size = -1;
}

static volatile int g_restart_requested = 0;

void app_request_restart(void) {
    g_restart_requested = 1;
}

int app_restart_requested(void) {
    return g_restart_requested;
}

int app_verbose(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = (int)config_get_int("VERBOSE", 0, 0, 1);
    }
    return cached;
}

const char *config_get_str(const char *key, char *out, size_t out_size, const char *fallback) {
    if (config_get(key, out, out_size)) {
        return out;
    }
    return fallback;
}

/*
 * Writes one key back to the .env, leaving every other byte of it
 * exactly as it was.
 *
 * This file holds real credentials -- the Home Assistant token, the
 * player password -- so it is rewritten line by line rather than
 * regenerated: anything this function does not recognise is copied
 * through untouched, comments and blank lines included. A setting that
 * saved itself by reformatting the file would be a setting that quietly
 * dropped somebody's token the first time the format changed.
 *
 * Written to a temporary file in the same directory and renamed over
 * the original, so an interrupted write leaves the old file whole
 * rather than a half-written one with no password in it.
 */
int config_set_int(const char *key, long value) {
    if (!key || !*key) {
        return -1;
    }
    char path[512];
    app_config_path(path, sizeof(path));

    FILE *in = fopen(path, "r");
    char tmp[600];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
        if (in) fclose(in);
        return -1;
    }
    FILE *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        fprintf(stderr, "config: cannot write %s\n", tmp);
        return -1;
    }

    const size_t key_len = strlen(key);
    int replaced = 0;
    if (in) {
        char line[4096];
        while (fgets(line, sizeof(line), in)) {
            /* The key, then '=', with leading spaces allowed and
             * nothing else before it. A commented-out line stays
             * commented: it is not this setting, it is a note about
             * it. */
            const char *at = line;
            while (*at == ' ' || *at == '\t') {
                at++;
            }
            if (!replaced && strncmp(at, key, key_len) == 0 && at[key_len] == '=') {
                fprintf(out, "%s=%ld\n", key, value);
                replaced = 1;
                continue;
            }
            fputs(line, out);
        }
        fclose(in);
    }
    if (!replaced) {
        fprintf(out, "%s=%ld\n", key, value);
    }
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
        fclose(out);
        unlink(tmp);
        return -1;
    }
    fclose(out);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        fprintf(stderr, "config: cannot replace %s\n", path);
        return -1;
    }
    /* The cached copy is now a generation behind its own file. */
    app_config_invalidate();
    return 0;
}

long config_get_int(const char *key, long fallback, long min_value, long max_value) {
    char buf[64];
    if (!config_get(key, buf, sizeof(buf))) {
        return fallback;
    }
    /* Base 0, not 10: USB ids are naturally written in hex (lsusb prints
     * "2508:0003"), so both `0x2508` and a plain decimal must work. */
    char *end = NULL;
    long value = strtol(buf, &end, 0);
    if (end == buf) {
        fprintf(stderr, "config: %s is not a number ('%s'), using %ld\n", key, buf, fallback);
        return fallback;
    }
    if (value < min_value) {
        fprintf(stderr, "config: %s=%ld is below the minimum, clamped to %ld\n", key, value, min_value);
        value = min_value;
    } else if (value > max_value) {
        fprintf(stderr, "config: %s=%ld is above the maximum, clamped to %ld\n", key, value, max_value);
        value = max_value;
    }
    return value;
}
