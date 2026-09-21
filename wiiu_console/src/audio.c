#include "audio.h"
#include "c2s_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <whb/log.h>

#define AUDIO_RING_FRAMES 8192

#define AUDIO_TARGET_MS       25
#define AUDIO_MARGIN_MS        5
#define AUDIO_HARD_EXTRA_MS   30

#define AUDIO_PPM_PER_FRAME    4
#define AUDIO_MAX_PPM       5000
#define AUDIO_SLEW_PPM        20

#define PHASE_ONE (UINT64_C(1) << 32)

static SDL_AudioDeviceID g_device;
static int g_rate = 48000;

static int16_t g_ring[AUDIO_RING_FRAMES * 2];

static uint32_t g_head;
static uint32_t g_tail;
static uint32_t g_count;

static uint32_t g_callback_frames;

static int g_playing;
static uint64_t g_phase;
static int32_t g_ratio_ppm;

static unsigned long g_packets;
static unsigned long g_failed;
static unsigned long g_dropped;


static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}


static uint32_t target_frames(void)
{
    uint32_t target =
        (uint32_t)(
            (uint64_t)g_rate *
            AUDIO_TARGET_MS /
            1000u);

    if (g_callback_frames) {
        const uint32_t minimum =
            g_callback_frames +
            (uint32_t)(
                (uint64_t)g_rate *
                AUDIO_MARGIN_MS /
                1000u);

        if (target < minimum) {
            target = minimum;
        }
    }

    if (target > AUDIO_RING_FRAMES / 2u) {
        target = AUDIO_RING_FRAMES / 2u;
    }

    return target;
}


static int16_t sample_at(uint32_t offset,
                         int channel)
{
    const uint32_t frame =
        (g_tail + offset) %
        AUDIO_RING_FRAMES;

    return g_ring[
        (size_t)frame * 2u +
        (size_t)channel];
}


static void audio_callback(void *userdata,
                           Uint8 *stream,
                           int len)
{
    (void)userdata;

    memset(stream, 0, (size_t)len);

    const uint32_t out_frames =
        (uint32_t)len /
        (sizeof(int16_t) * 2u);

    if (!out_frames ||
        g_rate <= 0) {
        return;
    }

    g_callback_frames =
        out_frames;

    const uint32_t target =
        target_frames();

    if (!g_playing) {
        if (g_count < target) {
            return;
        }

        g_playing = 1;
        g_phase = 0;
        g_ratio_ppm = 0;
    }

    /*
     * Queue depth controls a real continuous source-rate ratio.
     * Unlike the old +/-1 frame per callback scheme, correction strength
     * does not depend on SDL's callback size.
     */
    const int error =
        (int)g_count -
        (int)target;

    int wanted_ppm =
        error *
        AUDIO_PPM_PER_FRAME;

    wanted_ppm =
        clamp_int(
            wanted_ppm,
            -AUDIO_MAX_PPM,
            AUDIO_MAX_PPM);

    int delta =
        wanted_ppm -
        g_ratio_ppm;

    delta =
        clamp_int(
            delta,
            -AUDIO_SLEW_PPM,
            AUDIO_SLEW_PPM);

    g_ratio_ppm += delta;

    const int64_t correction =
        ((int64_t)PHASE_ONE *
         g_ratio_ppm) /
        1000000ll;

    const uint64_t step =
        (uint64_t)(
            (int64_t)PHASE_ONE +
            correction);

    const uint64_t last =
        g_phase +
        step *
        (uint64_t)(
            out_frames - 1u);

    const uint32_t required =
        (uint32_t)(
            last >> 32) +
        2u;

    if (g_count < required) {
        g_playing = 0;
        g_phase = 0;
        g_ratio_ppm = 0;
        return;
    }

    int16_t *out =
        (int16_t *)stream;

    uint64_t phase =
        g_phase;

    for (uint32_t i = 0;
         i < out_frames;
         ++i) {

        const uint32_t a =
            (uint32_t)(
                phase >> 32);

        const uint32_t frac =
            (uint32_t)phase;

        const uint32_t b =
            a + 1u;

        for (int ch = 0;
             ch < 2;
             ++ch) {

            const int32_t sa =
                sample_at(a, ch);

            const int32_t sb =
                sample_at(b, ch);

            out[
                (size_t)i * 2u +
                (size_t)ch] =
                    (int16_t)(
                        sa +
                        (int32_t)(
                            ((int64_t)
                                 (sb - sa) *
                             frac) >>
                            32));
        }

        phase += step;
    }

    const uint32_t consumed =
        (uint32_t)(
            phase >> 32);

    g_phase =
        phase &
        (PHASE_ONE - 1u);

    if (consumed > g_count) {
        g_playing = 0;
        g_phase = 0;
        g_ratio_ppm = 0;
        return;
    }

    g_tail =
        (g_tail + consumed) %
        AUDIO_RING_FRAMES;

    g_count -= consumed;
}


int audio_init(int rate,
               int channels,
               char *why,
               size_t why_size)
{
    if (why && why_size) {
        why[0] = '\0';
    }

    if (g_device) {
        audio_exit();
    }

    if (rate <= 0 ||
        channels != 2) {
        if (why && why_size) {
            snprintf(
                why,
                why_size,
                "PCM expects stereo");
        }
        return -1;
    }

    SDL_AudioSpec want;
    SDL_AudioSpec have;

    memset(&want, 0, sizeof(want));
    memset(&have, 0, sizeof(have));

    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 256;
    want.callback = audio_callback;

    g_device =
        SDL_OpenAudioDevice(
            NULL,
            0,
            &want,
            &have,
            SDL_AUDIO_ALLOW_SAMPLES_CHANGE);

    if (!g_device) {
        if (why && why_size) {
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

        if (why && why_size) {
            snprintf(
                why,
                why_size,
                "unexpected audio %dHz/%uch fmt=%04x",
                have.freq,
                have.channels,
                have.format);
        }

        SDL_CloseAudioDevice(g_device);
        g_device = 0;
        return -1;
    }

    g_rate = rate;

    g_head = 0;
    g_tail = 0;
    g_count = 0;

    g_callback_frames =
        have.samples;

    g_playing = 0;
    g_phase = 0;
    g_ratio_ppm = 0;

    g_packets = 0;
    g_failed = 0;
    g_dropped = 0;

    WHBLogPrintf(
        "audio: UDP PCM -> AX %dHz samples=%u target=%ums",
        have.freq,
        have.samples,
        (unsigned)(
            (uint64_t)
                target_frames() *
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
        SDL_PauseAudioDevice(g_device, 1);
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }

    g_head = 0;
    g_tail = 0;
    g_count = 0;

    g_callback_frames = 0;

    g_playing = 0;
    g_phase = 0;
    g_ratio_ppm = 0;
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

    SDL_LockAudioDevice(g_device);

    const uint32_t target =
        target_frames();

    uint32_t hard_max =
        target +
        (uint32_t)(
            (uint64_t)g_rate *
            AUDIO_HARD_EXTRA_MS /
            1000u);

    if (hard_max >
        AUDIO_RING_FRAMES - frames) {
        hard_max =
            AUDIO_RING_FRAMES - frames;
    }

    /*
     * One exceptional live-edge recovery rather than hundreds of tiny
     * overflow drops.
     */
    if (g_count + frames >
            hard_max &&
        g_count > target) {

        const uint32_t remove =
            g_count -
            target;

        g_tail =
            (g_tail + remove) %
            AUDIO_RING_FRAMES;

        g_count -= remove;

        g_phase = 0;
        g_ratio_ppm = 0;

        g_dropped++;
    }

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

        g_count -= remove;

        g_phase = 0;
        g_ratio_ppm = 0;

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
                 ch) *
                2u;

            const uint16_t u =
                (uint16_t)data[at] |
                ((uint16_t)data[
                    at + 1u] << 8);

            g_ring[
                (size_t)dst * 2u +
                ch] =
                    (int16_t)u;
        }
    }

    g_head =
        (g_head + frames) %
        AUDIO_RING_FRAMES;

    g_count += frames;
    g_packets++;

    SDL_UnlockAudioDevice(g_device);
}


void audio_stats(unsigned long *packets,
                 unsigned long *failed,
                 unsigned long *dropped)
{
    if (packets) {
        *packets = g_packets;
    }

    if (failed) {
        *failed = g_failed;
    }

    if (dropped) {
        *dropped = g_dropped;
    }
}


unsigned audio_queue_ms(void)
{
    if (!g_device ||
        g_rate <= 0) {
        return 0;
    }

    SDL_LockAudioDevice(g_device);

    const uint32_t frames =
        g_count;

    SDL_UnlockAudioDevice(g_device);

    return (unsigned)(
        (uint64_t)frames *
        1000ull /
        (uint64_t)g_rate);
}
