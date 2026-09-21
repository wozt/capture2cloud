#include "audio.h"
#include "c2s_protocol.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <whb/log.h>

/*
 * Wii U console audio path:
 *
 *   host capture PCM S16LE
 *       -> TCP
 *       -> this ring
 *       -> SDL / AX
 *
 * No Opus encoder or decoder exists in this path.
 */

#define AUDIO_RING_FRAMES 8192

#define AUDIO_START_MS  25
#define AUDIO_TARGET_MS 40
#define AUDIO_LOW_MS    25
#define AUDIO_HIGH_MS   55

static SDL_AudioDeviceID g_device;

static int g_rate = 48000;
static int g_channels = 2;

static int16_t g_ring[AUDIO_RING_FRAMES * 2];

static uint32_t g_head;
static uint32_t g_tail;
static uint32_t g_count;

static int g_playing;

/*
 * -1 = consume one source frame less per callback
 *  0 = exact 1:1
 * +1 = consume one source frame more per callback
 *
 * At a normal 512-frame callback this is only ~0.195 % correction.
 * Hysteresis means it does not jump back and forth every callback.
 */
static int g_drift_mode;

static unsigned long g_packets;
static unsigned long g_failed;
static unsigned long g_dropped;


static int16_t ring_sample(uint32_t offset,
                           int channel)
{
    const uint32_t frame =
        (g_tail + offset) %
        AUDIO_RING_FRAMES;

    return g_ring[
        (size_t)frame *
        (size_t)g_channels +
        (size_t)channel];
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

    if (g_rate <= 0 ||
        g_channels != 2) {
        return;
    }

    const uint32_t out_frames =
        (uint32_t)len /
        ((uint32_t)sizeof(int16_t) * 2u);

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

    const uint32_t low_frames =
        (uint32_t)(
            (uint64_t)g_rate *
            AUDIO_LOW_MS /
            1000u);

    const uint32_t high_frames =
        (uint32_t)(
            (uint64_t)g_rate *
            AUDIO_HIGH_MS /
            1000u);

    if (!g_playing) {
        if (g_count < start_frames) {
            return;
        }

        g_playing = 1;
        g_drift_mode = 0;
    }

    /*
     * Wide hysteresis:
     *
     * enter correction only outside 25..55 ms,
     * leave it again around the 40 ms target.
     */
    if (g_drift_mode == 0) {
        if (g_count > high_frames) {
            g_drift_mode = 1;
        } else if (g_count < low_frames) {
            g_drift_mode = -1;
        }

    } else if (g_drift_mode > 0) {
        if (g_count <= target_frames) {
            g_drift_mode = 0;
        }

    } else {
        if (g_count >= target_frames) {
            g_drift_mode = 0;
        }
    }

    int consume =
        (int)out_frames +
        g_drift_mode;

    if (consume < 2) {
        consume = 2;
    }

    if (g_count <
        (uint32_t)consume) {

        /*
         * A real underrun: output silence until a short clean buffer has
         * formed again.
         */
        g_playing = 0;
        g_drift_mode = 0;
        return;
    }

    int16_t *out =
        (int16_t *)stream;

    if (out_frames == 1) {
        out[0] = ring_sample(0, 0);
        out[1] = ring_sample(0, 1);

    } else {
        /*
         * Map consume input frames onto exactly out_frames output
         * frames.
         *
         * Normal operation:
         *     512 -> 512
         *
         * Clock correction:
         *     513 -> 512
         * or  511 -> 512
         *
         * The last generated sample lands on the last consumed source
         * sample, so the next callback continues with the immediately
         * following source frame rather than restarting a fractional
         * phase as the old corrector did.
         */
        const uint64_t scale =
            ((uint64_t)(consume - 1) << 16) /
            (uint64_t)(out_frames - 1);

        for (uint32_t i = 0;
             i < out_frames;
             ++i) {

            const uint64_t pos =
                (uint64_t)i *
                scale;

            const uint32_t a =
                (uint32_t)(
                    pos >> 16);

            const uint32_t frac =
                (uint32_t)(
                    pos & 0xffffu);

            uint32_t b =
                a + 1;

            if (b >=
                (uint32_t)consume) {
                b =
                    (uint32_t)consume - 1;
            }

            for (int ch = 0;
                 ch < 2;
                 ++ch) {

                const int32_t sa =
                    ring_sample(
                        a,
                        ch);

                const int32_t sb =
                    ring_sample(
                        b,
                        ch);

                const int32_t mixed =
                    sa +
                    (int32_t)(
                        ((int64_t)(sb - sa) *
                         (int64_t)frac) >>
                        16);

                out[
                    (size_t)i * 2u +
                    (size_t)ch] =
                        (int16_t)mixed;
            }
        }
    }

    g_tail =
        (g_tail +
         (uint32_t)consume) %
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

    if (g_device) {
        audio_exit();
    }

    if (rate <= 0 ||
        channels != 2) {

        if (why && why_size) {
            snprintf(
                why,
                why_size,
                "PCM expects 48k stereo");
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
    want.samples = 512;
    want.callback = audio_callback;
    want.userdata = NULL;

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

        SDL_CloseAudioDevice(
            g_device);

        g_device = 0;

        return -1;
    }

    g_rate = rate;
    g_channels = 2;

    g_head = 0;
    g_tail = 0;
    g_count = 0;

    g_playing = 0;
    g_drift_mode = 0;

    g_packets = 0;
    g_failed = 0;
    g_dropped = 0;

    WHBLogPrintf(
        "audio: raw PCM S16LE -> AX, %dHz stereo samples=%u",
        have.freq,
        have.samples);

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

    g_playing = 0;
    g_drift_mode = 0;
}


void audio_push_pcm_s16le(const uint8_t *data,
                          uint32_t size)
{
    if (!g_device ||
        !data ||
        !size) {
        return;
    }

    /*
     * Stereo, signed 16-bit:
     *
     * 4 bytes per PCM frame.
     */
    if ((size & 3u) != 0) {
        g_failed++;
        return;
    }

    uint32_t frames =
        size / 4u;

    if (!frames) {
        return;
    }

    g_packets++;

    /*
     * Packets are normally 240 frames / 5 ms.
     * Protect against a malformed giant packet anyway.
     */
    if (frames >
        AUDIO_RING_FRAMES) {

        const uint32_t skip =
            frames -
            AUDIO_RING_FRAMES;

        data +=
            (size_t)skip * 4u;

        frames =
            AUDIO_RING_FRAMES;

        g_dropped++;
    }

    SDL_LockAudioDevice(
        g_device);

    const uint32_t free_frames =
        AUDIO_RING_FRAMES -
        g_count;

    if (frames > free_frames) {
        /*
         * Emergency only. Keep the live edge rather than allowing
         * seconds of stale sound to accumulate.
         */
        const uint32_t remove =
            frames -
            free_frames;

        g_tail =
            (g_tail + remove) %
            AUDIO_RING_FRAMES;

        g_count -= remove;

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
                 (size_t)ch) * 2u;

            /*
             * Wire is explicitly little-endian.
             * PPC/Wii U is big-endian, so build the numeric sample
             * instead of memcpy'ing the bytes into AUDIO_S16SYS.
             */
            const uint16_t u =
                (uint16_t)data[at] |
                ((uint16_t)data[at + 1] << 8);

            g_ring[
                (size_t)dst * 2u +
                ch] =
                    (int16_t)u;
        }
    }

    g_head =
        (g_head + frames) %
        AUDIO_RING_FRAMES;

    g_count +=
        frames;

    SDL_UnlockAudioDevice(
        g_device);
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
