#define _GNU_SOURCE

#include "audio_capture.h"

#include "app_config.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "Capture2Cloud"

/* Noise gate: below this peak level a chunk is treated as silence and
 * muted outright, killing the constant hiss these capture sticks emit
 * between sounds. Released only after several consecutive loud chunks,
 * so a quiet passage inside real audio isn't chopped up. */
#define AUDIO_GATE_THRESHOLD 0
#define AUDIO_GATE_RELEASE_CHUNKS 20

/* One-pole high-pass, removing the DC offset the card adds -- which
 * would otherwise eat headroom and skew the gate's level detection. */
#define AUDIO_HIGHPASS_ALPHA 0.995f

/* Notch filters on the mains hum and its harmonics, the dominant
 * artefact on these devices. Narrow (high Q) so real audio content near
 * those frequencies is barely touched. */
/* How long to wait between attempts to reopen the capture source. Long
 * enough not to spin against a device that is genuinely gone, short
 * enough that a passing glitch costs a fraction of a second of sound. */
#define AUDIO_REOPEN_RETRY_MS 500

/* No audio for this long means the source has stopped, whatever it
 * claims about its state. Comfortably longer than any scheduling hiccup
 * and short enough that a stall costs a second of sound rather than the
 * rest of the session. */
#define AUDIO_STALL_TIMEOUT_MS 1200

/* How often the recording mainloop wakes its waiter even with nothing to
 * deliver, so the timeout above can be noticed at all. */
#define PA_RECORD_TICK_US (100 * 1000)

#define AUDIO_NOTCH_COUNT 9
#define AUDIO_NOTCH_Q 20.0f

/* Frames per packet handed to the WebRTC stream: 5 ms at 48 kHz, to
 * match the Opus encoder's frame-size. Pushing in bigger chunks than the
 * encoder consumes would just add its own wait on top of the encoder's,
 * so the two are kept aligned. */
#define WEB_STREAM_AUDIO_PACKET_FRAMES 240

/* Local playback runs on its own thread, fed through this ring.
 *
 * It used to share the capture thread: read a chunk, filter it, push it
 * to WebRTC, then pa_simple_write() it to the speakers. That last call
 * blocks until the sound card has room, which paced the whole loop --
 * measured cost: capture latency of 22 ms instead of the 1.8 ms the
 * same code reaches with playback disabled, i.e. ~20 ms of pure delay
 * added to the WebRTC path by a feature that has nothing to do with it.
 * (Enlarging the playback buffer made it worse, not better: the loop
 * then paced itself on playback consumption instead.)
 *
 * With the write moved off the capture thread, capture is paced only by
 * capture. When the speakers can't keep up the ring drops its OLDEST
 * chunk rather than blocking: local monitoring is the expendable
 * consumer here, and a brief local glitch beats delaying the stream. */
#define PLAYBACK_RING_CHUNKS 24

struct PlaybackRing {
    int16_t *data;      /* PLAYBACK_RING_CHUNKS * chunk_samples */
    size_t chunk_samples;
    int head, tail, count;
    int running;
    SDL_mutex *mutex;
    SDL_cond *cond;
    pa_simple *play;
    /* Kept so the stream can be built again without reaching back into
     * the capture's own state from another thread. */
    pa_sample_spec spec;
    pa_buffer_attr attr;
};

struct AudioCapture {
    SDL_Thread *thread;
    volatile sig_atomic_t *running;
    WebStream *web;
    GstWebrtcStream *gst;
    const char *source; /* points into source_buf, or NULL for the default */
    char source_buf[512];
    int local_playback;         /* LOCAL_PLAYBACK in the .env */
    /* The speakers here, changed from the settings window while
     * running. Muting stops what is queued to them; it never touches
     * what the browser and the console receive. Read on the capture
     * thread, written on GTK's -- an int either side of a change is one
     * chunk of five milliseconds, so no lock earns its keep. */
    /* Asked for from elsewhere, acted on by the capture thread. Two
     * different things: `wanted` is whether the speakers exist at all,
     * which is what "show capture" turns on in a session that started
     * headless; `muted` is a stream that is open and silent, which is
     * instant and keeps the device alive. */
    volatile int local_wanted;
    volatile int local_muted;
    volatile int local_volume;  /* 0..100, 25 = the source's own level */
    SDL_Thread *playback_thread;
    struct PlaybackRing ring;
};

/* Consumes the ring at the speakers' own pace. Blocking here is fine --
 * that is the entire point of moving it off the capture thread. */
/* Opens the speakers here and the thread that feeds them.
 *
 * Split out of the capture's setup so it can happen later as well.
 * Headless opens nothing, and "show capture" then brings the picture
 * back -- and used to bring the picture back and nothing else, because
 * the only chance to open the speakers had gone by half an hour
 * earlier. Failing is not fatal: the capture, and therefore the stream,
 * carries on without them. */
static int local_playback_open(AudioCapture *ac);
static void local_playback_close(AudioCapture *ac);

static int playback_thread_main(void *arg) {
    struct PlaybackRing *r = arg;
    int16_t *chunk = malloc(r->chunk_samples * sizeof(int16_t));
    if (!chunk) {
        return 1;
    }
    for (;;) {
        SDL_LockMutex(r->mutex);
        while (r->running && r->count == 0) {
            SDL_CondWait(r->cond, r->mutex);
        }
        if (!r->running && r->count == 0) {
            SDL_UnlockMutex(r->mutex);
            break;
        }
        memcpy(chunk, r->data + (size_t)r->tail * r->chunk_samples, r->chunk_samples * sizeof(int16_t));
        r->tail = (r->tail + 1) % PLAYBACK_RING_CHUNKS;
        r->count--;
        SDL_UnlockMutex(r->mutex);

        int err = 0;
        if (pa_simple_write(r->play, chunk, r->chunk_samples * sizeof(int16_t), &err) < 0) {
            /* Ending the thread here left the speakers dead for the rest
             * of the session with one line in a log nobody was reading.
             * The stream is rebuilt instead, the same way the capture
             * side rebuilds its own. */
            fprintf(stderr, "audio_capture: local playback failed (%s), reopening\n",
                    pa_strerror(err));
            pa_simple_free(r->play);
            r->play = NULL;
            while (r->running && !r->play) {
                SDL_Delay(AUDIO_REOPEN_RETRY_MS);
                r->play = pa_simple_new(NULL, APP_NAME, PA_STREAM_PLAYBACK, NULL,
                                        "Capture audio", &r->spec, NULL, &r->attr, &err);
            }
            if (!r->play) {
                break; /* asked to stop while retrying */
            }
            fprintf(stderr, "audio_capture: local playback is back\n");
        }
    }
    free(chunk);
    return 0;
}

/* Never blocks: if the consumer is behind, the oldest chunk goes. */
static void playback_ring_push(struct PlaybackRing *r, const int16_t *samples) {
    SDL_LockMutex(r->mutex);
    if (r->count == PLAYBACK_RING_CHUNKS) {
        r->tail = (r->tail + 1) % PLAYBACK_RING_CHUNKS;
        r->count--;
    }
    memcpy(r->data + (size_t)r->head * r->chunk_samples, samples, r->chunk_samples * sizeof(int16_t));
    r->head = (r->head + 1) % PLAYBACK_RING_CHUNKS;
    r->count++;
    SDL_CondSignal(r->cond);
    SDL_UnlockMutex(r->mutex);
}

struct biquad {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1[2];
    float z2[2];
};

static void init_notch(struct biquad *bq, float freq, float sample_rate, float q) {
    float w0 = 2.0f * (float)M_PI * freq / sample_rate;
    float cos_w0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;

    bq->b0 = 1.0f / a0;
    bq->b1 = (-2.0f * cos_w0) / a0;
    bq->b2 = 1.0f / a0;
    bq->a1 = (-2.0f * cos_w0) / a0;
    bq->a2 = (1.0f - alpha) / a0;
    bq->z1[0] = 0.0f;
    bq->z1[1] = 0.0f;
    bq->z2[0] = 0.0f;
    bq->z2[1] = 0.0f;
}

static float process_biquad(struct biquad *bq, float in, size_t ch) {
    float out = bq->b0 * in + bq->z1[ch];
    bq->z1[ch] = bq->b1 * in - bq->a1 * out + bq->z2[ch];
    bq->z2[ch] = bq->b2 * in - bq->a2 * out;
    return out;
}


/* --- recording, on the asynchronous API ------------------------------
 *
 * This used to be pa_simple_read(), which is three lines and has one
 * flaw that turned out to matter more than everything it saved: it can
 * block forever. Not fail -- block. Caught in the act with a debugger,
 * the audio thread sitting in pa_threaded_mainloop_wait() inside
 * pa_simple_read(), the source still marked RUNNING, our stream still
 * listed, and nothing arriving. Sound was gone on the browser and on the
 * console at once, and only restarting the whole program brought it
 * back. An earlier attempt at reconnecting on error could not help,
 * because there is no error: the call simply never returns.
 *
 * So the read has to be able to give up, and pa_simple has no timeout of
 * any kind. This is the same thing built on the API that does: data
 * arrives in a callback, a repeating timer wakes the waiter even when
 * nothing arrives, and a read that waits too long says so and is
 * reconnected. */
typedef struct {
    pa_threaded_mainloop *ml;
    pa_context *ctx;
    pa_stream *stream;
    pa_time_event *tick;

    uint8_t *buf;
    size_t cap, len;
    int broken; /* the server said this stream or context is finished */
} PaRecord;

/* Wakes anyone waiting, whether or not audio arrived, so a stall is
 * noticed instead of waited on. */
static void record_tick(pa_mainloop_api *api, pa_time_event *e, const struct timeval *tv,
                        void *userdata) {
    (void)tv;
    PaRecord *r = userdata;
    pa_threaded_mainloop_signal(r->ml, 0);
    struct timeval next;
    pa_gettimeofday(&next);
    pa_timeval_add(&next, PA_RECORD_TICK_US);
    api->time_restart(e, &next);
}

static void record_state_cb(pa_stream *s, void *userdata) {
    PaRecord *r = userdata;
    pa_stream_state_t st = pa_stream_get_state(s);
    if (st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED) {
        r->broken = 1;
    }
    pa_threaded_mainloop_signal(r->ml, 0);
}

static void context_state_cb(pa_context *c, void *userdata) {
    PaRecord *r = userdata;
    pa_context_state_t st = pa_context_get_state(c);
    if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
        r->broken = 1;
    }
    pa_threaded_mainloop_signal(r->ml, 0);
}

static void record_read_cb(pa_stream *s, size_t nbytes, void *userdata) {
    (void)nbytes;
    PaRecord *r = userdata;
    const void *data;
    size_t n;
    while (pa_stream_readable_size(s) > 0) {
        if (pa_stream_peek(s, &data, &n) < 0) {
            r->broken = 1;
            break;
        }
        if (n == 0) {
            break;
        }
        /* `data` is NULL for a hole in the recording -- a gap the server
         * could not fill. Its length still has to be dropped, and
         * silence is the honest thing to put in its place. */
        if (r->len + n <= r->cap) {
            if (data) {
                memcpy(r->buf + r->len, data, n);
            } else {
                memset(r->buf + r->len, 0, n);
            }
            r->len += n;
        }
        pa_stream_drop(s);
    }
    pa_threaded_mainloop_signal(r->ml, 0);
}

static void record_close(PaRecord *r) {
    if (r->ml) {
        pa_threaded_mainloop_stop(r->ml);
    }
    if (r->stream) {
        pa_stream_disconnect(r->stream);
        pa_stream_unref(r->stream);
        r->stream = NULL;
    }
    if (r->ctx) {
        pa_context_disconnect(r->ctx);
        pa_context_unref(r->ctx);
        r->ctx = NULL;
    }
    if (r->ml) {
        pa_threaded_mainloop_free(r->ml);
        r->ml = NULL;
    }
    free(r->buf);
    r->buf = NULL;
    r->cap = r->len = 0;
    r->broken = 0;
}

/* Returns 0 once the stream is recording. */
static int record_open(PaRecord *r, const char *source, const pa_sample_spec *ss,
                       const pa_buffer_attr *attr, size_t capacity) {
    memset(r, 0, sizeof(*r));
    r->buf = malloc(capacity);
    if (!r->buf) {
        return -1;
    }
    r->cap = capacity;

    r->ml = pa_threaded_mainloop_new();
    if (!r->ml) {
        record_close(r);
        return -1;
    }
    pa_mainloop_api *api = pa_threaded_mainloop_get_api(r->ml);
    r->ctx = pa_context_new(api, APP_NAME);
    if (!r->ctx) {
        record_close(r);
        return -1;
    }
    pa_context_set_state_callback(r->ctx, context_state_cb, r);
    if (pa_context_connect(r->ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0 ||
        pa_threaded_mainloop_start(r->ml) < 0) {
        record_close(r);
        return -1;
    }

    pa_threaded_mainloop_lock(r->ml);
    while (pa_context_get_state(r->ctx) != PA_CONTEXT_READY) {
        if (r->broken) {
            pa_threaded_mainloop_unlock(r->ml);
            record_close(r);
            return -1;
        }
        pa_threaded_mainloop_wait(r->ml);
    }

    r->stream = pa_stream_new(r->ctx, "HDMI capture", ss, NULL);
    if (!r->stream) {
        pa_threaded_mainloop_unlock(r->ml);
        record_close(r);
        return -1;
    }
    pa_stream_set_state_callback(r->stream, record_state_cb, r);
    pa_stream_set_read_callback(r->stream, record_read_cb, r);
    if (pa_stream_connect_record(r->stream, source, attr, PA_STREAM_ADJUST_LATENCY) < 0) {
        pa_threaded_mainloop_unlock(r->ml);
        record_close(r);
        return -1;
    }
    while (pa_stream_get_state(r->stream) != PA_STREAM_READY) {
        if (r->broken) {
            pa_threaded_mainloop_unlock(r->ml);
            record_close(r);
            return -1;
        }
        pa_threaded_mainloop_wait(r->ml);
    }

    struct timeval next;
    pa_gettimeofday(&next);
    pa_timeval_add(&next, PA_RECORD_TICK_US);
    r->tick = api->time_new(api, &next, record_tick, r);

    pa_threaded_mainloop_unlock(r->ml);
    return 0;
}

/* Fills `dst` with exactly `bytes`. Returns 0, or -1 if the stream broke
 * or delivered nothing for `timeout_ms` -- the case pa_simple could not
 * express. */
static int record_read(PaRecord *r, void *dst, size_t bytes, unsigned timeout_ms) {
    pa_threaded_mainloop_lock(r->ml);
    const Uint32 started = SDL_GetTicks();
    while (r->len < bytes) {
        if (r->broken) {
            pa_threaded_mainloop_unlock(r->ml);
            return -1;
        }
        if (SDL_GetTicks() - started > timeout_ms) {
            pa_threaded_mainloop_unlock(r->ml);
            return -1;
        }
        pa_threaded_mainloop_wait(r->ml);
    }
    memcpy(dst, r->buf, bytes);
    r->len -= bytes;
    memmove(r->buf, r->buf + bytes, r->len);
    pa_threaded_mainloop_unlock(r->ml);
    return 0;
}

static int audio_thread(void *arg) {
    AudioCapture *ac = arg;

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 48000;
    ss.channels = 2;

    const size_t frame_size = 2 * 2;
    const size_t chunk_frames = 64;
    const size_t chunk_bytes = chunk_frames * frame_size;

    pa_buffer_attr rec_attr;
    rec_attr.maxlength = (uint32_t)-1;
    rec_attr.tlength = (uint32_t)-1;
    rec_attr.prebuf = (uint32_t)-1;
    rec_attr.minreq = (uint32_t)-1;
    rec_attr.fragsize = (uint32_t)chunk_bytes;

    pa_buffer_attr play_attr;
    play_attr.maxlength = (uint32_t)(chunk_bytes * 4);
    play_attr.tlength = (uint32_t)(chunk_bytes * 2);
    play_attr.prebuf = 0;
    play_attr.minreq = (uint32_t)chunk_bytes;
    play_attr.fragsize = (uint32_t)-1;

    PaRecord rec;
    /* Room for a good fraction of a second, so a scheduling hiccup on
     * this side costs latency rather than a hole in the sound. */
    if (record_open(&rec, ac->source, &ss, &rec_attr, chunk_bytes * 64) != 0) {
        fprintf(stderr, "Pulse record: could not open %s\n", ac->source);
        return 1;
    }

    ac->ring.chunk_samples = chunk_frames * 2;
    ac->ring.spec = ss;
    ac->ring.attr = play_attr;
    /* Both: allowed by the .env, and asked for right now. Opening on
     * the first alone meant headless opened the speakers and the loop
     * closed them again a chunk later -- a device appearing in the mixer
     * for five milliseconds. */
    if (ac->local_playback && ac->local_wanted) {
        local_playback_open(ac);
    }

    int16_t samples[64 * 2];
    int16_t web_samples[WEB_STREAM_AUDIO_PACKET_FRAMES * 2];
    size_t web_sample_frames = 0;
    float hp_prev_in[2] = {0.0f, 0.0f};
    float hp_prev_out[2] = {0.0f, 0.0f};
    struct biquad notches[AUDIO_NOTCH_COUNT];
    const float notch_freqs[AUDIO_NOTCH_COUNT] = {
        60.0f, 120.0f, 180.0f, 240.0f, 300.0f,
        360.0f, 420.0f, 480.0f, 540.0f
    };
    int gate_hold = 0;

    for (size_t i = 0; i < AUDIO_NOTCH_COUNT; i++) {
        init_notch(&notches[i], notch_freqs[i], (float)ss.rate, AUDIO_NOTCH_Q);
    }

    while (*ac->running) {
        if (record_read(&rec, samples, sizeof(samples), AUDIO_STALL_TIMEOUT_MS) != 0) {
            /* Reopen rather than give up.
             *
             * The whole stream is rebuilt, not just retried: what stops
             * here is a source that has gone quiet while still claiming
             * to be running, and nothing short of a new stream gets it
             * back. The video capture already survives its device
             * dropping off the bus and returning; sound now does too. */
            fprintf(stderr, "audio_capture: no sound from %s for %d ms, reopening it\n",
                    ac->source, AUDIO_STALL_TIMEOUT_MS);
            record_close(&rec);

            int reopened = 0;
            while (*ac->running && !reopened) {
                SDL_Delay(AUDIO_REOPEN_RETRY_MS);
                reopened = (record_open(&rec, ac->source, &ss, &rec_attr, chunk_bytes * 64) == 0);
            }
            if (!reopened) {
                break; /* asked to stop while retrying */
            }
            fprintf(stderr, "audio_capture: sound is back\n");

            /* The filters carry per-sample history across what is now a
             * gap of unknown length; keeping it would ring for a moment
             * on the first samples after the break. */
            hp_prev_in[0] = hp_prev_in[1] = 0.0f;
            hp_prev_out[0] = hp_prev_out[1] = 0.0f;
            for (size_t i = 0; i < AUDIO_NOTCH_COUNT; i++) {
                init_notch(&notches[i], notch_freqs[i], (float)ss.rate, AUDIO_NOTCH_Q);
            }
            web_sample_frames = 0;
            continue;
        }

        int64_t energy = 0;
        int peak = 0;
        for (size_t i = 0; i < chunk_frames; i++) {
            for (size_t ch = 0; ch < 2; ch++) {
                size_t idx = i * 2 + ch;
                float in = (float)samples[idx];
                float out = AUDIO_HIGHPASS_ALPHA * (hp_prev_out[ch] + in - hp_prev_in[ch]);
                hp_prev_in[ch] = in;
                hp_prev_out[ch] = out;

                for (size_t notch = 0; notch < AUDIO_NOTCH_COUNT; notch++) {
                    out = process_biquad(&notches[notch], out, ch);
                }

                if (out > 32767.0f) {
                    out = 32767.0f;
                } else if (out < -32768.0f) {
                    out = -32768.0f;
                }

                samples[idx] = (int16_t)out;
                int abs_sample = samples[idx] < 0 ? -samples[idx] : samples[idx];
                if (abs_sample > peak) {
                    peak = abs_sample;
                }
                energy += (int64_t)abs_sample * abs_sample;
            }
        }

        int rms = (int)sqrt((double)energy / (double)(chunk_frames * 2));
        if (AUDIO_GATE_THRESHOLD > 0) {
            if (peak > AUDIO_GATE_THRESHOLD * 3 || rms > AUDIO_GATE_THRESHOLD) {
                gate_hold = AUDIO_GATE_RELEASE_CHUNKS;
            } else if (gate_hold > 0) {
                gate_hold--;
            } else {
                memset(samples, 0, sizeof(samples));
            }
        }

        if (ac->web && ac->gst && web_stream_is_running(ac->web)) {
            size_t copied = 0;
            while (copied < chunk_frames) {
                size_t room = WEB_STREAM_AUDIO_PACKET_FRAMES - web_sample_frames;
                size_t n = chunk_frames - copied;
                if (n > room) {
                    n = room;
                }
                memcpy(&web_samples[web_sample_frames * 2], &samples[copied * 2], n * 2 * sizeof(int16_t));
                web_sample_frames += n;
                copied += n;

                if (web_sample_frames == WEB_STREAM_AUDIO_PACKET_FRAMES) {
                    gst_webrtc_stream_push_audio(ac->gst, web_samples, WEB_STREAM_AUDIO_PACKET_FRAMES);
                    web_sample_frames = 0;
                }
            }
        } else {
            web_sample_frames = 0;
        }

        /* Opened and closed here rather than where it is asked for: the
         * capture thread owns the stream. */
        if (ac->local_playback && ac->local_wanted && !ac->ring.play) {
            local_playback_open(ac);
        } else if (ac->ring.play && !ac->local_wanted) {
            local_playback_close(ac);
            fprintf(stderr, "audio_capture: local speakers off\n");
        }

        if (ac->ring.play) {
            /* Muting pushes SILENCE rather than pushing nothing.
             *
             * Stopping the writes was the obvious way and left the
             * speakers dead after unmuting: a playback stream that is
             * starved for any length of time is one the server has
             * every right to tear down, and there is nothing to notice
             * that from -- the thread is asleep waiting for data that
             * is not coming. Feeding it zeros keeps it a live stream
             * that happens to be quiet, which is what mute means
             * anyway.
             *
             * The volume is applied to a copy: `samples` has already
             * gone to the browser and the console, and turning the
             * speakers here down must not turn them down there. */
            int16_t local[64 * 2];
            if (ac->local_muted) {
                memset(local, 0, chunk_frames * 2 * sizeof(int16_t));
                playback_ring_push(&ac->ring, local);
            } else if (ac->local_volume != 25) {
                const int gain = ac->local_volume * 4; /* percent of the source */
                for (size_t i = 0; i < chunk_frames * 2; i++) {
                    int v = samples[i] * gain / 100;
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    local[i] = (int16_t)v;
                }
                playback_ring_push(&ac->ring, local);
            } else {
                playback_ring_push(&ac->ring, samples);
            }
        }
    }

    local_playback_close(ac);
    if (ac->ring.mutex) SDL_DestroyMutex(ac->ring.mutex);
    if (ac->ring.cond) SDL_DestroyCond(ac->ring.cond);
    free(ac->ring.data);
    ac->ring.data = NULL;

    record_close(&rec);
    return 0;
}
AudioCapture *audio_capture_start(const char *source, volatile sig_atomic_t *running, WebStream *web,
                                  GstWebrtcStream *gst, int local_output) {
    AudioCapture *ac = calloc(1, sizeof(*ac));
    if (!ac) {
        return NULL;
    }
    ac->running = running;
    ac->web = web;
    ac->gst = gst;
    /* On by default: the native window is a legitimate way to use this
     * (screen sharing over Discord, for one). Turn it off when only the
     * browser matters -- it saves a thread and a sound-card stream. */
    /* `local_playback` is whether they are ALLOWED at all -- the .env's
     * say, decided once. `local_wanted` is whether they are on right
     * now, which headless starts with off and "show capture" turns on. */
    ac->local_playback = (int)config_get_int("LOCAL_PLAYBACK", 1, 0, 1);
    ac->local_wanted = local_output;
    ac->local_volume = 25; /* the source's own level; see the header */
    if (source && source[0]) {
        snprintf(ac->source_buf, sizeof(ac->source_buf), "%s", source);
        ac->source = ac->source_buf;
    } else {
        ac->source = NULL; /* let PulseAudio pick its default source */
    }

    ac->thread = SDL_CreateThread(audio_thread, "audio", ac);
    if (!ac->thread) {
        fprintf(stderr, "audio_capture: SDL_CreateThread failed\n");
        free(ac);
        return NULL;
    }
    return ac;
}

static int local_playback_open(AudioCapture *ac) {
    if (ac->ring.play) {
        return 0;
    }
    int error = 0;
    if (!ac->ring.mutex) ac->ring.mutex = SDL_CreateMutex();
    if (!ac->ring.cond) ac->ring.cond = SDL_CreateCond();
    if (!ac->ring.data) {
        ac->ring.data = malloc(PLAYBACK_RING_CHUNKS * ac->ring.chunk_samples * sizeof(int16_t));
    }
    ac->ring.head = ac->ring.tail = ac->ring.count = 0;
    ac->ring.play = pa_simple_new(NULL, APP_NAME, PA_STREAM_PLAYBACK, NULL, "Capture audio",
                                  &ac->ring.spec, NULL, &ac->ring.attr, &error);
    if (!ac->ring.data || !ac->ring.mutex || !ac->ring.cond || !ac->ring.play) {
        fprintf(stderr, "audio_capture: local playback unavailable (%s), continuing without it\n",
                pa_strerror(error));
        local_playback_close(ac);
        return -1;
    }
    ac->ring.running = 1;
    ac->playback_thread = SDL_CreateThread(playback_thread_main, "audio-playback", &ac->ring);
    if (!ac->playback_thread) {
        local_playback_close(ac);
        return -1;
    }
    fprintf(stderr, "audio_capture: local speakers on\n");
    return 0;
}

static void local_playback_close(AudioCapture *ac) {
    /* The thread first, then its stream: freeing a stream something is
     * writing to is the one ordering that cannot be recovered from. */
    if (ac->ring.mutex) {
        SDL_LockMutex(ac->ring.mutex);
        ac->ring.running = 0;
        SDL_CondSignal(ac->ring.cond);
        SDL_UnlockMutex(ac->ring.mutex);
    }
    if (ac->playback_thread) {
        SDL_WaitThread(ac->playback_thread, NULL);
        ac->playback_thread = NULL;
    }
    if (ac->ring.play) {
        int error = 0;
        pa_simple_free(ac->ring.play);
        ac->ring.play = NULL;
        (void)error;
    }
}

void audio_capture_set_local_output(AudioCapture *ac, int enabled, int volume) {
    if (!ac) {
        return;
    }
    /* Only asked for here. The capture thread owns the stream, and
     * opening a PulseAudio stream from whichever thread happened to
     * click a menu is how two threads end up owning one device. */
    ac->local_wanted = enabled ? 1 : 0;
    ac->local_volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
}

void audio_capture_set_local_mute(AudioCapture *ac, int muted) {
    if (ac) {
        ac->local_muted = muted ? 1 : 0;
    }
}

void audio_capture_stop(AudioCapture *ac) {
    if (!ac) {
        return;
    }
    SDL_WaitThread(ac->thread, NULL);
    free(ac);
}
