#include "audio.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <opus/opus.h>

/* The host encodes 5 ms Opus frames. 120 ms was too tight: this client's
 * frame loop is what feeds the queue, so every hitch in decoding video
 * starved it, and the gaps were the "disgusting" part of the sound.
 * 250 ms rides those out and is still well under the point where the
 * delay itself becomes the complaint. */
#define RING_MS 250

static OpusDecoder *g_decoder = NULL;
static SDL_AudioDeviceID g_device = 0;
static int g_rate = 48000;
static int g_channels = 2;
static int g_muted = 0;
/* The volume slider's position, 0 to 100, where 100 is eight times the
 * stream's own level.
 *
 * It used to run 0 to 200 and mean literally that, so the top of the
 * range was twice the source and barely louder than it -- a stream that
 * arrives quiet stayed quiet. Same scheme as the browser page now: the
 * scale reads 0-100 because nobody thinks of volume as a number above a
 * hundred, and the loudest it goes is eight times the source.
 *
 * That puts the stream's own level at 12.5, which is not a whole number,
 * so the default is 13 -- four percent above the source, which nobody
 * can hear. The same eight is used by the page and by the host's own
 * speakers, so a given position sounds the same wherever it plays. */
#define VOLUME_MAX_GAIN 8
static int g_volume_percent = 13;

static unsigned long g_decoded = 0, g_failed = 0, g_dropped = 0;

int audio_init(int rate, int channels) {
    int err = 0;
    g_decoder = opus_decoder_create(rate, channels, &err);
    if (err != OPUS_OK || !g_decoder) {
        printf("opus_decoder_create: %s\n", opus_strerror(err));
        return -1;
    }
    g_rate = rate;
    g_channels = channels;

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    /* Small buffer, because this is the audio for something being played
     * live: a large one would only add delay between a button press and
     * the sound it caused. */
    /* Bigger than feels necessary, deliberately: the queue is filled
     * from the frame loop, not on the device's clock, so the device has
     * to be able to run for a while without being fed. 1024 frames is
     * ~21 ms of headroom against a loop that stutters. */
    want.samples = 1024;
    want.callback = NULL; /* queued, not pulled: frames arrive from the
                             network thread, not on the device's clock */

    g_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_device) {
        printf("SDL_OpenAudioDevice: %s\n", SDL_GetError());
        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
        return -1;
    }
    SDL_PauseAudioDevice(g_device, 0);
    return 0;
}

void audio_exit(void) {
    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    if (g_decoder) {
        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
    }
}

void audio_set_muted(int muted) { g_muted = muted ? 1 : 0; }
int  audio_is_muted(void) { return g_muted; }
void audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume_percent = percent;
}
int audio_volume(void) { return g_volume_percent; }

void audio_decode(const uint8_t *data, uint32_t size) {
    if (!g_decoder || !g_device || !data || !size) {
        return;
    }
    /* Opus frames here are 5 ms; the buffer is sized for the largest a
     * packet could legally carry so a bigger one still fits. */
    static int16_t pcm[5760 * 2];
    int frames = opus_decode(g_decoder, data, (opus_int32)size, pcm,
                             (int)(sizeof(pcm) / sizeof(pcm[0]) / g_channels), 0);
    if (frames <= 0) {
        g_failed++;
        return;
    }
    g_decoded++;

    if (g_muted) {
        /* Still decoded, never queued: Opus is a predictive codec, so
         * skipping packets while muted would leave the decoder unable to
         * reconstruct the first frames after unmuting. */
        return;
    }

    int samples = frames * g_channels;
    /* Compared as a gain rather than as a slider position: with a top of
     * eight there is no whole position that means exactly unity, so this
     * is now almost always true and the samples are almost always
     * scaled. That is an integer multiply and divide per sample, which
     * at 48 kHz stereo is nothing next to decoding the stream itself. */
    if (g_volume_percent * VOLUME_MAX_GAIN != 100) {
        for (int i = 0; i < samples; i++) {
            /* Clipped rather than wrapped: above the source's own level
             * a loud passage has nowhere to go, and wrapping would turn
             * it into noise instead of just flattening it. */
            int v = pcm[i] * g_volume_percent * VOLUME_MAX_GAIN / 100;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            pcm[i] = (int16_t)v;
        }
    }

    /* If the queue has run away the sound is already late, and adding to
     * it only makes it later. But CLEARING it -- which is what this did
     * -- cuts mid-waveform and is heard as a click every time, which on
     * a stream that is constantly a little behind means a click every
     * second or two. Dropping this one packet instead lets the backlog
     * drain by 5 ms at a time, which is short enough to pass unnoticed. */
    Uint32 queued = SDL_GetQueuedAudioSize(g_device);
    Uint32 limit = (Uint32)(g_rate * g_channels * 2 * RING_MS / 1000);
    if (queued > limit) {
        g_dropped++;
        return;
    }
    SDL_QueueAudio(g_device, pcm, (Uint32)(samples * (int)sizeof(int16_t)));
}

void audio_stats(unsigned long *decoded, unsigned long *failed) {
    if (decoded) *decoded = g_decoded;
    if (failed) *failed = g_failed;
}
