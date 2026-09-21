#include "audio.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <opus/opus.h>
#include <whb/log.h>

/*
 * Audio is the continuous clock.
 *
 * The old code used SDL_QueueAudio and let latency climb to 120 ms,
 * then dropped a complete Opus packet. Any small clock difference
 * between the host and the Wii U therefore eventually became an
 * audible discontinuity.
 *
 * Keep a small PCM ring instead. The SDL callback consumes it at the
 * hardware clock rate and very gently changes its input/output ratio
 * when the ring moves away from the target.
 */

#define AUDIO_RING_FRAMES 16384

#define AUDIO_TARGET_MS 35
#define AUDIO_START_MS  25

/*
 * Maximum drift correction per SDL callback.
 *
 * At 512 output frames, +/-4 is below 0.8 %, and normally the actual
 * correction is only one frame.
 */
/*
 * Normal correction stays tiny, but a queue that has already wandered
 * far from the live edge must be allowed to recover in seconds rather
 * than minutes.
 */
#define AUDIO_MAX_NORMAL_SLIP   12
#define AUDIO_MAX_MEDIUM_SLIP   24
#define AUDIO_MAX_CATCHUP_SLIP  48
#define AUDIO_MAX_STRETCH_SLIP   8

static OpusDecoder *g_decoder;
static SDL_AudioDeviceID g_device;

static int g_rate = 48000;
static int g_channels = 2;

static int16_t g_ring[AUDIO_RING_FRAMES * 2];

static uint32_t g_head;
static uint32_t g_tail;
static uint32_t g_count;

static int g_playing;

static unsigned long g_decoded;
static unsigned long g_failed;
static unsigned long g_dropped;


/*
 * Return one interleaved sample from the ring without moving its tail.
 */
static int16_t ring_sample(uint32_t frame_offset,
                           int channel)
{
    const uint32_t frame =
        (g_tail + frame_offset) %
        AUDIO_RING_FRAMES;

    return g_ring[
        (size_t)frame *
        (size_t)g_channels +
        (size_t)channel];
}


/*
 * Tiny linear resampler used only for clock correction.
 *
 * Most callbacks are exactly 1:1.
 * If the PCM ring slowly grows, consume 1-4 extra source frames.
 * If it slowly shrinks, consume 1-4 fewer.
 *
 * No packet is abruptly removed from the middle of playback.
 */
static void audio_callback(void *userdata,
                           Uint8 *stream,
                           int len)
{
    (void)userdata;

    memset(stream, 0, (size_t)len);

    if (g_channels <= 0 ||
        g_rate <= 0) {
        return;
    }

    const uint32_t out_frames =
        (uint32_t)len /
        ((uint32_t)sizeof(int16_t) *
         (uint32_t)g_channels);

    if (!out_frames) {
        return;
    }

    const uint32_t start_frames =
        (uint32_t)(
            (uint64_t)g_rate *
            AUDIO_START_MS /
            1000u);

    const uint32_t target_frames =
        (uint32_t)(
            (uint64_t)g_rate *
            AUDIO_TARGET_MS /
            1000u);

    /*
     * On initial startup or after a real underrun, output silence until
     * there is enough PCM to resume cleanly.
     */
    if (!g_playing) {
        if (g_count < start_frames) {
            return;
        }

        g_playing = 1;
    }

    if (g_count < out_frames) {
        g_playing = 0;
        return;
    }

    int adjust = 0;

    /*
     * Proportional drift controller.
     *
     * Around the 35 ms target it changes only a handful of source
     * frames per callback. If the queue has already reached hundreds of
     * milliseconds it is deliberately allowed to catch up much faster.
     *
     * Linear interpolation below makes this a short smooth time-scale
     * correction instead of an audible discontinuity.
     */
    const int step_frames =
        g_rate / 400;  /* about 2.5 ms */

    const int error =
        (int)g_count -
        (int)target_frames;

    if (step_frames > 0) {
        if (error > step_frames) {
            int max_slip =
                AUDIO_MAX_NORMAL_SLIP;

            if (error >
                g_rate * 120 / 1000) {

                max_slip =
                    AUDIO_MAX_CATCHUP_SLIP;

            } else if (error >
                       g_rate * 60 / 1000) {

                max_slip =
                    AUDIO_MAX_MEDIUM_SLIP;
            }

            adjust =
                error /
                step_frames;

            if (adjust > max_slip) {
                adjust = max_slip;
            }

        } else if (error < -step_frames) {
            adjust =
                -((-error) /
                  step_frames);

            if (adjust <
                -AUDIO_MAX_STRETCH_SLIP) {

                adjust =
                    -AUDIO_MAX_STRETCH_SLIP;
            }
        }
    }

    int consume =
        (int)out_frames +
        adjust;

    if (consume < 1) {
        consume = 1;
    }

    if ((uint32_t)consume > g_count) {
        consume = (int)g_count;
    }

    int16_t *out =
        (int16_t *)stream;

    /*
     * Map 'consume' input frames onto exactly 'out_frames' output
     * frames. With consume==out_frames this is effectively a copy.
     */
    if (out_frames == 1) {
        for (int ch = 0;
             ch < g_channels;
             ++ch) {
            out[ch] =
                ring_sample(0, ch);
        }
    } else {
        const uint64_t scale =
            ((uint64_t)(consume - 1) << 16) /
            (uint64_t)(out_frames - 1);

        for (uint32_t i = 0;
             i < out_frames;
             ++i) {

            const uint64_t pos =
                (uint64_t)i * scale;

            const uint32_t a =
                (uint32_t)(pos >> 16);

            const uint32_t frac =
                (uint32_t)(pos & 0xffffu);

            uint32_t b = a + 1;

            if (b >= (uint32_t)consume) {
                b = (uint32_t)consume - 1;
            }

            for (int ch = 0;
                 ch < g_channels;
                 ++ch) {

                const int32_t sa =
                    ring_sample(a, ch);

                const int32_t sb =
                    ring_sample(b, ch);

                const int32_t mixed =
                    sa +
                    (int32_t)(
                        ((int64_t)(sb - sa) *
                         (int64_t)frac) >>
                        16);

                out[
                    (size_t)i *
                    (size_t)g_channels +
                    (size_t)ch] =
                        (int16_t)mixed;
            }
        }
    }

    g_tail =
        (g_tail + (uint32_t)consume) %
        AUDIO_RING_FRAMES;

    g_count -=
        (uint32_t)consume;
}


int audio_init(int rate,
               int channels,
               char *why,
               size_t why_size)
{
    if (why && why_size) {
        why[0] = '\0';
    }

    if (g_device || g_decoder) {
        audio_exit();
    }

    if (channels != 2) {
        if (why && why_size) {
            snprintf(
                why,
                why_size,
                "audio ring expects stereo");
        }

        return -1;
    }

    int opus_error =
        OPUS_OK;

    g_decoder =
        opus_decoder_create(
            rate,
            channels,
            &opus_error);

    if (!g_decoder ||
        opus_error != OPUS_OK) {

        if (why && why_size) {
            snprintf(
                why,
                why_size,
                "Opus: %s",
                opus_strerror(opus_error));
        }

        g_decoder = NULL;
        return -1;
    }

    SDL_AudioSpec want;
    SDL_AudioSpec have;

    memset(&want, 0, sizeof(want));
    memset(&have, 0, sizeof(have));

    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = 512;

    want.callback =
        audio_callback;

    want.userdata =
        NULL;

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

        opus_decoder_destroy(
            g_decoder);

        g_decoder = NULL;

        return -1;
    }

    if (have.freq != rate ||
        have.channels != channels ||
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

        SDL_CloseAudioDevice(
            g_device);

        g_device = 0;

        opus_decoder_destroy(
            g_decoder);

        g_decoder = NULL;

        return -1;
    }

    g_rate = rate;
    g_channels = channels;

    g_head = 0;
    g_tail = 0;
    g_count = 0;

    g_playing = 0;

    g_decoded = 0;
    g_failed = 0;
    g_dropped = 0;

    WHBLogPrintf(
        "audio: callback ring %dHz stereo samples=%u target=%dms",
        have.freq,
        have.samples,
        AUDIO_TARGET_MS);

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

    if (g_decoder) {
        opus_decoder_destroy(
            g_decoder);

        g_decoder = NULL;
    }

    g_head = 0;
    g_tail = 0;
    g_count = 0;
    g_playing = 0;
}


void audio_decode(const uint8_t *data,
                  uint32_t size)
{
    if (!g_decoder ||
        !g_device ||
        !data ||
        size == 0) {
        return;
    }

    /*
     * Opus maximum packet duration is 120 ms.
     */
    static int16_t pcm[
        5760 * 2];

    const int capacity =
        (int)(
            sizeof(pcm) /
            sizeof(pcm[0]) /
            g_channels);

    const int frames =
        opus_decode(
            g_decoder,
            data,
            (opus_int32)size,
            pcm,
            capacity,
            0);

    if (frames <= 0) {
        g_failed++;
        return;
    }

    g_decoded++;

    SDL_LockAudioDevice(
        g_device);

    uint32_t incoming =
        (uint32_t)frames;

    /*
     * Emergency protection only.
     *
     * The normal drift correction keeps us nowhere near this point.
     * If something stalls for hundreds of milliseconds, keep the live
     * edge rather than overwriting ring memory.
     */
    if (incoming >
        AUDIO_RING_FRAMES) {

        incoming =
            AUDIO_RING_FRAMES;

        g_dropped++;
    }

    const uint32_t free_frames =
        AUDIO_RING_FRAMES -
        g_count;

    if (incoming >
        free_frames) {

        const uint32_t remove =
            incoming -
            free_frames;

        g_tail =
            (g_tail + remove) %
            AUDIO_RING_FRAMES;

        g_count -= remove;

        g_dropped++;
    }

    for (uint32_t i = 0;
         i < incoming;
         ++i) {

        const uint32_t dst =
            (g_head + i) %
            AUDIO_RING_FRAMES;

        for (int ch = 0;
             ch < g_channels;
             ++ch) {

            g_ring[
                (size_t)dst *
                (size_t)g_channels +
                (size_t)ch] =
                    pcm[
                        (size_t)i *
                        (size_t)g_channels +
                        (size_t)ch];
        }
    }

    g_head =
        (g_head + incoming) %
        AUDIO_RING_FRAMES;

    g_count += incoming;

    SDL_UnlockAudioDevice(
        g_device);
}


void audio_stats(unsigned long *decoded,
                 unsigned long *failed,
                 unsigned long *dropped)
{
    if (decoded) {
        *decoded = g_decoded;
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

    SDL_LockAudioDevice(
        g_device);

    const uint32_t frames =
        g_count;

    SDL_UnlockAudioDevice(
        g_device);

    return (unsigned)(
        ((uint64_t)frames *
         1000ull) /
        (uint64_t)g_rate);
}
