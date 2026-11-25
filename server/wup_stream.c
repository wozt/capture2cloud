#define _GNU_SOURCE
#include "wup_stream.h"
#include "wup_touch.h"
#include "gamepad_bridge.h"
#include "output_pcble.h"
#include "../wiiu_wupstream/shared.h"
#include <SDL2/SDL.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* WUP_STREAM_ bit assignments from the pinned wozt/wup-stream/host/wupstream.h. */
enum {
    WB_A = 0x8000, WB_B = 0x4000, WB_X = 0x2000, WB_Y = 0x1000,
    WB_LEFT = 0x0800, WB_RIGHT = 0x0400, WB_UP = 0x0200, WB_DOWN = 0x0100,
    WB_ZL = 0x0080, WB_ZR = 0x0040, WB_L = 0x0020, WB_R = 0x0010,
    WB_PLUS = 0x0008, WB_MINUS = 0x0004, WB_HOME = 0x0002,
    WB_L3 = 0x10000, WB_R3 = 0x20000
};

struct WupStream {
    int fd;
    C2cWupShared *shared;
    pid_t pid;
    char helper[PATH_MAX];
    char ip[80];
    char error[160];
    struct SwsContext *sws;
    int connected_before;
    unsigned last_input_seq;
    WupTouch touch;
    int last_touch_contact; /* physical touchscreen only, never stick axes */
};

static int axis(int v) {
    v = v * 100 / 1100;
    return v < -100 ? -100 : v > 100 ? 100 : v;
}

static void input_state(const C2cWupInput *in, int8_t out[CONTROLLER_STATE_COUNT]) {
    memset(out, 0, CONTROLLER_STATE_COUNT);
#define MAP(btn, idx) out[(idx)] = (in->buttons & (btn)) ? 100 : 0
    MAP(WB_A, CONTROLLER_FACE_EAST);
    MAP(WB_B, CONTROLLER_FACE_SOUTH);
    MAP(WB_X, CONTROLLER_FACE_NORTH);
    MAP(WB_Y, CONTROLLER_FACE_WEST);
    MAP(WB_LEFT, CONTROLLER_LEFT); MAP(WB_RIGHT, CONTROLLER_RIGHT);
    MAP(WB_UP, CONTROLLER_UP); MAP(WB_DOWN, CONTROLLER_DOWN);
    MAP(WB_ZL, CONTROLLER_L2); MAP(WB_ZR, CONTROLLER_R2);
    MAP(WB_L, CONTROLLER_L1); MAP(WB_R, CONTROLLER_R1);
    MAP(WB_PLUS, CONTROLLER_START); MAP(WB_MINUS, CONTROLLER_SELECT);
    MAP(WB_HOME, CONTROLLER_HOME);
    MAP(WB_L3, CONTROLLER_L3); MAP(WB_R3, CONTROLLER_R3);
#undef MAP
    out[CONTROLLER_LX] = (int8_t)axis(in->lx);
    out[CONTROLLER_RX] = (int8_t)axis(in->rx);
    /* WUP Stream says up is positive. Capture2Cloud's conventional
     * input representation has down positive, unlike the Nintendo HID. */
    out[CONTROLLER_LY] = (int8_t)-axis(in->ly);
    out[CONTROLLER_RY] = (int8_t)-axis(in->ry);
}

static int start_child(WupStream *w) {
    pid_t child = fork();
    if (child < 0) { snprintf(w->error, sizeof(w->error), "fork: %s", strerror(errno)); return -1; }
    if (!child) {
        char fd_buf[24];
        snprintf(fd_buf, sizeof(fd_buf), "%d", w->fd);
        execl(w->helper, w->helper, fd_buf, w->ip[0] ? w->ip : "-", (char *)NULL);
        _exit(127);
    }
    w->pid = child;
    return 0;
}

WupStream *wup_stream_start(const char *project_dir, const char *pad_ip) {
    WupStream *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->fd = -1;
    snprintf(w->helper, sizeof(w->helper), "%s/wiiu_wupstream/wup_stream_helper", project_dir);
    if (pad_ip) snprintf(w->ip, sizeof(w->ip), "%s", pad_ip);
    if (access(w->helper, X_OK) != 0) {
        snprintf(w->error, sizeof(w->error), "helper missing: run scripts/build_wupstream.sh");
        return w;
    }
    w->fd = memfd_create("capture2cloud-wupstream", 0 /* child must inherit fd */);
    if (w->fd < 0 || ftruncate(w->fd, sizeof(C2cWupShared)) != 0) {
        snprintf(w->error, sizeof(w->error), "memfd: %s", strerror(errno));
        return w;
    }
    w->shared = mmap(NULL, sizeof(C2cWupShared), PROT_READ | PROT_WRITE,
                     MAP_SHARED, w->fd, 0);
    if (w->shared == MAP_FAILED) {
        w->shared = NULL;
        snprintf(w->error, sizeof(w->error), "mmap: %s", strerror(errno));
        return w;
    }
    atomic_init(&w->shared->running, 1);
    if (start_child(w) == 0)
        snprintf(w->error, sizeof(w->error), "waiting for WUP Stream GamePad");
    return w;
}

int wup_stream_connected(const WupStream *w) {
    /* Read by the audio thread too: never access the main-thread-owned PID. */
    return w && w->shared &&
           atomic_load_explicit(&w->shared->connected, memory_order_acquire);
}

const char *wup_stream_status(WupStream *w) {
    if (!w) return "WUP Stream disabled";
    if (wup_stream_connected(w)) return "WUP Stream connected (854x480, ~60 fps)";
    return w->error;
}

void wup_stream_poll(WupStream *w, int touch_mode) {
    if (!w || !w->shared) return;
    if (w->pid > 0) {
        int status = 0;
        pid_t ret = waitpid(w->pid, &status, WNOHANG);
        if (ret == w->pid || (ret == -1 && errno == ECHILD)) {
            w->pid = 0;
            snprintf(w->error, sizeof(w->error), "WUP Stream helper exited (run build_wupstream.sh if needed)");
            atomic_store(&w->shared->connected, 0);
        }
    }
    int connected = wup_stream_connected(w);
    if (!connected) {
        if (w->connected_before) {
            gamepad_bridge_forget(GAMEPAD_SOURCE_WUPSTREAM);
            if (output_pcble_mouse_enabled()) output_pcble_mouse(0, 0, 0, 0, 0);
            wup_touch_reset(&w->touch);
            w->last_touch_contact = 0;
            w->connected_before = 0;
        }
        return;
    }
    w->connected_before = 1;
    /* Apply touch mode immediately, even if the next input report
     * has not arrived. Stop any held optical-mouse buttons.
     * Controller stick mapping is completely independent. */
    const int touch_mode_changed = !w->touch.mode_initialized ||
                                   w->touch.mode != touch_mode;
    (void)wup_touch_set_mode(&w->touch, touch_mode, w->last_touch_contact);
    if (touch_mode_changed && output_pcble_mouse_enabled())
        output_pcble_mouse(0, 0, 0, 0, 0);
    unsigned seq1 = atomic_load_explicit(&w->shared->input_seq, memory_order_acquire);
    if (seq1 == w->last_input_seq || (seq1 & 1u)) return;
    C2cWupInput report = w->shared->input;
    atomic_thread_fence(memory_order_acquire);
    unsigned seq2 = atomic_load_explicit(&w->shared->input_seq, memory_order_acquire);
    if (seq1 != seq2 || (seq2 & 1u)) return;
    w->last_input_seq = seq2;
    w->last_touch_contact = report.touched != 0;
    int8_t buttons[CONTROLLER_STATE_COUNT];
    input_state(&report, buttons);
    gamepad_bridge_update(GAMEPAD_SOURCE_WUPSTREAM, buttons);
    if (touch_mode != WUP_TOUCH_DISABLED && output_pcble_mouse_enabled()) {
        WupTouchEvent ev;
        if (wup_touch_step(&w->touch, report.touched, report.tx, report.ty,
                           touch_mode, SDL_GetTicks(), &ev))
            output_pcble_mouse(ev.dx, ev.dy, ev.wheel, ev.buttons, ev.active);
    } else if (!output_pcble_mouse_enabled() &&
               (w->touch.touching || w->touch.pulse_button)) {
        wup_touch_reset(&w->touch);
    }
}

static enum AVPixelFormat pixel_format(VideoPixelFormat p) {
    switch (p) {
        case VIDEO_PIXEL_YUYV422: return AV_PIX_FMT_YUYV422;
        case VIDEO_PIXEL_YUV420P: return AV_PIX_FMT_YUV420P;
        case VIDEO_PIXEL_YUV422P: return AV_PIX_FMT_YUV422P;
        case VIDEO_PIXEL_RGB24: return AV_PIX_FMT_RGB24;
        default: return AV_PIX_FMT_NONE;
    }
}

void wup_stream_push_video(WupStream *w, const VideoFrame *frame, int width, int height) {
    if (!wup_stream_connected(w) || !frame || width <= 0 || height <= 0) return;
    enum AVPixelFormat pix = pixel_format(frame->pixel);
    if (pix == AV_PIX_FMT_NONE) return;
    int chosen = -1, expected = 0;
    for (int k = 0; k < C2C_WUP_VIDEO_SLOTS; ++k) {
        expected = 0;
        if (atomic_compare_exchange_strong(&w->shared->video_state[k], &expected, 1)) {
            chosen = k; break;
        }
    }
    if (chosen < 0) {
        unsigned oldest = UINT_MAX;
        for (int k = 0; k < C2C_WUP_VIDEO_SLOTS; ++k) {
            if (atomic_load(&w->shared->video_state[k]) == 2 &&
                w->shared->video_sequence[k] < oldest) {
                chosen = k;
                oldest = w->shared->video_sequence[k];
            }
        }
        if (chosen < 0) return;
        expected = 2;
        if (!atomic_compare_exchange_strong(&w->shared->video_state[chosen], &expected, 1)) return;
    }
    w->sws = sws_getCachedContext(w->sws, width, height, pix,
                                   C2C_WUP_W, C2C_WUP_H, AV_PIX_FMT_BGRA,
                                   SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!w->sws) {
        atomic_store(&w->shared->video_state[chosen], 0);
        return;
    }
    const uint8_t *src[4] = {frame->plane[0], frame->plane[1], frame->plane[2], NULL};
    int stride[4] = {frame->stride[0], frame->stride[1], frame->stride[2], 0};
    uint8_t *dst[4] = {w->shared->video[chosen], NULL, NULL, NULL};
    int out_stride[4] = {C2C_WUP_W * 4, 0, 0, 0};
    if (sws_scale(w->sws, src, stride, 0, height, dst, out_stride) <= 0) {
        atomic_store(&w->shared->video_state[chosen], 0);
        return;
    }
    w->shared->video_sequence[chosen] = atomic_fetch_add(&w->shared->frame_generation, 1) + 1;
    atomic_store_explicit(&w->shared->video_state[chosen], 2, memory_order_release);
}

void wup_stream_push_audio(WupStream *w, const int16_t *samples, size_t frames) {
    if (!wup_stream_connected(w) || !samples || !frames) return;
    C2cWupShared *m = w->shared;
    unsigned wr = atomic_load_explicit(&m->audio_write, memory_order_relaxed);
    unsigned rd = atomic_load_explicit(&m->audio_read, memory_order_acquire);
    if (wr - rd >= C2C_WUP_AUDIO_FRAMES) return;
    if (frames > C2C_WUP_AUDIO_FRAMES - (wr - rd))
        frames = C2C_WUP_AUDIO_FRAMES - (wr - rd);
    for (size_t k = 0; k < frames; ++k) {
        unsigned idx = ((wr + (unsigned)k) % C2C_WUP_AUDIO_FRAMES) * 2;
        m->audio[idx] = samples[k * 2]; m->audio[idx + 1] = samples[k * 2 + 1];
    }
    atomic_store_explicit(&m->audio_write, wr + (unsigned)frames, memory_order_release);
}

void wup_stream_stop(WupStream *w) {
    if (!w) return;
    if (w->shared) atomic_store(&w->shared->running, 0);
    if (w->pid > 0) {
        kill(w->pid, SIGTERM);
        for (int n = 0; n < 20; ++n) {
            if (waitpid(w->pid, NULL, WNOHANG) == w->pid) { w->pid = 0; break; }
            SDL_Delay(25);
        }
        if (w->pid > 0) { kill(w->pid, SIGKILL); waitpid(w->pid, NULL, 0); }
    }
    if (w->connected_before) {
        gamepad_bridge_forget(GAMEPAD_SOURCE_WUPSTREAM);
        if (output_pcble_mouse_enabled()) output_pcble_mouse(0, 0, 0, 0, 0);
    }
    sws_freeContext(w->sws);
    if (w->shared) munmap(w->shared, sizeof(C2cWupShared));
    if (w->fd >= 0) close(w->fd);
    free(w);
}
