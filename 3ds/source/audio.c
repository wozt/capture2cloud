#include "audio.h"

#include "network.h"

#include <3ds.h>
#include <opus/opus.h>

#include <string.h>

#define AUDIO_CHANNEL 0
#define AUDIO_BUFFERS 12
#define AUDIO_MAX_FRAMES 1920
#define AUDIO_BATCH_PACKETS 4
#define AUDIO_START_PACKETS 8
#define AUDIO_ADPCM_START_PACKETS 3

static OpusDecoder *g_decoder;
static ndspWaveBuf g_wave[AUDIO_BUFFERS];
static AudioStats g_stats;
static bool g_ndsp;
static Thread g_thread;
static LightLock g_lock;
static volatile bool g_running;
static volatile bool g_enabled;
static volatile bool g_clear_requested;
static bool g_started;

static int16_t clamp_s16(int value)
{
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

static int16_t decode_adpcm_nibble(uint8_t code, int *predictor, int *index)
{
    static const int step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
        408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
        1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
        3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
        8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767
    };
    static const int index_adjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    const int step = step_table[*index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    *predictor += (code & 8) ? -delta : delta;
    *predictor = clamp_s16(*predictor);
    *index += index_adjust[code & 7];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return (int16_t)*predictor;
}

static int decode_adpcm_packet(const uint8_t *packet, uint32_t size,
                               int16_t *output, int output_frames)
{
    /* A zero-sized queued packet is packet-loss concealment. */
    if (!size) {
        if (output_frames < C2S_ADPCM_FRAMES * 2) return -1;
        memset(output, 0, C2S_ADPCM_FRAMES * 2 * 2 * sizeof(int16_t));
        return C2S_ADPCM_FRAMES * 2;
    }
    if (size != sizeof(C2sAdpcmUdpHeader) + C2S_ADPCM_MAX_BYTES ||
        output_frames < C2S_ADPCM_FRAMES * 2) return -1;
    C2sAdpcmUdpHeader header;
    memcpy(&header, packet, sizeof(header));
    if (c2s_le32(header.magic) != C2S_ADPCM_UDP_MAGIC ||
        c2s_le16(header.frames) != C2S_ADPCM_FRAMES ||
        c2s_le16(header.bytes) != C2S_ADPCM_MAX_BYTES ||
        header.index_left > 88 || header.index_right > 88) return -1;

    int left_predictor = (int16_t)c2s_le16((uint16_t)header.predictor_left);
    int right_predictor = (int16_t)c2s_le16((uint16_t)header.predictor_right);
    int left_index = header.index_left;
    int right_index = header.index_right;
    const uint8_t *encoded = packet + sizeof(header);
    for (int frame = 0; frame < C2S_ADPCM_FRAMES; frame++) {
        int16_t left;
        int16_t right;
        if (frame == 0) {
            left = (int16_t)left_predictor;
            right = (int16_t)right_predictor;
        } else {
            const uint8_t packed = encoded[frame - 1];
            left = decode_adpcm_nibble(packed & 0x0f,
                                       &left_predictor, &left_index);
            right = decode_adpcm_nibble(packed >> 4,
                                        &right_predictor, &right_index);
        }
        /* NDSP remains at 48 kHz.  Duplicating the 24 kHz samples is
         * essentially free and keeps every existing audio buffer/layout. */
        const int out = frame * 4;
        output[out] = left;
        output[out + 1] = right;
        output[out + 2] = left;
        output[out + 3] = right;
    }
    return C2S_ADPCM_FRAMES * 2;
}

static void audio_clear_now(void)
{
    ndspChnWaveBufClear(AUDIO_CHANNEL);
    for (int i = 0; i < AUDIO_BUFFERS; i++) g_wave[i].status = NDSP_WBUF_FREE;
    if (g_decoder) opus_decoder_ctl(g_decoder, OPUS_RESET_STATE);
    network_clear_audio();
    g_started = false;
    LightLock_Lock(&g_lock);
    g_stats.queued_buffers = 0;
    LightLock_Unlock(&g_lock);
}

static uint32_t audio_service_once(void)
{
    uint8_t packet[4096];
    uint32_t queued = 0;
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        if (g_wave[i].status == NDSP_WBUF_QUEUED ||
            g_wave[i].status == NDSP_WBUF_PLAYING) queued++;
    }

    for (int batch = 0; batch < AUDIO_BUFFERS; batch++) {
        const uint32_t depth = network_audio_depth();
        uint8_t queued_codec = C2S_CODEC_OPUS;
        if (!depth || !network_peek_audio_codec(&queued_codec)) break;
        const int packets_per_wave =
            queued_codec == C2S_CODEC_OLD3DS_ADPCM ? 1 : AUDIO_BATCH_PACKETS;
        if (depth < (uint32_t)packets_per_wave && queued >= 2) break;

        int free_index = -1;
        for (int i = 0; i < AUDIO_BUFFERS; i++) {
            if (g_wave[i].status == NDSP_WBUF_FREE ||
                g_wave[i].status == NDSP_WBUF_DONE) {
                free_index = i;
                break;
            }
        }
        if (free_index < 0) break;

        ndspWaveBuf *wave = &g_wave[free_index];
        uint32_t total_frames = 0;
        for (int packet_index = 0;
             packet_index < packets_per_wave &&
             total_frames < AUDIO_MAX_FRAMES;
             packet_index++) {
            uint32_t size = 0;
            uint8_t codec = C2S_CODEC_OPUS;
            if (!network_take_audio(packet, sizeof(packet), &size, &codec)) break;

            int frames;
            if (codec == C2S_CODEC_OLD3DS_ADPCM) {
                frames = decode_adpcm_packet(
                    packet, size, wave->data_pcm16 + total_frames * 2u,
                    AUDIO_MAX_FRAMES - (int)total_frames);
            } else if (codec == C2S_CODEC_PCM_S16LE) {
                if (!size || (size & 3u) != 0 ||
                    size > (AUDIO_MAX_FRAMES - total_frames) * 4u) {
                    frames = -1;
                } else {
                    frames = (int)(size / 4u);
                    memcpy(wave->data_pcm16 + total_frames * 2u,
                           packet, size);
                }
            } else {
                const uint8_t *encoded = size ? packet : NULL;
                frames = opus_decode(
                    g_decoder, encoded, (opus_int32)size,
                    wave->data_pcm16 + total_frames * 2u,
                    AUDIO_MAX_FRAMES - (int)total_frames, 0);
            }
            if (frames <= 0) {
                LightLock_Lock(&g_lock);
                g_stats.decode_errors++;
                LightLock_Unlock(&g_lock);
                continue;
            }
            total_frames += (uint32_t)frames;
            LightLock_Lock(&g_lock);
            g_stats.decoded_packets++;
            LightLock_Unlock(&g_lock);
        }
        if (!total_frames) continue;

        /* NDSP works much more reliably with ~20 ms buffers than with
         * one 5 ms Opus/PCM packet per wave buffer.  The packets remain
         * low-latency on the wire; only the DSP submission is batched. */
        wave->nsamples = total_frames;
        wave->looping = false;
        DSP_FlushDataCache(wave->data_pcm16,
                           total_frames * 2u * sizeof(int16_t));
        ndspChnWaveBufAdd(AUDIO_CHANNEL, wave);
        queued++;
    }
    LightLock_Lock(&g_lock);
    g_stats.queued_buffers = queued;
    LightLock_Unlock(&g_lock);
    return queued;
}

static void audio_thread(void *unused)
{
    (void)unused;
    while (g_running) {
        if (g_clear_requested) {
            g_clear_requested = false;
            audio_clear_now();
        }
        if (g_enabled) {
            if (!g_started) {
                uint8_t codec = C2S_CODEC_OPUS;
                const uint32_t threshold =
                    network_peek_audio_codec(&codec) &&
                    codec == C2S_CODEC_OLD3DS_ADPCM
                        ? AUDIO_ADPCM_START_PACKETS
                        : AUDIO_START_PACKETS;
                if (network_audio_depth() >= threshold) g_started = true;
            }
            if (g_started && audio_service_once() == 0) {
                g_started = false;
                LightLock_Lock(&g_lock);
                g_stats.underruns++;
                LightLock_Unlock(&g_lock);
            }
        }
        svcSleepThread(1000000LL);
    }
}

bool audio_init(void)
{
    LightLock_Init(&g_lock);
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
    float mix[12] = {1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    ndspChnSetMix(AUDIO_CHANNEL, mix);
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
    g_running = true;
    /* Audio deadlines are short and independent of video decode.  Run
     * them on the system core time granted by APT, next to (and at a
     * slightly higher priority than) networking. */
    /* libopus uses substantially more stack than the small network
     * workers.  16 KiB overflowed into the thread allocation on real
     * Old 3DS hardware; the damage only became visible when threadFree
     * walked the heap during shutdown. */
    g_thread = threadCreate(audio_thread, NULL, 64 * 1024, 0x2f, 1, false);
    if (!g_thread) {
        g_thread = threadCreate(audio_thread, NULL, 64 * 1024,
                                0x2f, -2, false);
    }
    if (!g_thread) {
        g_running = false;
        audio_exit();
        return false;
    }
    return true;
}

void audio_clear(void)
{
    if (!g_ndsp) return;
    g_clear_requested = true;
}

void audio_exit(void)
{
    g_enabled = false;
    g_running = false;
    if (g_thread) {
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
    }
    if (g_ndsp) {
        audio_clear_now();
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
    if (g_enabled && !enabled) g_clear_requested = true;
    g_enabled = enabled;
}

void audio_get_stats(AudioStats *stats)
{
    LightLock_Lock(&g_lock);
    *stats = g_stats;
    LightLock_Unlock(&g_lock);
}
