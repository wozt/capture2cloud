#define _POSIX_C_SOURCE 200809L
#include "shared.h"
#include "wupstream.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void terminate(int unused) { (void)unused; interrupted = 1; }

static void set_input(C2cWupShared *m, const wupstream_input_t *in) {
    /* Single writer: publish a seqlock snapshot to the parent process. */
    atomic_fetch_add_explicit(&m->input_seq, 1, memory_order_acq_rel);
    if (in) {
        m->input.buttons = in->buttons;
        m->input.lx = in->lx; m->input.ly = in->ly;
        m->input.rx = in->rx; m->input.ry = in->ry;
        m->input.touched = in->touched;
        m->input.tx = in->tx; m->input.ty = in->ty;
    } else {
        memset(&m->input, 0, sizeof(m->input));
    }
    atomic_fetch_add_explicit(&m->input_seq, 1, memory_order_release);
}

static int newest_frame(C2cWupShared *m, unsigned char *last,
                        unsigned *sequence) {
    int best = -1;
    unsigned best_seq = 0;
    for (int k = 0; k < C2C_WUP_VIDEO_SLOTS; ++k) {
        if (atomic_load_explicit(&m->video_state[k], memory_order_acquire) == 2 &&
            (best < 0 || m->video_sequence[k] > best_seq)) {
            best = k; best_seq = m->video_sequence[k];
        }
    }
    if (best < 0) return 0;
    int expected = 2;
    if (!atomic_compare_exchange_strong_explicit(&m->video_state[best], &expected, 3,
                                                  memory_order_acq_rel, memory_order_acquire))
        return 0;
    if (sequence) *sequence = best_seq;
    memcpy(last, m->video[best], C2C_WUP_VIDEO_SIZE);
    atomic_store_explicit(&m->video_state[best], 0, memory_order_release);
    return 1;
}

static void pull_audio(C2cWupShared *m, int16_t *out, unsigned frames) {
    unsigned r = atomic_load_explicit(&m->audio_read, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&m->audio_write, memory_order_acquire);
    unsigned available = w - r;
    if (available > C2C_WUP_AUDIO_FRAMES) { r = w - C2C_WUP_AUDIO_FRAMES; available = C2C_WUP_AUDIO_FRAMES; }
    /* Fast-forward excess queued sound: low latency beats stale playback. */
    if (available > 2400u) { r = w - 1600u; available = 1600u; }
    unsigned n = available < frames ? available : frames;
    for (unsigned i = 0; i < n; ++i) {
        unsigned idx = ((r + i) % C2C_WUP_AUDIO_FRAMES) * 2;
        out[i * 2] = m->audio[idx]; out[i * 2 + 1] = m->audio[idx + 1];
    }
    if (n < frames) memset(out + n * 2, 0, (frames - n) * 2 * sizeof(int16_t));
    atomic_store_explicit(&m->audio_read, r + n, memory_order_release);
}

static double monotonic(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1000000000.0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "Usage: wup_stream_helper MEMFD PAD_IP_OR_DASH\n"); return 2; }
    char *end = NULL;
    long n = strtol(argv[1], &end, 10);
    if (!end || *end || n < 0 || n > 65535) return 2;
    struct stat st;
    if (fstat((int)n, &st) != 0 || st.st_size != (off_t)sizeof(C2cWupShared)) {
        fprintf(stderr, "wup_stream: invalid shared memory descriptor\n"); return 2;
    }
    C2cWupShared *m = mmap(NULL, sizeof(*m), PROT_READ | PROT_WRITE, MAP_SHARED, (int)n, 0);
    if (m == MAP_FAILED) { perror("wup_stream mmap"); return 1; }
    signal(SIGTERM, terminate); signal(SIGINT, terminate);
    unsigned char *last = calloc(C2C_WUP_VIDEO_SIZE, 1);
    if (!last) return 1;
    const char *ip = strcmp(argv[2], "-") ? argv[2] : NULL;
    while (!interrupted && atomic_load(&m->running)) {
        wupstream *pad = wupstream_open(ip, 2000);
        if (!pad) {
            struct timespec t = {1, 0}; nanosleep(&t, NULL);
            continue;
        }
        fprintf(stderr, "wup_stream: GamePad connected\n");
        atomic_store_explicit(&m->connected, 1, memory_order_release);
        atomic_store(&m->audio_read, atomic_load(&m->audio_write));
        double prev = monotonic(), audio_remainder = 0;
        double sample_started = prev, frame_total = 0;
        unsigned tx = 0, fresh = 0, dropped = 0, errors = 0;
        unsigned last_seq = 0;
        unsigned last_captured = atomic_load_explicit(&m->frame_generation, memory_order_acquire);
        atomic_store(&m->stats_tx_fps10, 0);
        while (!interrupted && atomic_load(&m->running)) {
            wupstream_input_t in;
            if (wupstream_input(pad, &in) != 0) break;
            set_input(m, &in);
            unsigned seq = 0;
            if (newest_frame(m, last, &seq)) {
                fresh++;
                if (last_seq && seq > last_seq + 1) dropped += seq - last_seq - 1;
                last_seq = seq;
            }
            const double frame_started = monotonic();
            int sent = wupstream_frame(pad, last, C2C_WUP_W * 4, WUPSTREAM_BGRA);
            frame_total += (monotonic() - frame_started) * 1000000.0;
            if (sent != 0) { errors++; break; }
            tx++;
            double now = monotonic();
            if (now - sample_started >= 1.0) {
                const double span = now - sample_started;
                const unsigned total_captured = atomic_load_explicit(
                    &m->frame_generation, memory_order_acquire);
                atomic_store(&m->stats_new_fps10, (unsigned)(10.0 * fresh / span + .5));
                atomic_store(&m->stats_capture_fps10,
                    (unsigned)(10.0 * (total_captured - last_captured) / span + .5));
                atomic_store(&m->stats_repeat, tx >= fresh ? tx - fresh : 0);
                atomic_store(&m->stats_drop, dropped);
                atomic_store(&m->stats_error, errors);
                atomic_store(&m->stats_frame_us, tx ? (unsigned)(frame_total / tx) : 0);
                atomic_store_explicit(&m->stats_tx_fps10,
                    (unsigned)(10.0 * tx / span + .5), memory_order_release);
                sample_started = now; tx = fresh = dropped = errors = 0;
                frame_total = 0;
                last_captured = total_captured;
            }
            double elapsed = now - prev;
            prev = now;
            if (elapsed < 0.0) elapsed = 0.0;
            if (elapsed > .05) elapsed = .05;
            audio_remainder += elapsed * 48000.0;
            unsigned nf = (unsigned)audio_remainder;
            if (nf > 2048) nf = 2048;
            audio_remainder -= nf;
            if (nf) {
                int16_t samples[2048 * 2];
                pull_audio(m, samples, nf);
                if (wupstream_audio(pad, samples, (int)nf) != 0) break;
            }
        }
        atomic_store(&m->stats_tx_fps10, 0);
        atomic_store_explicit(&m->connected, 0, memory_order_release);
        set_input(m, NULL);
        wupstream_close(pad);
        fprintf(stderr, "wup_stream: GamePad disconnected\n");
    }
    atomic_store(&m->connected, 0);
    set_input(m, NULL);
    free(last);
    munmap(m, sizeof(*m));
    return 0;
}
