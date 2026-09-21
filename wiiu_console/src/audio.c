#include "audio.h"
#include "c2s_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <whb/log.h>

/*
 * Diagnostic/simple audio path.
 *
 * Host:
 *     S16LE 48 kHz stereo
 *          |
 *       UDP/5084
 *          |
 * Wii U PCM ring
 *          |
 *       1:1 copy
 *          |
 *       SDL2 / AX
 *
 * No Opus.
 * No software ASRC.
 * No artificial low-latency queue trimming.
 *
 * SDL's Wii U backend already programs AX's source-rate conversion.
 */

#define AUDIO_RING_FRAMES 8192

/*
 * UDP is currently drained from the ~60 Hz render loop, so packets
 * naturally arrive at the audio ring in ~15-20 ms batches.
 *
 * 30 ms startup gives that batching a little safety without becoming a
 * large persistent latency reservoir.
 */
#define AUDIO_START_MS 30
#define AUDIO_CALLBACK_MARGIN_MS 8

static SDL_AudioDeviceID g_device;
static int g_rate = 48000;

static int16_t g_ring[
    AUDIO_RING_FRAMES * 2];

static uint32_t g_head;
static uint32_t g_tail;
static uint32_t g_count;

static uint32_t g_callback_frames;

static int g_playing;

static unsigned long g_packets;
static unsigned long g_failed;

/*
 * Now this means a REAL ring overflow only.
 *
 * There is no target+30ms trimming anymore.
 */
static unsigned long g_dropped;

static uint32_t g_underruns;

/* Absolute frame counters used to measure the real rates. */
static uint64_t g_input_total;
static uint64_t g_device_total;
static uint64_t g_used_total;

/* Diagnostic sampling, updated from the UI/main thread. */
static uint32_t g_diag_at;

static uint64_t g_diag_input_at;
static uint64_t g_diag_device_at;
static uint64_t g_diag_used_at;

static unsigned g_diag_input_fps;
static unsigned g_diag_device_fps;
static unsigned g_diag_used_fps;


static uint32_t startup_frames(void)
{
    uint32_t frames =
        (uint32_t)(
            (uint64_t)g_rate *
            AUDIO_START_MS /
            1000u);

    /*
     * Must contain at least one real hardware callback plus a little
     * scheduling margin.
     */
    if (g_callback_frames) {
        const uint32_t minimum =
            g_callback_frames +
            (uint32_t)(
                (uint64_t)g_rate *
                AUDIO_CALLBACK_MARGIN_MS /
                1000u);

        if (frames < minimum) {
            frames = minimum;
        }
    }

    if (frames >= AUDIO_RING_FRAMES) {
        frames =
            AUDIO_RING_FRAMES / 2u;
    }

    return frames;
}


static void audio_callback(void *userdata,
                           Uint8 *stream,
                           int len)
{
    (void)userdata;

    memset(
        stream,
        0,
        (size_t)len);

    if (g_rate <= 0) {
        return;
    }

    const uint32_t out_frames =
        (uint32_t)len /
        ((uint32_t)sizeof(int16_t) * 2u);

    if (!out_frames) {
        return;
    }

    /*
     * This is what SDL/AX ACTUALLY asks us for.
     */
    g_callback_frames =
        out_frames;

    g_device_total +=
        out_frames;

    if (!g_playing) {
        if (g_count <
            startup_frames()) {
            return;
        }

        g_playing = 1;
    }

    if (g_count <
        out_frames) {

        /*
         * Genuine underrun.
         *
         * Do not repeat old samples and do not stretch anything:
         * silence this callback and rebuild the short startup buffer.
         */
        g_underruns++;

        g_playing = 0;

        return;
    }

    int16_t *out =
        (int16_t *)stream;

    /*
     * Strictly 1 source frame -> 1 output frame.
     *
     * No interpolation, no pitch shifting, no phase state.
     */
    for (uint32_t i = 0;
         i < out_frames;
         ++i) {

        const uint32_t src =
            (g_tail + i) %
            AUDIO_RING_FRAMES;

        out[
            (size_t)i * 2u] =
                g_ring[
                    (size_t)src * 2u];

        out[
            (size_t)i * 2u + 1u] =
                g_ring[
                    (size_t)src * 2u + 1u];
    }

    g_tail =
        (g_tail + out_frames) %
        AUDIO_RING_FRAMES;

    g_count -=
        out_frames;

    g_used_total +=
        out_frames;
}


int audio_init(int rate,
               int channels,
               char *why,
               size_t why_size)
{
    if (why &&
        why_size) {
        why[0] = '\0';
    }

    if (g_device) {
        audio_exit();
    }

    if (rate <= 0 ||
        channels != 2) {

        if (why &&
            why_size) {

            snprintf(
                why,
                why_size,
                "PCM expects stereo");
        }

        return -1;
    }

    SDL_AudioSpec want;
    SDL_AudioSpec have;

    memset(
        &want,
        0,
        sizeof(want));

    memset(
        &have,
        0,
        sizeof(have));

    want.freq =
        rate;

    want.format =
        AUDIO_S16SYS;

    want.channels =
        2;

    want.samples =
        256;

    want.callback =
        audio_callback;

    g_device =
        SDL_OpenAudioDevice(
            NULL,
            0,
            &want,
            &have,
            SDL_AUDIO_ALLOW_SAMPLES_CHANGE);

    if (!g_device) {
        if (why &&
            why_size) {

            snprintf(
                why,
                why_size,
                "SDL audio: %s",
                SDL_GetError());
        }

        return -1;
    }

    if (have.freq != rate ||
        have.channels != 2 ||
        have.format != AUDIO_S16SYS) {

        if (why &&
            why_size) {

            snprintf(
                why,
                why_size,
                "unexpected audio %dHz/%uch fmt=%04x",
                have.freq,
                have.channels,
                have.format);
        }

        SDL_CloseAudioDevice(
            g_device);

        g_device = 0;

        return -1;
    }

    g_rate =
        rate;

    g_head = 0;
    g_tail = 0;
    g_count = 0;

    g_callback_frames =
        have.samples;

    g_playing = 0;

    g_packets = 0;
    g_failed = 0;
    g_dropped = 0;
    g_underruns = 0;

    g_input_total = 0;
    g_device_total = 0;
    g_used_total = 0;

    g_diag_at =
        SDL_GetTicks();

    g_diag_input_at = 0;
    g_diag_device_at = 0;
    g_diag_used_at = 0;

    g_diag_input_fps = 0;
    g_diag_device_fps = 0;
    g_diag_used_fps = 0;

    WHBLogPrintf(
        "audio: UDP PCM 1:1 -> SDL/AX %dHz cb=%u start=%ums",
        have.freq,
        have.samples,
        (unsigned)(
            (uint64_t)
                startup_frames() *
            1000ull /
            (uint64_t)g_rate));

    SDL_PauseAudioDevice(
        g_device,
        0);

    return 0;
}


void audio_exit(void)
{
    if (g_device) {
        SDL_PauseAudioDevice(
            g_device,
            1);

        SDL_CloseAudioDevice(
            g_device);

        g_device = 0;
    }

    g_head = 0;
    g_tail = 0;
    g_count = 0;

    g_callback_frames = 0;

    g_playing = 0;
}


void audio_push_pcm_s16le(const uint8_t *data,
                          uint32_t size)
{
    if (!g_device ||
        !data ||
        !size) {
        return;
    }

    if ((size & 3u) != 0) {
        g_failed++;
        return;
    }

    const uint32_t frames =
        size / 4u;

    if (!frames ||
        frames >
            C2S_PCM_UDP_MAX_FRAMES) {

        g_failed++;
        return;
    }

    SDL_LockAudioDevice(
        g_device);

    /*
     * This is the ONLY drop condition now:
     *
     * the physical 8192-frame ring is genuinely full.
     */
    if (frames >
        AUDIO_RING_FRAMES -
        g_count) {

        const uint32_t remove =
            frames -
            (AUDIO_RING_FRAMES -
             g_count);

        g_tail =
            (g_tail + remove) %
            AUDIO_RING_FRAMES;

        g_count -=
            remove;

        g_dropped++;
    }

    for (uint32_t i = 0;
         i < frames;
         ++i) {

        const uint32_t dst =
            (g_head + i) %
            AUDIO_RING_FRAMES;

        for (uint32_t ch = 0;
             ch < 2;
             ++ch) {

            const size_t at =
                ((size_t)i * 2u +
                 (size_t)ch) *
                2u;

            /*
             * Network PCM is S16LE.
             * Wii U PPC is big endian.
             */
            const uint16_t u =
                (uint16_t)data[at] |
                ((uint16_t)data[
                    at + 1u] << 8);

            g_ring[
                (size_t)dst * 2u +
                (size_t)ch] =
                    (int16_t)u;
        }
    }

    g_head =
        (g_head + frames) %
        AUDIO_RING_FRAMES;

    g_count +=
        frames;

    g_packets++;

    g_input_total +=
        frames;

    SDL_UnlockAudioDevice(
        g_device);
}


void audio_stats(unsigned long *packets,
                 unsigned long *failed,
                 unsigned long *dropped)
{
    if (packets) {
        *packets =
            g_packets;
    }

    if (failed) {
        *failed =
            g_failed;
    }

    if (dropped) {
        *dropped =
            g_dropped;
    }
}


void audio_diag(AudioDiag *diag)
{
    if (!diag) {
        return;
    }

    memset(
        diag,
        0,
        sizeof(*diag));

    if (!g_device) {
        return;
    }

    const uint32_t now =
        SDL_GetTicks();

    uint64_t input;
    uint64_t device;
    uint64_t used;

    uint32_t callback_frames;
    uint32_t underruns;

    SDL_LockAudioDevice(
        g_device);

    input =
        g_input_total;

    device =
        g_device_total;

    used =
        g_used_total;

    callback_frames =
        g_callback_frames;

    underruns =
        g_underruns;

    SDL_UnlockAudioDevice(
        g_device);

    const uint32_t elapsed =
        now -
        g_diag_at;

    if (elapsed >= 1000) {
        g_diag_input_fps =
            (unsigned)(
                (input -
                 g_diag_input_at) *
                1000ull /
                elapsed);

        g_diag_device_fps =
            (unsigned)(
                (device -
                 g_diag_device_at) *
                1000ull /
                elapsed);

        g_diag_used_fps =
            (unsigned)(
                (used -
                 g_diag_used_at) *
                1000ull /
                elapsed);

        g_diag_at =
            now;

        g_diag_input_at =
            input;

        g_diag_device_at =
            device;

        g_diag_used_at =
            used;
    }

    diag->input_fps =
        g_diag_input_fps;

    diag->device_fps =
        g_diag_device_fps;

    diag->used_fps =
        g_diag_used_fps;

    diag->callback_frames =
        callback_frames;

    diag->underruns =
        underruns;
}


unsigned audio_queue_ms(void)
{
    if (!g_device ||
        g_rate <= 0) {
        return 0;
    }

    SDL_LockAudioDevice(
        g_device);

    const uint32_t frames =
        g_count;

    SDL_UnlockAudioDevice(
        g_device);

    return (unsigned)(
        (uint64_t)frames *
        1000ull /
        (uint64_t)g_rate);
}
