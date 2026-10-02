#ifndef CAPTURE2CLOUD_VIDEO_CAPTURE_H
#define CAPTURE2CLOUD_VIDEO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

/*
 * V4L2 capture from the HDMI-to-USB card, in one of two formats.
 *
 * YUYV is the default, because it is the cheaper of the two by a wide
 * margin: the card's raw 4:2:2 frames are handed over untouched, with no
 * JPEG decode at all. Measured on the whole process at 1080p60 with a
 * browser and the console both watching: 149% of a core against 186% for
 * MJPEG. The catch is bandwidth -- raw 1080p60 is ~250 MB/s, which needs
 * a real USB3 path.
 *
 * MJPEG was the default for a while, chosen when raw YUYV was suspected
 * of the long-session freezes. That suspicion was never confirmed, and
 * the toggle exists precisely so the two can be compared over hours; if
 * the freezes come back on YUYV, this is the first thing to change.
 *
 * Both now hand over YUV, so neither pays for a colour conversion. The
 * MJPEG path used to decode to RGB and cost 3.63 ms for the decode plus
 * 5.11 ms to convert it back to the encoder's I420; it now decodes into
 * the JPEG's own planes for 2.97 ms and the conversion is 1.03 ms.
 *
 * MJPEG costs that decode but moves a fraction of the bytes over USB,
 * which matters on a host whose USB3 is shared -- a Raspberry Pi 4's is
 * shared with its Ethernet -- and, on the reference setup here, is the
 * format that does NOT come with the long-session freezes that raw YUYV
 * was suspected of. It is the default for that reason; YUYV remains one
 * click away, and the driver falls back to it automatically if MJPEG is
 * refused.
 *
 * Which one is used comes from CAPTURE_FORMAT in the .env. Buffers are
 * memory-mapped and recycled, so nothing is copied between the kernel
 * and this process.
 */

typedef enum {
    VIDEO_FORMAT_YUYV,  /* packed Y0 U0 Y1 V0, 2 bytes per pixel */
    VIDEO_FORMAT_MJPEG  /* compressed on the card, decoded here */
} VideoFormat;

/* What a delivered frame actually is.
 *
 * Distinct from VideoFormat, which is what was asked of the card. The
 * MJPEG path used to decode to RGB, and everything downstream then
 * converted that back to YUV -- a full colour-space round trip on two
 * million pixels, sixty times a second, to undo one libjpeg had just
 * done. It now hands over the JPEG's own planes untouched, so the
 * subsampling is whatever the card encoded (4:2:2 on the reference
 * card) and the pixel format has to be reported rather than assumed.
 *
 * Measured at 1080p60, per frame: 3.63 ms to decode to RGB plus 5.11 ms
 * to convert it to I420, against 2.97 ms and 1.03 ms for the planes.
 *
 * RGB24 remains for JPEGs whose subsampling is not one of the two that
 * can be handed over directly. */
typedef enum {
    VIDEO_PIXEL_YUYV422,  /* packed, straight from the card */
    VIDEO_PIXEL_YUV420P,  /* 3 planes, chroma halved both ways */
    VIDEO_PIXEL_YUV422P,  /* 3 planes, chroma halved horizontally */
    VIDEO_PIXEL_RGB24     /* 1 plane, the fallback */
} VideoPixelFormat;

/* One frame, however many planes it has. Pointers stay valid until the
 * next read or until close. */
typedef struct {
    const uint8_t *plane[3];
    int stride[3];
    VideoPixelFormat pixel;
} VideoFrame;

typedef struct VideoCapture VideoCapture;

/* Opens `device` (a /dev/video* node, ideally a stable /dev/v4l/by-id/
 * path). `format` is the preferred format; if the driver refuses it the
 * other one is tried, so an unusual card still works. The negotiated
 * size -- which the driver may adjust -- is written back through
 * width/height. Returns NULL and reports why on stderr on failure. */
VideoCapture *video_capture_open(const char *device, VideoFormat format, unsigned int *width,
                                 unsigned int *height);

void video_capture_close(VideoCapture *vc);

/* What frames actually come out as -- not necessarily what was asked
 * for, if the driver refused it. */
VideoFormat video_capture_format(const VideoCapture *vc);

/* File descriptor, for poll()/select() in the caller's own loop. */
int video_capture_fd(const VideoCapture *vc);

/* Fetches the next frame into `out`.
 *
 * For YUYV the single plane addresses the driver's mapped buffer
 * directly -- no copy at all -- so the frame must be consumed before
 * asking for another one.
 *
 *   1  a frame is available in *out
 *   0  nothing right now, or a corrupt frame was skipped -- these
 *      sticks emit one occasionally and dropping it beats aborting
 *  -1  fatal error on the device
 */
int video_capture_read(VideoCapture *vc, VideoFrame *out);

/* --- switching format while running ---------------------------------
 *
 * Which format the card delivers is a plausible cause of the freezes
 * seen in long sessions, and the only way to tell is to run for hours on
 * each. So it is switchable from the page rather than only through the
 * .env and a restart.
 *
 * The request is just a flag: the capture loop is the only thing allowed
 * to close and reopen the device, because it owns the mapped buffers and
 * is mid-frame the rest of the time. */
void video_capture_request_format(VideoFormat format);

/* Takes a pending request, if any. Returns 1 and writes it to `out`. */
int video_capture_take_format_request(VideoFormat *out);

/* The format the device is currently opened with -- reported to the page
 * so the toggle shows what is really running, not what was asked for. */
VideoFormat video_capture_active_format(void);

/* --- waiting for the picture to change ------------------------------
 *
 * Used after waking the console. The adapter has to re-handshake, but
 * only once the console is actually up: doing it as soon as the wake
 * script returns was too early -- the link came back while the console
 * was still ~10 s from showing anything, and it never re-attached.
 *
 * The card is a UVC device that generates its own "no signal" pattern,
 * so V4L2 reports the input as fine either way and there is nothing to
 * query. What CAN be seen is the picture changing. Rather than encode
 * what that pattern looks like, the frame present when the watch is
 * armed becomes the reference: whatever is on screen while the console
 * sleeps. A frame that differs from it means something woke up.
 */
void video_capture_watch_for_change(void);

/* 1 exactly once, on the first frame that differs from the reference.
 * Also returns 1 if the watch times out, so a wake that somehow changes
 * nothing on screen still gets its follow-up. */
int video_capture_take_change_detected(void);

/* "yuyv" / "mjpeg", for display and for the HTTP endpoint. */
const char *video_format_name(VideoFormat format);

/* Parses a CAPTURE_FORMAT value ("yuyv"/"mjpeg", case-insensitive).
 * Anything unrecognised falls back to YUYV with a warning. */
VideoFormat video_format_from_name(const char *name);

#endif
