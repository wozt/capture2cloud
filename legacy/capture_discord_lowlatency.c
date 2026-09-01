#define _GNU_SOURCE

#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define APP_NAME "Capture Discord"
#define AUDIO_SOURCE "alsa_input.usb-MACROSILICON_USB3_Video_20210623-02.pro-input-0"
#define AUDIO_GATE_THRESHOLD 0
#define AUDIO_GATE_RELEASE_CHUNKS 20
#define AUDIO_HIGHPASS_ALPHA 0.995f
#define AUDIO_NOTCH_COUNT 9
#define AUDIO_NOTCH_Q 20.0f

struct buffer {
    void *start;
    size_t length;
};

struct app {
    int video_fd;
    struct buffer *buffers;
    unsigned int buffer_count;
    unsigned int width;
    unsigned int height;
    volatile sig_atomic_t running;
};

struct jpeg_error_state {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

struct biquad {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1[2];
    float z2[2];
};

static struct app g_app = {
    .video_fd = -1,
    .buffers = NULL,
    .buffer_count = 0,
    .width = 1920,
    .height = 1080,
    .running = 1,
};

static int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static void on_signal(int sig) {
    (void)sig;
    g_app.running = 0;
}

static void jpeg_error_exit(j_common_ptr cinfo) {
    struct jpeg_error_state *err = (struct jpeg_error_state *)cinfo->err;
    longjmp(err->jump, 1);
}

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

static int open_video(const char *device) {
    int fd = open(device, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        perror("open video");
        return -1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = g_app.width;
    fmt.fmt.pix.height = g_app.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        close(fd);
        return -1;
    }

    g_app.width = fmt.fmt.pix.width;
    g_app.height = fmt.fmt.pix.height;

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 60;
    xioctl(fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        close(fd);
        return -1;
    }

    g_app.buffers = calloc(req.count, sizeof(*g_app.buffers));
    if (!g_app.buffers) {
        close(fd);
        return -1;
    }
    g_app.buffer_count = req.count;

    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            close(fd);
            return -1;
        }

        g_app.buffers[i].length = buf.length;
        g_app.buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (g_app.buffers[i].start == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return -1;
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            close(fd);
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        close(fd);
        return -1;
    }

    return fd;
}

static void close_video(void) {
    if (g_app.video_fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(g_app.video_fd, VIDIOC_STREAMOFF, &type);
    }

    for (unsigned int i = 0; i < g_app.buffer_count; i++) {
        if (g_app.buffers[i].start && g_app.buffers[i].start != MAP_FAILED) {
            munmap(g_app.buffers[i].start, g_app.buffers[i].length);
        }
    }

    free(g_app.buffers);
    g_app.buffers = NULL;
    g_app.buffer_count = 0;

    if (g_app.video_fd >= 0) {
        close(g_app.video_fd);
        g_app.video_fd = -1;
    }
}

static int decode_mjpeg_to_rgb(const unsigned char *jpeg, unsigned long jpeg_size, unsigned char *rgb, int stride) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_state jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg, jpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    if (cinfo.output_width != g_app.width || cinfo.output_height != g_app.height || cinfo.output_components != 3) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *row = rgb + cinfo.output_scanline * stride;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

static int audio_thread(void *unused) {
    (void)unused;

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

    int error = 0;
    pa_simple *rec = pa_simple_new(NULL, APP_NAME, PA_STREAM_RECORD, AUDIO_SOURCE, "HDMI capture", &ss, NULL, &rec_attr, &error);
    if (!rec) {
        fprintf(stderr, "Pulse record: %s\n", pa_strerror(error));
        return 1;
    }

    pa_simple *play = pa_simple_new(NULL, APP_NAME, PA_STREAM_PLAYBACK, NULL, "Capture audio", &ss, NULL, &play_attr, &error);
    if (!play) {
        fprintf(stderr, "Pulse playback: %s\n", pa_strerror(error));
        pa_simple_free(rec);
        return 1;
    }

    int16_t samples[64 * 2];
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

    while (g_app.running) {
        if (pa_simple_read(rec, samples, sizeof(samples), &error) < 0) {
            fprintf(stderr, "Pulse read: %s\n", pa_strerror(error));
            break;
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

        if (pa_simple_write(play, samples, sizeof(samples), &error) < 0) {
            fprintf(stderr, "Pulse write: %s\n", pa_strerror(error));
            break;
        }
    }

    pa_simple_drain(play, &error);
    pa_simple_free(play);
    pa_simple_free(rec);
    return 0;
}

int main(int argc, char **argv) {
    const char *video_device = argc > 1 ? argv[1] : "/dev/video0";

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_app.video_fd = open_video(video_device);
    if (g_app.video_fd < 0) {
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        close_video();
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

    SDL_Window *window = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          (int)g_app.width, (int)g_app.height,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        close_video();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    }
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        close_video();
        return 1;
    }

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                             (int)g_app.width, (int)g_app.height);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        close_video();
        return 1;
    }

    unsigned char *rgb = malloc(g_app.width * g_app.height * 3);
    if (!rgb) {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        close_video();
        return 1;
    }

    SDL_Thread *audio = SDL_CreateThread(audio_thread, "capture-audio", NULL);
    if (!audio) {
        fprintf(stderr, "SDL_CreateThread: %s\n", SDL_GetError());
    }

    while (g_app.running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                g_app.running = 0;
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.clicks == 2) {
                Uint32 flags = SDL_GetWindowFlags(window);
                Uint32 fullscreen = flags & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F11) {
                Uint32 flags = SDL_GetWindowFlags(window);
                Uint32 fullscreen = flags & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }

        struct pollfd pfd;
        pfd.fd = g_app.video_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (pr == 0) {
            continue;
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(g_app.video_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            perror("VIDIOC_DQBUF");
            break;
        }

        if (decode_mjpeg_to_rgb(g_app.buffers[buf.index].start, buf.bytesused, rgb, (int)g_app.width * 3) == 0) {
            SDL_UpdateTexture(texture, NULL, rgb, (int)g_app.width * 3);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
        }

        if (xioctl(g_app.video_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            break;
        }
    }

    g_app.running = 0;
    if (audio) {
        SDL_WaitThread(audio, NULL);
    }

    free(rgb);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    close_video();
    return 0;
}
