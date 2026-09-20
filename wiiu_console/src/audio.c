#include "audio.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <opus/opus.h>
#include <whb/log.h>

/*
 * This is only the maximum queued latency.
 *
 * Normal playback should stay far below this. If video stalls the main
 * loop for too long, dropping one new packet is preferable to letting
 * audio become increasingly late.
 */
#define AUDIO_MAX_QUEUE_MS 120

static OpusDecoder *g_decoder;
static SDL_AudioDeviceID g_device;

static int g_rate = 48000;
static int g_channels = 2;

static unsigned long g_decoded;
static unsigned long g_failed;
static unsigned long g_dropped;

int audio_init(int rate, int channels,
               char *why, size_t why_size)
{
    if (why && why_size) {
        why[0] = '\0';
    }

    if (g_device || g_decoder) {
        audio_exit();
    }

    int opus_error = OPUS_OK;

    g_decoder =
        opus_decoder_create(rate,
                            channels,
                            &opus_error);

    if (!g_decoder || opus_error != OPUS_OK) {
        if (why && why_size) {
            snprintf(why, why_size,
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

    /*
     * PowerPC is big endian, so AUDIO_S16SYS is AUDIO_S16MSB here.
     * That is also what SDL's Wii U AX backend uses.
     */
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;

    /*
     * Roughly 10 ms at 48 kHz.
     *
     * The Wii U backend may round this up to AX's own frame size.
     */
    want.samples = 512;

    /*
     * Queued playback rather than an SDL callback:
     * C2S packets arrive on the application's network loop.
     */
    want.callback = NULL;

    g_device =
        SDL_OpenAudioDevice(NULL,
                            0,
                            &want,
                            &have,
                            SDL_AUDIO_ALLOW_SAMPLES_CHANGE);

    if (!g_device) {
        if (why && why_size) {
            snprintf(why, why_size,
                     "SDL audio: %s",
                     SDL_GetError());
        }

        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
        return -1;
    }

    if (have.freq != rate ||
        have.channels != channels ||
        have.format != AUDIO_S16SYS) {

        if (why && why_size) {
            snprintf(why, why_size,
                     "unexpected audio %dHz/%uch fmt=%04x",
                     have.freq,
                     have.channels,
                     have.format);
        }

        SDL_CloseAudioDevice(g_device);
        g_device = 0;

        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;

        return -1;
    }

    g_rate = rate;
    g_channels = channels;

    g_decoded = 0;
    g_failed = 0;
    g_dropped = 0;

    WHBLogPrintf(
        "audio: SDL driver=%s %dHz stereo samples=%u",
        SDL_GetCurrentAudioDriver()
            ? SDL_GetCurrentAudioDriver()
            : "?",
        have.freq,
        have.samples);

    /*
     * SDL's Wii U audio backend opens AX/sndcore2 here and routes
     * stereo to both TV and DRC.
     */
    SDL_PauseAudioDevice(g_device, 0);

    return 0;
}

void audio_exit(void)
{
    if (g_device) {
        SDL_ClearQueuedAudio(g_device);
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }

    if (g_decoder) {
        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
    }
}

void audio_decode(const uint8_t *data, uint32_t size)
{
    if (!g_decoder ||
        !g_device ||
        !data ||
        size == 0) {
        return;
    }

    /*
     * Opus allows up to 120 ms in one packet.
     * Stereo worst case:
     *
     *   5760 frames * 2 channels
     */
    static int16_t pcm[5760 * 2];

    const int capacity =
        (int)(sizeof(pcm) /
              sizeof(pcm[0]) /
              g_channels);

    int frames =
        opus_decode(g_decoder,
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

    const Uint32 queued =
        SDL_GetQueuedAudioSize(g_device);

    const Uint32 max_queue =
        (Uint32)(
            g_rate *
            g_channels *
            sizeof(int16_t) *
            AUDIO_MAX_QUEUE_MS /
            1000);

    if (queued > max_queue) {
        /*
         * Do NOT clear the queue here:
         * cutting PCM halfway through a waveform makes an audible click.
         *
         * Let the device consume what it already has and skip this one
         * 5 ms Opus packet instead.
         */
        g_dropped++;
        return;
    }

    const Uint32 bytes =
        (Uint32)(
            frames *
            g_channels *
            sizeof(int16_t));

    if (SDL_QueueAudio(g_device,
                       pcm,
                       bytes) != 0) {
        g_failed++;
    }
}

void audio_stats(unsigned long *decoded,
                 unsigned long *failed,
                 unsigned long *dropped)
{
    if (decoded) *decoded = g_decoded;
    if (failed)  *failed = g_failed;
    if (dropped) *dropped = g_dropped;
}


unsigned audio_queue_ms(void)
{
    if (!g_device ||
        g_rate <= 0 ||
        g_channels <= 0) {
        return 0;
    }

    const Uint32 bytes =
        SDL_GetQueuedAudioSize(g_device);

    const uint64_t bytes_per_second =
        (uint64_t)g_rate *
        (uint64_t)g_channels *
        sizeof(int16_t);

    return bytes_per_second
        ? (unsigned)(
            ((uint64_t)bytes * 1000ull) /
            bytes_per_second)
        : 0;
}
