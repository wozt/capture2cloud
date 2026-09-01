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
