#include "gamepad_bridge.h"

#include "app_config.h"
#include "output_backend.h"
#include "output_titan.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define GAMEPAD_MAX_SOURCES 8
#define GAMEPAD_SOURCE_TIMEOUT_MS 1500

typedef struct {
    int in_use;
    unsigned key;
    Uint32 last_ms;
    int8_t state[CONTROLLER_STATE_COUNT];
} GamepadSource;

static GamepadSource g_sources[GAMEPAD_MAX_SOURCES];
static SDL_mutex *g_sources_mutex;
static const GamepadOutputBackend *g_backend;

static int is_axis(int i)
{
    return i == CONTROLLER_RX || i == CONTROLLER_RY ||
           i == CONTROLLER_LX || i == CONTROLLER_LY;
}

/* Caller holds g_sources_mutex. */
static void recombine(int8_t merged[CONTROLLER_STATE_COUNT])
{
    const Uint32 now = SDL_GetTicks();

    memset(merged, 0, CONTROLLER_STATE_COUNT);

    for (int s = 0; s < GAMEPAD_MAX_SOURCES; s++) {
        GamepadSource *src = &g_sources[s];

        if (!src->in_use) {
            continue;
        }

        if (now - src->last_ms > GAMEPAD_SOURCE_TIMEOUT_MS) {
            src->in_use = 0;
            continue;
        }

        for (int i = 0; i < CONTROLLER_STATE_COUNT; i++) {
            const int v = src->state[i];

            if (is_axis(i)) {
                if (abs(v) > abs(merged[i])) {
                    merged[i] = (int8_t)v;
                }
            } else if (v > merged[i]) {
                merged[i] = (int8_t)v;
            }
        }
    }
}

static const GamepadOutputBackend *find_backend(const char *name)
{
    const GamepadOutputBackend *backends[] = {
        output_titan_backend(),
    };

    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (backends[i] && strcasecmp(backends[i]->name, name) == 0) {
            return backends[i];
        }
    }

    return NULL;
}

int gamepad_bridge_init(void)
{
    if (g_backend) {
        return 1;
    }

    char backend_buf[32];
    const char *wanted = config_get_str(
        "GAMEPAD_OUTPUT_BACKEND",
        backend_buf,
        sizeof(backend_buf),
        "titan");

    const GamepadOutputBackend *backend = find_backend(wanted);

    if (!backend) {
        fprintf(stderr,
                "gamepad_bridge: unknown output backend \"%s\" "
                "(currently available: titan)\n",
                wanted);
        return 0;
    }

    g_sources_mutex = SDL_CreateMutex();
    if (!g_sources_mutex) {
        fprintf(stderr, "gamepad_bridge: could not create source mutex\n");
        return 0;
    }

    memset(g_sources, 0, sizeof(g_sources));

    if (!backend->init || !backend->init()) {
        SDL_DestroyMutex(g_sources_mutex);
        g_sources_mutex = NULL;
        return 0;
    }

    g_backend = backend;

    fprintf(stderr,
            "gamepad_bridge: output backend \"%s\" active\n",
            g_backend->name);

    return 1;
}

void gamepad_bridge_update(
    unsigned source,
    const int8_t state[GAMEPAD_BRIDGE_STATE_COUNT])
{
    if (!g_backend || !g_sources_mutex || !state) {
        return;
    }

    int8_t merged[CONTROLLER_STATE_COUNT];

    SDL_LockMutex(g_sources_mutex);

    int slot = -1;
    int free_slot = -1;

    for (int i = 0; i < GAMEPAD_MAX_SOURCES; i++) {
        if (g_sources[i].in_use && g_sources[i].key == source) {
            slot = i;
            break;
        }

        if (!g_sources[i].in_use && free_slot < 0) {
            free_slot = i;
        }
    }

    if (slot < 0) {
        slot = free_slot;
    }

    if (slot >= 0) {
        g_sources[slot].in_use = 1;
        g_sources[slot].key = source;
        g_sources[slot].last_ms = SDL_GetTicks();
        memcpy(
            g_sources[slot].state,
            state,
            sizeof(g_sources[slot].state));
    }

    recombine(merged);
    SDL_UnlockMutex(g_sources_mutex);

    if (g_backend->update) {
        g_backend->update(merged);
    }
}

void gamepad_bridge_forget(unsigned source)
{
    if (!g_backend || !g_sources_mutex) {
        return;
    }

    int8_t merged[CONTROLLER_STATE_COUNT];

    SDL_LockMutex(g_sources_mutex);

    for (int i = 0; i < GAMEPAD_MAX_SOURCES; i++) {
        if (g_sources[i].in_use && g_sources[i].key == source) {
            g_sources[i].in_use = 0;
        }
    }

    recombine(merged);
    SDL_UnlockMutex(g_sources_mutex);

    if (g_backend->update) {
        g_backend->update(merged);
    }
}

void gamepad_bridge_reset(void)
{
    if (g_backend && g_backend->reset) {
        g_backend->reset();
    }
}

void gamepad_bridge_press_home(void)
{
    if (g_backend && g_backend->press_home) {
        g_backend->press_home();
    }
}

int gamepad_bridge_link_up(void)
{
    if (!g_backend || !g_backend->link_up) {
        return 0;
    }

    return g_backend->link_up();
}

double gamepad_bridge_report_rate(void)
{
    if (!g_backend || !g_backend->report_rate) {
        return 0.0;
    }

    return g_backend->report_rate();
}

const char *gamepad_bridge_backend_name(void)
{
    return g_backend ? g_backend->name : "";
}

void gamepad_bridge_shutdown(void)
{
    if (g_backend && g_backend->shutdown) {
        g_backend->shutdown();
    }

    g_backend = NULL;

    if (g_sources_mutex) {
        SDL_DestroyMutex(g_sources_mutex);
        g_sources_mutex = NULL;
    }

    memset(g_sources, 0, sizeof(g_sources));
}

/*
 * Titan output-protocol compatibility API.
 *
 * Capture2Cloud already exposes these controls in several places. Keep
 * them working while Titan is the only registered output backend.
 */
const char *gamepad_protocol_name(int value)
{
    return output_titan_protocol_name(value);
}

const char *gamepad_protocol_label(int value)
{
    return output_titan_protocol_label(value);
}

int gamepad_protocol_from_name(const char *name)
{
    return output_titan_protocol_from_name(name);
}

int gamepad_protocol_count(void)
{
    return output_titan_protocol_count();
}

static int using_titan(void)
{
    return g_backend && strcmp(g_backend->name, "titan") == 0;
}

int gamepad_bridge_output_protocol(void)
{
    return using_titan() ? output_titan_output_protocol() : -1;
}

int gamepad_bridge_console(void)
{
    return using_titan() ? output_titan_console() : -1;
}

void gamepad_bridge_request_output_protocol(int value)
{
    if (using_titan()) {
        output_titan_request_output_protocol(value);
    }
}
