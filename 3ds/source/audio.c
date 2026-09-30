#include "audio.h"

#include "network.h"

#include <3ds.h>
#include <opus/opus.h>

#include <string.h>

#define AUDIO_CHANNEL 0
#define AUDIO_BUFFERS 12
#define AUDIO_MAX_FRAMES 960

static OpusDecoder *g_decoder;
static ndspWaveBuf g_wave[AUDIO_BUFFERS];
static AudioStats g_stats;
static bool g_ndsp;

bool audio_init(void)
{
    int error = OPUS_OK;
    g_decoder = opus_decoder_create(48000, 2, &error);
    if (!g_decoder || error != OPUS_OK) return false;
    if (R_FAILED(ndspInit())) {
        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
        return false;
    }
    g_ndsp = true;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(AUDIO_CHANNEL);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CHANNEL, 48000.0f);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        memset(&g_wave[i], 0, sizeof(g_wave[i]));
        g_wave[i].data_pcm16 = linearAlloc(AUDIO_MAX_FRAMES * 2 * sizeof(int16_t));
        if (!g_wave[i].data_pcm16) {
            audio_exit();
            return false;
        }
    }
    g_stats.available = true;
    return true;
}

void audio_clear(void)
{
    if (!g_ndsp) return;
    ndspChnWaveBufClear(AUDIO_CHANNEL);
    for (int i = 0; i < AUDIO_BUFFERS; i++) g_wave[i].status = NDSP_WBUF_FREE;
    if (g_decoder) opus_decoder_ctl(g_decoder, OPUS_RESET_STATE);
}

void audio_exit(void)
{
    if (g_ndsp) {
        ndspChnWaveBufClear(AUDIO_CHANNEL);
        for (int i = 0; i < AUDIO_BUFFERS; i++) {
            linearFree(g_wave[i].data_pcm16);
            g_wave[i].data_pcm16 = NULL;
        }
        ndspExit();
        g_ndsp = false;
    }
    if (g_decoder) {
        opus_decoder_destroy(g_decoder);
        g_decoder = NULL;
    }
    g_stats.available = false;
}

void audio_service(bool enabled)
{
    if (!g_ndsp || !g_decoder) return;
    if (!enabled) {
        audio_clear();
        return;
    }

    uint8_t packet[4096];
    uint32_t size = 0;
    for (int packet_index = 0; packet_index < AUDIO_BUFFERS &&
         network_take_audio(packet, sizeof(packet), &size); packet_index++) {
        int free_index = -1;
        for (int i = 0; i < AUDIO_BUFFERS; i++) {
            if (g_wave[i].status == NDSP_WBUF_FREE ||
                g_wave[i].status == NDSP_WBUF_DONE) {
                free_index = i;
                break;
            }
        }
        if (free_index < 0) {
            g_stats.dropped_packets++;
            continue;
        }
        ndspWaveBuf *wave = &g_wave[free_index];
        int frames = opus_decode(g_decoder, packet, (opus_int32)size,
                                 wave->data_pcm16, AUDIO_MAX_FRAMES, 0);
        if (frames <= 0) {
            g_stats.decode_errors++;
            continue;
        }
        wave->nsamples = (uint32_t)frames;
        wave->looping = false;
        DSP_FlushDataCache(wave->data_pcm16,
                           (uint32_t)frames * 2u * sizeof(int16_t));
        ndspChnWaveBufAdd(AUDIO_CHANNEL, wave);
        g_stats.decoded_packets++;
    }

    uint32_t queued = 0;
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        if (g_wave[i].status == NDSP_WBUF_QUEUED ||
            g_wave[i].status == NDSP_WBUF_PLAYING) queued++;
    }
    g_stats.queued_buffers = queued;
}

void audio_get_stats(AudioStats *stats) { *stats = g_stats; }
