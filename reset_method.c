#define _GNU_SOURCE

#include "reset_method.h"

#include "app_config.h"
#include "video_capture.h"

#include <SDL2/SDL.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Current legacy reset method: execute scripts/wake_console.sh.
 *
 * The script currently power-cycles the console through Home Assistant.
 * Keeping execution here rather than in web_stream.c is intentional:
 * HTTP, native clients and the local GUI should all ask the same reset
 * layer to wake the console without knowing how that is implemented.
 *
 * Bluetooth wake will become a second reset method behind this API.
 */


const char *reset_method_name(ResetMethod method)
{
    switch (method) {
    case RESET_METHOD_SCRIPT:
        return "script";
    case RESET_METHOD_BLUETOOTH:
        return "bluetooth";
    default:
        return NULL;
    }
}

ResetMethod reset_method_current(void)
{
    char value[32];
    const char *name = config_get_str(
        "RESET_METHOD",
        value,
        sizeof(value),
        "script");

    if (strcmp(name, "bluetooth") == 0) {
        return RESET_METHOD_BLUETOOTH;
    }

    if (strcmp(name, "script") != 0) {
        fprintf(stderr,
                "reset_method: unknown RESET_METHOD '%s', using script\n",
                name);
    }

    return RESET_METHOD_SCRIPT;
}

int reset_method_set(ResetMethod method)
{
    const char *name = reset_method_name(method);

    if (!name) {
        return -1;
    }

    return config_set_str("RESET_METHOD", name);
}

static int script_wake_thread(void *arg)
{
    char *cmd = arg;
    int rc = system(cmd);

    free(cmd);

    if (rc != 0) {
        fprintf(stderr,
                "reset_method: wake_console.sh exited with %d\n",
                rc);
    }

    /*
     * Do not reset the controller output immediately.
     *
     * The existing script method cuts and restores mains power. The
     * console takes several seconds before it is ready to accept the
     * controller again, so the capture loop waits until the picture
     * changes away from the no-signal pattern and performs recovery at
     * that point.
     *
     * The future Bluetooth method has different timing and will use its
     * own post-wake recovery sequence.
     */
    video_capture_watch_for_change();

    return 0;
}

static int script_wake(void)
{
    char script[PATH_MAX];
    app_path(script, sizeof(script), "scripts/wake_console.sh");

    if (!script[0]) {
        return -1;
    }

    char *cmd = malloc(PATH_MAX + 64);
    if (!cmd) {
        return -1;
    }

    snprintf(
        cmd,
        PATH_MAX + 64,
        "/bin/bash '%s' >/dev/null 2>&1",
        script);

    SDL_Thread *thread = SDL_CreateThread(
        script_wake_thread,
        "wake-console",
        cmd);

    if (!thread) {
        free(cmd);
        return -1;
    }

    SDL_DetachThread(thread);
    return 0;
}

int reset_method_wake(void)
{
    switch (reset_method_current()) {
    case RESET_METHOD_SCRIPT:
        return script_wake();

    case RESET_METHOD_BLUETOOTH:
        /*
         * The selector exists before the Bluetooth implementation on
         * purpose: UI/configuration can be built against a stable reset
         * abstraction instead of teaching callers about each backend.
         */
        fprintf(stderr,
                "reset_method: Bluetooth wake is selected but not configured yet\n");
        return -1;

    default:
        return -1;
    }
}
