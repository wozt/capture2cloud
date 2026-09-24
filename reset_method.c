#define _GNU_SOURCE

#include "reset_method.h"

#include "app_config.h"
#include "video_capture.h"

#include <SDL2/SDL.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

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
    /*
     * Step one of the reset-method abstraction deliberately preserves
     * the old behaviour exactly. The configurable script/Bluetooth
     * selector is added on top of this interface next.
     */
    return script_wake();
}
