#define _GNU_SOURCE

#include "output_pcble.h"

#include "app_config.h"
#include "controller_state.h"

#include <SDL2/SDL.h>
#include <gio/gio.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
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
static char g_peer[32];

static GMutex g_session_lock;
static GSubprocess *g_launcher;
static int g_session_stopping;

/*
 * All reconnect entry points converge here:
 *
 *   GTK Reconnect paired Switch
 *   Maintenance / gamepad_bridge_reset()
 *   C2S RESET_DONGLE
 *   automatic startup recovery
 *
 * The operation is advanced by output_pcble_service() from the normal
 * Capture2Cloud main loop.
 */
static SDL_atomic_t g_reconnect_requested;
static SDL_atomic_t g_reconnect_stop_sent;

#define PCBLE_RUNNER "/usr/local/libexec/capture2cloud/pcble/run-classic.sh"

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
     * These centers and excursions are also advertised through the
     * emulated controller's SPI factory calibration. Keep both sides in
     * sync: the Switch interprets these raw 12-bit values through that
     * calibration table.
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

static void reply_peer(
    const char *reply,
    char *out,
    size_t out_size)
{
    if (!out || !out_size) {
        return;
    }

    out[0] = '\0';

    const char *p = strstr(reply, "\"peer\":\"");
    if (!p) {
        return;
    }

    p += strlen("\"peer\":\"");

    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < out_size) {
        out[n] = p[n];
        n++;
    }

    out[n] = '\0';
}

static int socket_request_timeout(
    const char *path,
    const char *request,
    int *initialized,
    char *peer,
    size_t peer_size,
    int timeout_ms)
{
    if (initialized) {
        *initialized = 0;
    }

    if (peer && peer_size) {
        peer[0] = '\0';
    }

    if (!path || !*path ||
        strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return 0;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }

    if (timeout_ms < 1) {
        timeout_ms = 1;
    }

    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
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
        ssize_t n = read(
            fd,
            reply + used,
            PCBLE_REPLY_MAX - 1 - used);

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

    if (ok && peer && peer_size) {
        reply_peer(reply, peer, peer_size);
    }

    free(reply);
    return ok;
}

static int socket_request(
    const char *path,
    const char *request,
    int *initialized,
    char *peer,
    size_t peer_size)
{
    return socket_request_timeout(
        path,
        request,
        initialized,
        peer,
        peer_size,
        150);
}

static void socket_path(char *out, size_t out_size, const char *name)
{
    snprintf(out, out_size, "%s/%s", g_socket_dir, name);
}

static int send_input(
    const int8_t state[CONTROLLER_STATE_COUNT],
    int home_override,
    int pair_mode,
    int *initialized,
    char *peer,
    size_t peer_size)
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
        pair_mode ? "joycon-left.sock" : "pro.sock");

    int left_ok =
        socket_request(
            path,
            request,
            &left_initialized,
            peer,
            peer_size);

    if (!pair_mode) {
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
            &right_initialized,
            NULL,
            0);

    if (initialized) {
        *initialized =
            left_initialized &&
            right_initialized;
    }

    return left_ok && right_ok;
}

static void send_release(int pair_mode)
{
    const char request[] =
        "{\"version\":1,\"method\":\"release\"}\n";

    char path[320];

    socket_path(
        path,
        sizeof(path),
        pair_mode ? "joycon-left.sock" : "pro.sock");

    socket_request(path, request, NULL, NULL, 0);

    if (pair_mode) {
        socket_path(
            path,
            sizeof(path),
            "joycon-right.sock");

        socket_request(path, request, NULL, NULL, 0);
    }
}

static int worker(void *unused)
{
    (void)unused;

    for (;;) {
        int8_t state[CONTROLLER_STATE_COUNT];
        int should_send = 0;
        int home = 0;
        int pair_mode = 0;
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
            pair_mode = g_pair_mode;
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
        char peer[sizeof(g_peer)] = {0};

        int ok =
            send_input(
                state,
                home,
                pair_mode,
                &initialized,
                peer,
                sizeof(peer));

        now = SDL_GetTicks();

        SDL_LockMutex(g_lock);

        g_ipc_up = ok;
        g_link_up = ok && initialized;

        if (ok && peer[0]) {
            snprintf(g_peer, sizeof(g_peer), "%s", peer);
        } else if (!ok) {
            g_peer[0] = '\0';
        }

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
    g_peer[0] = '\0';
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

    /*
     * Do NOT start pkexec/BlueZ recovery from backend init().
     *
     * At this point the rest of Capture2Cloud, including its desktop
     * integration, is still being constructed. Queue the exact same
     * recovery operation used by the GTK reconnect button instead; the
     * main loop will execute it immediately afterwards.
     */
    SDL_AtomicSet(
        &g_reconnect_requested,
        g_pair_mode ? 0 : 1);

    SDL_AtomicSet(
        &g_reconnect_stop_sent,
        0);

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
    /*
     * Backend recovery deliberately means a FRESH --reconnect session.
     *
     * Do not use the experimental in-session reconnect IPC path here.
     * The known-good path is the one used by "Reconnect paired Switch":
     * cleanly stop the helper, let run-classic.sh restore BlueZ, then
     * launch it again with --reconnect.
     */
    SDL_AtomicSet(
        &g_reconnect_stop_sent,
        0);

    SDL_AtomicSet(
        &g_reconnect_requested,
        1);

    fprintf(
        stderr,
        "pcble: fresh paired-Switch reconnect requested\n");
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
     * Capture2Cloud owns the Bluetooth session it started. Ask the helper
     * for a clean shutdown so BlueZ is restored immediately.
     */
    send_release(g_pair_mode);
    output_pcble_stop_session();

    SDL_DestroyMutex(g_lock);
    g_lock = NULL;

    g_ipc_up = 0;
    g_link_up = 0;
    g_report_rate = 0.0;
}


static int valid_hci(const char *id)
{
    if (!id || strncmp(id, "hci", 3) != 0 || !isdigit((unsigned char)id[3])) {
        return 0;
    }

    for (const char *p = id + 3; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }

    return 1;
}

static int valid_mac(const char *address)
{
    if (!address || strlen(address) != 17) {
        return 0;
    }

    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (address[i] != ':') {
                return 0;
            }
        } else if (!isxdigit((unsigned char)address[i])) {
            return 0;
        }
    }

    return 1;
}

static int valid_color(const char *value)
{
    if (!value || strlen(value) != 6) {
        return 0;
    }

    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)value[i])) {
            return 0;
        }
    }

    return 1;
}

int output_pcble_scan_adapters(
    OutputPcbleAdapter adapters[OUTPUT_PCBLE_MAX_ADAPTERS],
    char *error,
    size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }

    GError *gerror = NULL;
    GDBusConnection *bus =
        g_bus_get_sync(
            G_BUS_TYPE_SYSTEM,
            NULL,
            &gerror);

    if (!bus) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "%s",
                gerror ? gerror->message : "Could not open system D-Bus");
        }
        g_clear_error(&gerror);
        return -1;
    }

    GVariant *reply =
        g_dbus_connection_call_sync(
            bus,
            "org.bluez",
            "/",
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects",
            NULL,
            G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
            0,
            1500,
            NULL,
            &gerror);

    g_object_unref(bus);

    if (!reply) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "%s",
                gerror ? gerror->message : "BlueZ did not answer");
        }
        g_clear_error(&gerror);
        return -1;
    }

    GVariantIter *objects = NULL;
    g_variant_get(reply, "(a{oa{sa{sv}}})", &objects);

    int count = 0;
    char *path = NULL;
    GVariant *interfaces = NULL;

    while (g_variant_iter_next(
            objects,
            "{o@a{sa{sv}}}",
            &path,
            &interfaces)) {
        GVariant *props =
            g_variant_lookup_value(
                interfaces,
                "org.bluez.Adapter1",
                G_VARIANT_TYPE_VARDICT);

        if (props && count < OUTPUT_PCBLE_MAX_ADAPTERS) {
            const char *address = "";

            g_variant_lookup(
                props,
                "Address",
                "&s",
                &address);

            char *id = g_path_get_basename(path);

            snprintf(
                adapters[count].id,
                sizeof(adapters[count].id),
                "%s",
                id);

            snprintf(
                adapters[count].address,
                sizeof(adapters[count].address),
                "%s",
                address);

            count++;

            g_free(id);
        }

        if (props) {
            g_variant_unref(props);
        }

        g_free(path);
        g_variant_unref(interfaces);
    }

    g_variant_iter_free(objects);
    g_variant_unref(reply);

    return count;
}


static int output_pcble_start_saved_reconnect(
    char *error,
    size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }

    char profile[32];
    char peer[32];
    char paired_adapter[32];

    config_get_str(
        "PCBLE_CONTROLLER",
        profile,
        sizeof(profile),
        "pro");

    if (strcmp(profile, "pro") != 0) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "Automatic reconnect is currently available for Pro Controller sessions only");
        }

        return 0;
    }

    config_get_str(
        "PCBLE_SWITCH_ADDRESS",
        peer,
        sizeof(peer),
        "none");

    config_get_str(
        "PCBLE_PAIRED_ADAPTER",
        paired_adapter,
        sizeof(paired_adapter),
        "none");

    if (!valid_mac(peer)) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "No paired Switch is stored yet");
        }

        return 0;
    }

    if (!valid_mac(paired_adapter)) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "The Bluetooth adapter used for pairing is not stored");
        }

        return 0;
    }

    /*
     * This is deliberately the exact operation performed by the known
     * good GTK reconnect path:
     *
     *   resolve saved physical adapter MAC -> current hciN
     *   output_pcble_start_session(..., peer)
     *
     * No alternate in-session reconnect algorithm belongs here.
     */
    OutputPcbleAdapter adapters[OUTPUT_PCBLE_MAX_ADAPTERS];

    char scan_error[256];

    const int count =
        output_pcble_scan_adapters(
            adapters,
            scan_error,
            sizeof(scan_error));

    if (count < 0) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "%s",
                scan_error[0]
                    ? scan_error
                    : "Could not scan Bluetooth adapters");
        }

        return 0;
    }

    int selected = -1;

    for (int i = 0; i < count; i++) {
        if (g_ascii_strcasecmp(
                adapters[i].address,
                paired_adapter) == 0) {
            selected = i;
            break;
        }
    }

    if (selected < 0) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "The Bluetooth adapter used for pairing is not currently present");
        }

        return 0;
    }

    char body[16];
    char buttons[16];
    char left[16];
    char right[16];

    config_get_str(
        "PCBLE_BODY_COLOR",
        body,
        sizeof(body),
        "828282");

    config_get_str(
        "PCBLE_BUTTON_COLOR",
        buttons,
        sizeof(buttons),
        "0F0F0F");

    config_get_str(
        "PCBLE_LEFT_GRIP_COLOR",
        left,
        sizeof(left),
        "828282");

    config_get_str(
        "PCBLE_RIGHT_GRIP_COLOR",
        right,
        sizeof(right),
        "828282");

    const int verbose =
        (int)config_get_int(
            "PCBLE_VERBOSE",
            0,
            0,
            1);

    /*
     * Keep the normal preferred-adapter setting aligned with the
     * adapter that owns this pairing.
     */
    config_set_str(
        "PCBLE_PRIMARY_ADAPTER",
        paired_adapter);

    const int ok =
        output_pcble_start_session(
            adapters[selected].id,
            NULL,
            "pro",
            peer,
            body,
            buttons,
            left,
            right,
            verbose,
            error,
            error_size);

    if (ok) {
        fprintf(
            stderr,
            "pcble: fresh --reconnect session for %s on %s (%s)\n",
            peer,
            adapters[selected].id,
            adapters[selected].address);
    }

    return ok;
}


static void output_pcble_service(void)
{
    if (!SDL_AtomicGet(
            &g_reconnect_requested)) {
        return;
    }

    /*
     * A stale/disconnected helper may still own BlueZ. This is the
     * critical difference from the old implementation:
     *
     * do exactly what a person does before pressing Reconnect again:
     * stop the old session completely first.
     */
    if (output_pcble_session_running()) {
        if (SDL_AtomicCAS(
                &g_reconnect_stop_sent,
                0,
                1)) {
            fprintf(
                stderr,
                "pcble: stopping existing session before fresh reconnect\n");

            output_pcble_stop_session();
        }

        return;
    }

    /*
     * g_launcher disappears only after run-classic.sh has exited, whose
     * EXIT cleanup restores the normal BlueZ service. Therefore reaching
     * here means it is safe to launch the same --reconnect session as the
     * GTK button.
     */
    char error[256];

    const int ok =
        output_pcble_start_saved_reconnect(
            error,
            sizeof(error));

    SDL_AtomicSet(
        &g_reconnect_requested,
        0);

    SDL_AtomicSet(
        &g_reconnect_stop_sent,
        0);

    if (!ok) {
        fprintf(
            stderr,
            "pcble: paired-Switch reconnect failed: %s\n",
            error[0]
                ? error
                : "unknown error");
    }
}


int output_pcble_set_controller(const char *profile)
{
    int pair_mode;

    if (!profile || strcmp(profile, "pro") == 0) {
        pair_mode = 0;
    } else if (strcmp(profile, "joycon-pair") == 0) {
        pair_mode = 1;
    } else {
        return 0;
    }

    if (!g_lock) {
        g_pair_mode = pair_mode;
        return 1;
    }

    SDL_LockMutex(g_lock);

    g_pair_mode = pair_mode;
    g_ipc_up = 0;
    g_link_up = 0;
    g_peer[0] = '\0';
    g_dirty = 1;

    SDL_UnlockMutex(g_lock);

    return 1;
}

static void launcher_finished(
    GObject *source,
    GAsyncResult *result,
    gpointer unused)
{
    (void)unused;

    GError *error = NULL;

    if (!g_subprocess_wait_check_finish(
            G_SUBPROCESS(source),
            result,
            &error)) {
        g_mutex_lock(&g_session_lock);
        int stopping = g_session_stopping;
        g_mutex_unlock(&g_session_lock);

        if (!stopping && error) {
            fprintf(
                stderr,
                "pcble: session launcher exited: %s\n",
                error->message);
        }
    }

    g_clear_error(&error);

    g_mutex_lock(&g_session_lock);

    if (g_launcher == G_SUBPROCESS(source)) {
        g_clear_object(&g_launcher);
    }

    g_session_stopping = 0;

    g_mutex_unlock(&g_session_lock);
}

int output_pcble_start_session(
    const char *primary_adapter,
    const char *secondary_adapter,
    const char *profile,
    const char *reconnect_address,
    const char *body_color,
    const char *button_color,
    const char *left_grip_color,
    const char *right_grip_color,
    int verbose,
    char *error,
    size_t error_size)
{
    if (error && error_size) {
        error[0] = '\0';
    }

    if (!valid_hci(primary_adapter)) {
        if (error && error_size) {
            snprintf(error, error_size, "Invalid primary Bluetooth adapter");
        }
        return 0;
    }

    const int pair_mode =
        profile &&
        strcmp(profile, "joycon-pair") == 0;

    if (!profile ||
        (strcmp(profile, "pro") != 0 && !pair_mode)) {
        if (error && error_size) {
            snprintf(error, error_size, "Invalid controller profile");
        }
        return 0;
    }

    if (pair_mode &&
        (!valid_hci(secondary_adapter) ||
         strcmp(primary_adapter, secondary_adapter) == 0)) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "Joy-Con pair requires two distinct Bluetooth adapters");
        }
        return 0;
    }

    if (reconnect_address &&
        *reconnect_address &&
        !valid_mac(reconnect_address)) {
        if (error && error_size) {
            snprintf(error, error_size, "Invalid paired Switch address");
        }
        return 0;
    }

    if (!pair_mode &&
        (!valid_color(body_color) ||
         !valid_color(button_color) ||
         !valid_color(left_grip_color) ||
         !valid_color(right_grip_color))) {
        if (error && error_size) {
            snprintf(error, error_size, "Invalid Pro Controller color");
        }
        return 0;
    }

    if (access(PCBLE_RUNNER, X_OK) != 0) {
        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "Bluetooth backend is not installed. Run scripts/install_pcble_backend.sh");
        }
        return 0;
    }

    g_mutex_lock(&g_session_lock);

    if (g_launcher) {
        g_mutex_unlock(&g_session_lock);

        if (error && error_size) {
            snprintf(error, error_size, "A Bluetooth session is already running");
        }
        return 0;
    }

    const char *args[32];
    int n = 0;

    args[n++] = "pkexec";
    args[n++] = PCBLE_RUNNER;
    args[n++] = primary_adapter;
    args[n++] = "--desktop";
    args[n++] = "--profile";
    args[n++] = profile;

    if (pair_mode) {
        args[n++] = "--secondary";
        args[n++] = secondary_adapter;
    } else {
        args[n++] = "--body-color";
        args[n++] = body_color;
        args[n++] = "--button-color";
        args[n++] = button_color;
        args[n++] = "--left-grip-color";
        args[n++] = left_grip_color;
        args[n++] = "--right-grip-color";
        args[n++] = right_grip_color;
    }

    if (reconnect_address && *reconnect_address) {
        args[n++] = "--reconnect";
        args[n++] = reconnect_address;
    }

    if (verbose) {
        args[n++] = "--verbose";
    }

    args[n] = NULL;

    GError *gerror = NULL;

    g_launcher =
        g_subprocess_newv(
            args,
            G_SUBPROCESS_FLAGS_NONE,
            &gerror);

    if (!g_launcher) {
        g_mutex_unlock(&g_session_lock);

        if (error && error_size) {
            snprintf(
                error,
                error_size,
                "%s",
                gerror ? gerror->message : "Could not start Bluetooth session");
        }

        g_clear_error(&gerror);
        return 0;
    }

    g_session_stopping = 0;

    g_subprocess_wait_check_async(
        g_launcher,
        NULL,
        launcher_finished,
        NULL);

    g_mutex_unlock(&g_session_lock);

    output_pcble_set_controller(profile);

    fprintf(
        stderr,
        "pcble: starting %s session on %s%s%s\n",
        profile,
        primary_adapter,
        pair_mode ? " + " : "",
        pair_mode ? secondary_adapter : "");

    return 1;
}

void output_pcble_stop_session(void)
{
    int pair_mode = 0;

    if (g_lock) {
        SDL_LockMutex(g_lock);
        pair_mode = g_pair_mode;
        SDL_UnlockMutex(g_lock);
    } else {
        pair_mode = g_pair_mode;
    }

    const char request[] =
        "{\"version\":1,\"method\":\"stop\"}\n";

    char path[320];

    socket_path(
        path,
        sizeof(path),
        pair_mode ? "joycon-left.sock" : "pro.sock");

    int stopped =
        socket_request(
            path,
            request,
            NULL,
            NULL,
            0);

    if (pair_mode) {
        socket_path(
            path,
            sizeof(path),
            "joycon-right.sock");

        stopped |=
            socket_request(
                path,
                request,
                NULL,
                NULL,
                0);
    }

    g_mutex_lock(&g_session_lock);

    g_session_stopping = 1;

    if (!stopped && g_launcher) {
        g_subprocess_send_signal(g_launcher, SIGTERM);
    }

    g_mutex_unlock(&g_session_lock);
}

int output_pcble_ipc_up(void)
{
    if (!g_lock) {
        return 0;
    }

    SDL_LockMutex(g_lock);
    int up = g_ipc_up;
    SDL_UnlockMutex(g_lock);

    return up;
}

int output_pcble_session_running(void)
{
    g_mutex_lock(&g_session_lock);
    int running = g_launcher != NULL;
    g_mutex_unlock(&g_session_lock);

    return running;
}

void output_pcble_peer(char *out, size_t out_size)
{
    if (!out || !out_size) {
        return;
    }

    out[0] = '\0';

    if (!g_lock) {
        return;
    }

    SDL_LockMutex(g_lock);
    snprintf(out, out_size, "%s", g_peer);
    SDL_UnlockMutex(g_lock);
}

const GamepadOutputBackend *output_pcble_backend(void)
{
    static const GamepadOutputBackend backend = {
        .name = "pcble",
        .init = output_pcble_init,
        .update = output_pcble_update,
        .reset = output_pcble_reset,
        .press_home = output_pcble_press_home,
        .service = output_pcble_service,
        .link_up = output_pcble_link_up,
        .report_rate = output_pcble_report_rate,
        .shutdown = output_pcble_shutdown,
    };

    return &backend;
}
