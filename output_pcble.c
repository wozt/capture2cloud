#define _GNU_SOURCE

#include "output_pcble.h"

#include "app_config.h"
#include "controller_state.h"

#include <SDL2/SDL.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define PCBLE_KEEPALIVE_MS 100
#define PCBLE_RETRY_MS 250
#define PCBLE_HOME_MS 150
#define PCBLE_REPLY_MAX 32768

static SDL_mutex *g_lock;
static SDL_Thread *g_thread;

static int g_running;
static int g_dirty;
static int g_pair_mode;
static int g_ipc_up;
static int g_link_up;

static int8_t g_state[CONTROLLER_STATE_COUNT];

static Uint32 g_home_until;
static Uint32 g_last_attempt;
static Uint32 g_last_send;
static Uint32 g_rate_started;
static unsigned g_rate_count;
static double g_report_rate;

static char g_socket_dir[256];

static int pressed(const int8_t state[CONTROLLER_STATE_COUNT], int index)
{
    return state[index] > 0;
}

static uint16_t stick_axis(
    int value,
    int center,
    int positive,
    int negative,
    int invert)
{
    if (value > 100) {
        value = 100;
    } else if (value < -100) {
        value = -100;
    }

    if (invert) {
        value = -value;
    }

    int result;

    if (value >= 0) {
        result = center + (positive * value) / 100;
    } else {
        result = center + (negative * value) / 100;
    }

    if (result < 0) {
        result = 0;
    } else if (result > 4095) {
        result = 4095;
    }

    return (uint16_t)result;
}

/*
 * Translate Capture2Cloud's backend-independent positional state into
 * the three Nintendo button bytes and calibrated 12-bit stick values
 * expected by the vendored pcble backend.
 *
 * Capture2Cloud Y axes follow browser/SDL coordinates: positive is down.
 * Nintendo's internal stick values used by this backend are positive up,
 * hence the inversion on LY and RY.
 */
static void build_nintendo_state(
    const int8_t state[CONTROLLER_STATE_COUNT],
    int home_override,
    uint8_t buttons[3],
    uint16_t sticks[4])
{
    memset(buttons, 0, 3);

    /* Right-side face buttons, by physical position. */
    if (pressed(state, CONTROLLER_FACE_WEST))  buttons[0] |= 1u << 0; /* Y */
    if (pressed(state, CONTROLLER_FACE_NORTH)) buttons[0] |= 1u << 1; /* X */
    if (pressed(state, CONTROLLER_FACE_SOUTH)) buttons[0] |= 1u << 2; /* B */
    if (pressed(state, CONTROLLER_FACE_EAST))  buttons[0] |= 1u << 3; /* A */

    if (pressed(state, CONTROLLER_R1)) buttons[0] |= 1u << 6;
    if (pressed(state, CONTROLLER_R2)) buttons[0] |= 1u << 7;

    if (pressed(state, CONTROLLER_SELECT)) buttons[1] |= 1u << 0; /* Minus */
    if (pressed(state, CONTROLLER_START))  buttons[1] |= 1u << 1; /* Plus */
    if (pressed(state, CONTROLLER_R3))     buttons[1] |= 1u << 2;
    if (pressed(state, CONTROLLER_L3))     buttons[1] |= 1u << 3;
    if (pressed(state, CONTROLLER_HOME) || home_override)
        buttons[1] |= 1u << 4;

    if (pressed(state, CONTROLLER_DOWN))  buttons[2] |= 1u << 0;
    if (pressed(state, CONTROLLER_UP))    buttons[2] |= 1u << 1;
    if (pressed(state, CONTROLLER_RIGHT)) buttons[2] |= 1u << 2;
    if (pressed(state, CONTROLLER_LEFT))  buttons[2] |= 1u << 3;

    if (pressed(state, CONTROLLER_L1)) buttons[2] |= 1u << 6;
    if (pressed(state, CONTROLLER_L2)) buttons[2] |= 1u << 7;

    /*
     * Same centers and measured ranges as pcble2gamepad's validated
     * InputFrame -> ProState conversion.
     */
    sticks[0] = stick_axis(state[CONTROLLER_LX], 2159, 1466, 1517, 0);
    sticks[1] = stick_axis(state[CONTROLLER_LY], 1916, 1583, 1465, 1);
    sticks[2] = stick_axis(state[CONTROLLER_RX], 2070, 1414, 1522, 0);
    sticks[3] = stick_axis(state[CONTROLLER_RY], 2013, 1510, 1531, 1);
}

static int write_all(int fd, const char *data, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t n = write(fd, data + offset, size - offset);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }

        offset += (size_t)n;
    }

    return 1;
}

static int socket_request(
    const char *path,
    const char *request,
    int *initialized)
{
    if (initialized) {
        *initialized = 0;
    }

    if (!path || !*path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return 0;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 150000,
    };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return 0;
    }

    if (!write_all(fd, request, strlen(request))) {
        close(fd);
        return 0;
    }

    char *reply = malloc(PCBLE_REPLY_MAX);
    if (!reply) {
        close(fd);
        return 0;
    }

    size_t used = 0;
    int complete = 0;

    while (used + 1 < PCBLE_REPLY_MAX) {
        ssize_t n = read(fd, reply + used, PCBLE_REPLY_MAX - 1 - used);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (n == 0) {
            break;
        }

        used += (size_t)n;
        reply[used] = '\0';

        char *newline = strchr(reply, '\n');
        if (newline) {
            *newline = '\0';
            complete = 1;
            break;
        }
    }

    close(fd);

    int ok =
        complete &&
        strstr(reply, "\"ok\":true") != NULL;

    if (ok && initialized) {
        *initialized =
            strstr(reply, "\"initialized\":true") != NULL;
    }

    free(reply);
    return ok;
}

static void socket_path(char *out, size_t out_size, const char *name)
{
    snprintf(out, out_size, "%s/%s", g_socket_dir, name);
}

static int send_input(
    const int8_t state[CONTROLLER_STATE_COUNT],
    int home_override,
    int *initialized)
{
    uint8_t buttons[3];
    uint16_t sticks[4];

    build_nintendo_state(
        state,
        home_override,
        buttons,
        sticks);

    char request[256];

    snprintf(
        request,
        sizeof(request),
        "{\"version\":1,\"method\":\"input\","
        "\"buttons\":[%u,%u,%u],"
        "\"sticks\":[%u,%u,%u,%u]}\n",
        buttons[0],
        buttons[1],
        buttons[2],
        sticks[0],
        sticks[1],
        sticks[2],
        sticks[3]);

    char path[320];
    int left_initialized = 0;

    socket_path(
        path,
        sizeof(path),
        g_pair_mode ? "joycon-left.sock" : "pro.sock");

    int left_ok =
        socket_request(
            path,
            request,
            &left_initialized);

    if (!g_pair_mode) {
        if (initialized) {
            *initialized = left_initialized;
        }
        return left_ok;
    }

    int right_initialized = 0;

    socket_path(
        path,
        sizeof(path),
        "joycon-right.sock");

    int right_ok =
        socket_request(
            path,
            request,
            &right_initialized);

    if (initialized) {
        *initialized =
            left_initialized &&
            right_initialized;
    }

    return left_ok && right_ok;
}

static void send_release(void)
{
    const char request[] =
        "{\"version\":1,\"method\":\"release\"}\n";

    char path[320];

    socket_path(
        path,
        sizeof(path),
        g_pair_mode ? "joycon-left.sock" : "pro.sock");

    socket_request(path, request, NULL);

    if (g_pair_mode) {
        socket_path(
            path,
            sizeof(path),
            "joycon-right.sock");

        socket_request(path, request, NULL);
    }
}

static int worker(void *unused)
{
    (void)unused;

    for (;;) {
        int8_t state[CONTROLLER_STATE_COUNT];
        int should_send = 0;
        int home = 0;
        int was_ipc_up = 0;

        Uint32 now = SDL_GetTicks();

        SDL_LockMutex(g_lock);

        if (!g_running) {
            SDL_UnlockMutex(g_lock);
            break;
        }

        was_ipc_up = g_ipc_up;

        if (was_ipc_up) {
            should_send =
                g_dirty ||
                now - g_last_send >= PCBLE_KEEPALIVE_MS;
        } else {
            should_send =
                now - g_last_attempt >= PCBLE_RETRY_MS;
        }

        if (should_send) {
            memcpy(state, g_state, sizeof(state));
            home =
                g_home_until != 0 &&
                (Sint32)(g_home_until - now) > 0;

            g_dirty = 0;
            g_last_attempt = now;
        }

        SDL_UnlockMutex(g_lock);

        if (!should_send) {
            SDL_Delay(5);
            continue;
        }

        int initialized = 0;
        int ok =
            send_input(
                state,
                home,
                &initialized);

        now = SDL_GetTicks();

        SDL_LockMutex(g_lock);

        g_ipc_up = ok;
        g_link_up = ok && initialized;

        if (ok) {
            g_last_send = now;
            g_rate_count++;
        }

        if (!g_rate_started) {
            g_rate_started = now;
        }

        Uint32 elapsed = now - g_rate_started;

        if (elapsed >= 1000) {
            g_report_rate =
                elapsed
                    ? ((double)g_rate_count * 1000.0) /
                          (double)elapsed
                    : 0.0;

            g_rate_count = 0;
            g_rate_started = now;
        }

        SDL_UnlockMutex(g_lock);
    }

    return 0;
}

static int output_pcble_init(void)
{
    if (g_thread) {
        return 1;
    }

    char controller[32];

    const char *profile =
        config_get_str(
            "PCBLE_CONTROLLER",
            controller,
            sizeof(controller),
            "pro");

    if (strcmp(profile, "joycon-pair") == 0) {
        g_pair_mode = 1;
    } else if (strcmp(profile, "pro") == 0) {
        g_pair_mode = 0;
    } else {
        fprintf(
            stderr,
            "pcble: unknown PCBLE_CONTROLLER \"%s\"; using pro\n",
            profile);
        g_pair_mode = 0;
    }

    const char *override =
        getenv("CAPTURE2CLOUD_PCBLE_SOCKET_DIR");

    if (override && *override) {
        snprintf(
            g_socket_dir,
            sizeof(g_socket_dir),
            "%s",
            override);
    } else {
        snprintf(
            g_socket_dir,
            sizeof(g_socket_dir),
            "/run/capture2cloud/pcble/%u",
            (unsigned)getuid());
    }

    g_lock = SDL_CreateMutex();
    if (!g_lock) {
        fprintf(
            stderr,
            "pcble: could not create state mutex: %s\n",
            SDL_GetError());
        return 0;
    }

    memset(g_state, 0, sizeof(g_state));

    g_running = 1;
    g_dirty = 1;
    g_ipc_up = 0;
    g_link_up = 0;
    g_home_until = 0;
    g_last_attempt = SDL_GetTicks() - PCBLE_RETRY_MS;
    g_last_send = 0;
    g_rate_started = SDL_GetTicks();
    g_rate_count = 0;
    g_report_rate = 0.0;

    g_thread =
        SDL_CreateThread(
            worker,
            "c2c-pcble",
            NULL);

    if (!g_thread) {
        fprintf(
            stderr,
            "pcble: could not create worker thread: %s\n",
            SDL_GetError());

        SDL_DestroyMutex(g_lock);
        g_lock = NULL;
        g_running = 0;
        return 0;
    }

    fprintf(
        stderr,
        "pcble: backend ready (%s), socket directory %s\n",
        g_pair_mode ? "joycon-pair" : "pro",
        g_socket_dir);

    return 1;
}

static void output_pcble_update(
    const int8_t state[CONTROLLER_STATE_COUNT])
{
    if (!g_lock || !state) {
        return;
    }

    SDL_LockMutex(g_lock);

    memcpy(
        g_state,
        state,
        sizeof(g_state));

    g_dirty = 1;

    SDL_UnlockMutex(g_lock);
}

static void output_pcble_reset(void)
{
    if (!g_lock) {
        return;
    }

    SDL_LockMutex(g_lock);

    memset(
        g_state,
        0,
        sizeof(g_state));

    g_home_until = 0;
    g_dirty = 1;

    SDL_UnlockMutex(g_lock);
}

static void output_pcble_press_home(void)
{
    if (!g_lock) {
        return;
    }

    SDL_LockMutex(g_lock);

    g_home_until =
        SDL_GetTicks() +
        PCBLE_HOME_MS;

    g_dirty = 1;

    SDL_UnlockMutex(g_lock);
}

static int output_pcble_link_up(void)
{
    if (!g_lock) {
        return 0;
    }

    SDL_LockMutex(g_lock);
    int up = g_link_up;
    SDL_UnlockMutex(g_lock);

    return up;
}

static double output_pcble_report_rate(void)
{
    if (!g_lock) {
        return 0.0;
    }

    SDL_LockMutex(g_lock);
    double rate = g_report_rate;
    SDL_UnlockMutex(g_lock);

    return rate;
}

static void output_pcble_shutdown(void)
{
    if (!g_lock) {
        return;
    }

    SDL_LockMutex(g_lock);
    g_running = 0;
    SDL_UnlockMutex(g_lock);

    if (g_thread) {
        SDL_WaitThread(g_thread, NULL);
        g_thread = NULL;
    }

    /*
     * Neutralise the controller before the helper's own five-second
     * desktop lease expires.
     */
    send_release();

    SDL_DestroyMutex(g_lock);
    g_lock = NULL;

    g_ipc_up = 0;
    g_link_up = 0;
    g_report_rate = 0.0;
}

const GamepadOutputBackend *output_pcble_backend(void)
{
    static const GamepadOutputBackend backend = {
        .name = "pcble",
        .init = output_pcble_init,
        .update = output_pcble_update,
        .reset = output_pcble_reset,
        .press_home = output_pcble_press_home,
        .link_up = output_pcble_link_up,
        .report_rate = output_pcble_report_rate,
        .shutdown = output_pcble_shutdown,
    };

    return &backend;
}
