#define _GNU_SOURCE

#include "video_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include <jpeglib.h>
#include <linux/videodev2.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct buffer {
    void *start;
    size_t length;
};

struct VideoCapture {
    int fd;
    struct buffer *buffers;
    unsigned int buffer_count;
    unsigned int width;
    unsigned int height;
    VideoFormat format;

    /* Where MJPEG frames are decoded to; unused (and never allocated)
     * for YUYV, where frames are handed over straight from the mapped
     * buffer.
     *
     * Normally the JPEG's own three planes, untouched. `rgb` is the
     * fallback for a JPEG whose subsampling is neither 4:2:0 nor 4:2:2 --
     * the two that can be handed downstream as they are. */
    uint8_t *planes[3];
    int plane_stride[3];
    size_t plane_size[3];
    VideoPixelFormat decoded_pixel;
    uint8_t *rgb;

    /* Index of the buffer currently lent out to the caller, or -1.
     * It is handed back to the driver at the START of the next read
     * rather than immediately, which is what allows YUYV frames to be
     * used in place with no copy. */
    int held_index;
};

/* libjpeg aborts the process on error by default; this makes it long-jump
 * back to us instead, so a single corrupt frame is skipped rather than
 * taking the whole app down. These capture sticks do emit the occasional
 * malformed JPEG. */
struct jpeg_error_state {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    struct jpeg_error_state *err = (struct jpeg_error_state *)cinfo->err;
    longjmp(err->jump, 1);
}

/* ioctl retried across signal interruptions: without this, any signal
 * (the GTK thread's timers included) can make a perfectly good call
 * fail with EINTR. */
static int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static uint32_t v4l2_format_of(VideoFormat f) {
    return (f == VIDEO_FORMAT_YUYV) ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
}

/* -1 when nothing is pending. Written by the HTTP thread, read by the
 * capture loop; a single int, so no lock is needed for it. */
static volatile int g_format_request = -1;
static volatile int g_active_format = VIDEO_FORMAT_YUYV;

void video_capture_request_format(VideoFormat format) {
    g_format_request = (int)format;
}

int video_capture_take_format_request(VideoFormat *out) {
    int req = g_format_request;
    if (req < 0) {
        return 0;
    }
    g_format_request = -1;
    if (out) {
        *out = (VideoFormat)req;
    }
    return 1;
}

VideoFormat video_capture_active_format(void) {
    return (VideoFormat)g_active_format;
}

/* Lower case: this is both what the .env and the HTTP endpoint use, and
 * what the log prints. One spelling, one function. */
const char *video_format_name(VideoFormat format) {
    return format == VIDEO_FORMAT_YUYV ? "yuyv" : "mjpeg";
}

/* --- screen-change watch --------------------------------------------
 *
 * Cheap on purpose: this runs on every captured frame. A grid of samples
 * across the buffer, compared byte for byte, is enough to tell a static
 * generated pattern from a console that has started drawing.
 */
#define CHANGE_SAMPLES 1024
/* A single sample differing by less than this is noise, not content. */
#define CHANGE_SAMPLE_DELTA 12
/* How many of the samples must differ before the picture counts as
 * changed. Low enough to catch a console drawing into one corner, high
 * enough to ignore a pattern with an animated element. */
#define CHANGE_MIN_SAMPLES (CHANGE_SAMPLES / 16)
/* Consecutive changed frames required. One frame could be a glitch; the
 * card does emit the occasional corrupt one. */
#define CHANGE_MIN_FRAMES 3
/* Give up waiting and report a change anyway, so a wake never leaves the
 * follow-up undone. */
#define CHANGE_TIMEOUT_FRAMES (60 * 60)

static volatile int g_change_watching = 0;
static volatile int g_change_detected = 0;
static uint8_t g_change_reference[CHANGE_SAMPLES];
static int g_change_have_reference = 0;
static int g_change_streak = 0;
static int g_change_frames_waited = 0;

void video_capture_watch_for_change(void) {
    g_change_have_reference = 0;
    g_change_streak = 0;
    g_change_frames_waited = 0;
    g_change_detected = 0;
    g_change_watching = 1;
}

int video_capture_take_change_detected(void) {
    if (!g_change_detected) {
        return 0;
    }
    g_change_detected = 0;
    return 1;
}

/* Samples the frame into `out`, evenly spread over the whole buffer so
 * a change anywhere on screen is visible. */
static void sample_frame(const uint8_t *pixels, size_t bytes, uint8_t *out) {
    if (bytes < CHANGE_SAMPLES) {
        memset(out, 0, CHANGE_SAMPLES);
        return;
    }
    size_t step = bytes / CHANGE_SAMPLES;
    for (int i = 0; i < CHANGE_SAMPLES; i++) {
        out[i] = pixels[i * step];
    }
}

static void change_watch_feed(const uint8_t *pixels, size_t bytes) {
    if (!g_change_watching) {
        return;
    }
    uint8_t now[CHANGE_SAMPLES];
    sample_frame(pixels, bytes, now);

    if (!g_change_have_reference) {
        memcpy(g_change_reference, now, sizeof(now));
        g_change_have_reference = 1;
        return;
    }

    int differing = 0;
    for (int i = 0; i < CHANGE_SAMPLES; i++) {
        int d = (int)now[i] - (int)g_change_reference[i];
        if (d < 0) d = -d;
        if (d > CHANGE_SAMPLE_DELTA) differing++;
    }

    if (differing >= CHANGE_MIN_SAMPLES) {
        if (++g_change_streak >= CHANGE_MIN_FRAMES) {
            fprintf(stderr, "video_capture: picture changed (%d/%d samples), console is up\n",
                    differing, CHANGE_SAMPLES);
            g_change_watching = 0;
            g_change_detected = 1;
        }
    } else {
        g_change_streak = 0;
    }

    if (++g_change_frames_waited > CHANGE_TIMEOUT_FRAMES) {
        fprintf(stderr, "video_capture: picture never changed, giving up the watch\n");
        g_change_watching = 0;
        g_change_detected = 1;
    }
}

static int open_device(VideoCapture *vc, const char *device, VideoFormat wanted) {
    int fd = open(device, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        perror("open video");
        return -1;
    }

    /* Ask for the preferred format, and fall back to the other one if
     * the driver substitutes something else -- V4L2 is allowed to
     * silently pick a format it does support rather than failing. */
    VideoFormat chosen = wanted;
    struct v4l2_format fmt;
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = vc->width;
        fmt.fmt.pix.height = vc->height;
        fmt.fmt.pix.pixelformat = v4l2_format_of(chosen);
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        int rc = xioctl(fd, VIDIOC_S_FMT, &fmt);
        if (rc == 0 && fmt.fmt.pix.pixelformat == v4l2_format_of(chosen)) {
            break;
        }
        /* A device already streaming for someone else refuses every
         * format, which used to be reported as "accepts neither YUYV nor
         * MJPEG" -- a message that sends you looking at the card when the
         * answer is that a second copy of this program is already
         * running. Say what actually happened. */
        if (rc < 0 && errno == EBUSY) {
            fprintf(stderr, "video_capture: %s is busy -- another program (or a second instance "
                            "of this one) already has it open\n", device);
            close(fd);
            return -1;
        }
        if (attempt == 0) {
            VideoFormat other = (chosen == VIDEO_FORMAT_YUYV) ? VIDEO_FORMAT_MJPEG : VIDEO_FORMAT_YUYV;
            fprintf(stderr, "video_capture: %s unavailable on this device, trying %s\n",
                    video_format_name(chosen), video_format_name(other));
            chosen = other;
        } else {
            fprintf(stderr, "video_capture: device accepts neither YUYV nor MJPEG at %ux%u\n",
                    vc->width, vc->height);
            close(fd);
            return -1;
        }
    }
    vc->format = chosen;
    g_active_format = (int)chosen;
    vc->width = fmt.fmt.pix.width;
    vc->height = fmt.fmt.pix.height;
    fprintf(stderr, "video_capture: %s %ux%u\n", video_format_name(chosen), vc->width, vc->height);

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

    vc->buffers = calloc(req.count, sizeof(*vc->buffers));
    if (!vc->buffers) {
        close(fd);
        return -1;
    }
    vc->buffer_count = req.count;

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

        vc->buffers[i].length = buf.length;
        vc->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (vc->buffers[i].start == MAP_FAILED) {
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

    /* Hand the descriptor to the context: everything else (poll,
     * DQBUF/QBUF, teardown) reads it from there. */
    vc->fd = fd;
    return 0;
}

static void close_device(VideoCapture *vc) {
    if (vc->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(vc->fd, VIDIOC_STREAMOFF, &type);
    }

    for (unsigned int i = 0; i < vc->buffer_count; i++) {
        if (vc->buffers[i].start && vc->buffers[i].start != MAP_FAILED) {
            munmap(vc->buffers[i].start, vc->buffers[i].length);
        }
    }

    free(vc->buffers);
    vc->buffers = NULL;
    vc->buffer_count = 0;

    if (vc->fd >= 0) {
        close(vc->fd);
        vc->fd = -1;
    }
}

/* `exp_width`/`exp_height` are what the caller's RGB buffer was sized
 * for: a frame that decodes to any other size is rejected rather than
 * written past the end of that buffer. */
static int decode_mjpeg(const unsigned char *jpeg, unsigned long jpeg_size, unsigned char *rgb, int stride,
                        unsigned int exp_width, unsigned int exp_height) {
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

    if (cinfo.output_width != exp_width || cinfo.output_height != exp_height || cinfo.output_components != 3) {
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

static unsigned round_up(unsigned value, unsigned multiple) {
    return ((value + multiple - 1) / multiple) * multiple;
}

/* Decodes straight into the JPEG's own planes.
 *
 * `raw_data_out` skips both the chroma upsampling and the conversion to
 * RGB. The point is not mainly the decode -- 2.97 ms against 3.63 ms at
 * 1080p -- but what it saves downstream: every consumer wants YUV, and
 * converting RGB back to it costs 5.11 ms a frame that nobody was
 * getting anything for.
 *
 * libjpeg hands over one MCU row at a time and expects somewhere to put
 * a whole one, which is why the buffers are allocated rounded up to a
 * multiple of the MCU height rather than to the picture's.
 *
 * Returns 0 on success, -1 if the frame is unusable, and 1 if this JPEG's
 * subsampling is not one this can hand over -- the caller then falls back
 * to decoding it as RGB. */
static int decode_mjpeg_planes(VideoCapture *vc, const unsigned char *jpeg, unsigned long size) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_state jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg, size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    if (cinfo.num_components != 3 || cinfo.jpeg_color_space != JCS_YCbCr ||
        cinfo.image_width != vc->width || cinfo.image_height != vc->height) {
        jpeg_destroy_decompress(&cinfo);
        return 1;
    }

    const int hr = cinfo.max_h_samp_factor / cinfo.comp_info[1].h_samp_factor;
    const int vr = cinfo.max_v_samp_factor / cinfo.comp_info[1].v_samp_factor;
    VideoPixelFormat pixel;
    if (hr == 2 && vr == 1) {
        pixel = VIDEO_PIXEL_YUV422P;
    } else if (hr == 2 && vr == 2) {
        pixel = VIDEO_PIXEL_YUV420P;
    } else {
        jpeg_destroy_decompress(&cinfo);
        return 1; /* 4:4:4 and the odd ones: not worth a third code path */
    }
    if (cinfo.comp_info[1].h_samp_factor != cinfo.comp_info[2].h_samp_factor ||
        cinfo.comp_info[1].v_samp_factor != cinfo.comp_info[2].v_samp_factor) {
        jpeg_destroy_decompress(&cinfo);
        return 1;
    }

    const int mcu_rows = cinfo.max_v_samp_factor * DCTSIZE;
    const int y_stride = (int)round_up(vc->width, (unsigned)(cinfo.max_h_samp_factor * DCTSIZE));
    const int y_rows = (int)round_up(vc->height, (unsigned)mcu_rows);
    const int c_stride = y_stride / hr;
    const int c_rows = y_rows / vr;

    if (vc->plane_stride[0] != y_stride || vc->plane_size[0] != (size_t)y_stride * y_rows) {
        for (int i = 0; i < 3; i++) {
            free(vc->planes[i]);
            vc->planes[i] = NULL;
        }
        vc->plane_size[0] = (size_t)y_stride * y_rows;
        vc->plane_size[1] = vc->plane_size[2] = (size_t)c_stride * c_rows;
        vc->plane_stride[0] = y_stride;
        vc->plane_stride[1] = vc->plane_stride[2] = c_stride;
        for (int i = 0; i < 3; i++) {
            vc->planes[i] = malloc(vc->plane_size[i]);
            if (!vc->planes[i]) {
                jpeg_destroy_decompress(&cinfo);
                return -1;
            }
        }
    }

    cinfo.raw_data_out = TRUE;
    cinfo.out_color_space = JCS_YCbCr;
    cinfo.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&cinfo);

    /* DCTSIZE is 8 and max_v_samp_factor is at most 4, so a fixed 32 is
     * the most libjpeg can ask for. */
    JSAMPROW y_rows_p[32], cb_rows_p[32], cr_rows_p[32];
    JSAMPARRAY planes[3] = {y_rows_p, cb_rows_p, cr_rows_p};
    while (cinfo.output_scanline < cinfo.output_height) {
        const int line = (int)cinfo.output_scanline;
        for (int i = 0; i < mcu_rows; i++) {
            y_rows_p[i] = vc->planes[0] + (size_t)(line + i) * y_stride;
        }
        for (int i = 0; i < mcu_rows / vr; i++) {
            size_t off = (size_t)(line / vr + i) * c_stride;
            cb_rows_p[i] = vc->planes[1] + off;
            cr_rows_p[i] = vc->planes[2] + off;
        }
        jpeg_read_raw_data(&cinfo, planes, (JDIMENSION)mcu_rows);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    vc->decoded_pixel = pixel;
    return 0;
}

VideoFormat video_format_from_name(const char *name) {
    if (name) {
        if (strcasecmp(name, "yuyv") == 0) return VIDEO_FORMAT_YUYV;
        if (strcasecmp(name, "mjpeg") == 0 || strcasecmp(name, "mjpg") == 0) return VIDEO_FORMAT_MJPEG;
        if (name[0]) {
            fprintf(stderr, "video_capture: unknown CAPTURE_FORMAT '%s', using yuyv\n", name);
        }
    }
    return VIDEO_FORMAT_YUYV;
}

VideoCapture *video_capture_open(const char *device, VideoFormat format, unsigned int *width,
                                 unsigned int *height) {
    VideoCapture *vc = calloc(1, sizeof(*vc));
    if (!vc) {
        return NULL;
    }
    vc->fd = -1;
    vc->held_index = -1;
    vc->width = (width && *width) ? *width : 1920;
    vc->height = (height && *height) ? *height : 1080;

    if (open_device(vc, device, format) < 0) {
        free(vc);
        return NULL;
    }

    /* Nothing is allocated for the decode here: the plane sizes come
     * from the first JPEG's own sampling factors, which are not known
     * until one arrives.
     */

    if (width) *width = vc->width;
    if (height) *height = vc->height;
    return vc;
}

void video_capture_close(VideoCapture *vc) {
    if (!vc) {
        return;
    }
    close_device(vc);
    for (int i = 0; i < 3; i++) {
        free(vc->planes[i]);
    }
    free(vc->rgb);
    free(vc);
}

VideoFormat video_capture_format(const VideoCapture *vc) {
    return vc ? vc->format : VIDEO_FORMAT_YUYV;
}

int video_capture_fd(const VideoCapture *vc) {
    return vc ? vc->fd : -1;
}

int video_capture_read(VideoCapture *vc, VideoFrame *out) {
    /* Hand back the buffer lent out last time. Deferring it to here --
     * rather than requeueing right after the read -- is what lets a
     * YUYV frame be used in place, with no copy anywhere. */
    if (vc->held_index >= 0) {
        struct v4l2_buffer done;
        memset(&done, 0, sizeof(done));
        done.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        done.memory = V4L2_MEMORY_MMAP;
        done.index = (unsigned int)vc->held_index;
        vc->held_index = -1;
        if (xioctl(vc->fd, VIDIOC_QBUF, &done) < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vc->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) {
            return 0; /* nothing ready yet */
        }
        perror("VIDIOC_DQBUF");
        return -1;
    }

    if (vc->format == VIDEO_FORMAT_YUYV) {
        /* Straight out of the mapped buffer: no decode, no copy. */
        vc->held_index = (int)buf.index;
        out->plane[0] = vc->buffers[buf.index].start;
        out->stride[0] = (int)vc->width * 2;
        out->plane[1] = out->plane[2] = NULL;
        out->stride[1] = out->stride[2] = 0;
        out->pixel = VIDEO_PIXEL_YUYV422;
        change_watch_feed(out->plane[0], (size_t)out->stride[0] * vc->height);
        return 1;
    }

    /* The planes first; RGB only for a JPEG this cannot hand over as it
     * is, which the reference card never produces. */
    int planar = decode_mjpeg_planes(vc, vc->buffers[buf.index].start, buf.bytesused);
    int ok = (planar == 0);
    if (planar == 1) {
        if (!vc->rgb) {
            vc->rgb = malloc((size_t)vc->width * vc->height * 3);
        }
        ok = vc->rgb && decode_mjpeg(vc->buffers[buf.index].start, buf.bytesused, vc->rgb,
                                     (int)vc->width * 3, vc->width, vc->height) == 0;
        if (ok) {
            vc->decoded_pixel = VIDEO_PIXEL_RGB24;
        }
    }

    /* The decode copied everything out, so this buffer can go back
     * immediately -- nothing points into it. */
    if (xioctl(vc->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("VIDIOC_QBUF");
        return -1;
    }
    if (!ok) {
        return 0; /* corrupt frame, skipped */
    }

    out->pixel = vc->decoded_pixel;
    if (out->pixel == VIDEO_PIXEL_RGB24) {
        out->plane[0] = vc->rgb;
        out->stride[0] = (int)vc->width * 3;
        out->plane[1] = out->plane[2] = NULL;
        out->stride[1] = out->stride[2] = 0;
    } else {
        for (int i = 0; i < 3; i++) {
            out->plane[i] = vc->planes[i];
            out->stride[i] = vc->plane_stride[i];
        }
    }
    /* The watch only ever compares samples with the previous frame, and
     * luma alone is what "the picture changed" means. */
    change_watch_feed(out->plane[0], (size_t)out->stride[0] * vc->height);
    return 1;
}
