#ifndef GST_WEBRTC_STREAM_H
#define GST_WEBRTC_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/*
 * Low-latency broadcast (targeting "cloud gaming", ~20-60ms on LAN) to the
 * browser via WebRTC, built on GStreamer:
 *
 *   appsrc(video I420) -> vah264enc (hardware, zero-latency) -> h264parse
 *   -> rtph264pay -> tee -> [queue -> webrtcbin] per connected client
 *
 *   appsrc(audio S16LE) -> opusenc -> rtpopuspay -> tee -> [queue ->
 *   webrtcbin] per connected client
 *
 * A single encode is shared across all connected clients (no per-viewer
 * re-encoding). Each browser gets its own webrtcbin (its own
 * peer-to-peer connection).
 */

typedef struct GstWebrtcStream GstWebrtcStream;

GstWebrtcStream *gst_webrtc_stream_create(int width, int height, int audio_rate, int audio_channels);
void gst_webrtc_stream_destroy(GstWebrtcStream *g);
void gst_webrtc_stream_set_video_bitrate(GstWebrtcStream *g, int bitrate_kbps);
/* What that bitrate currently is. Read by /shared, so a page that did
 * not set it still shows what everyone is actually getting. */
int gst_webrtc_stream_get_video_bitrate(GstWebrtcStream *g);

/* One capture frame for the browser stream, in however many planes it
 * arrived in: packed YUYV straight off the card, the JPEG's own planes,
 * or RGB24 in the fallback case. `av_pixel_format` says which.
 *
 * Converted to I420 (swscale) on the way into the encoder. What that
 * costs depends entirely on what comes in: 1.03 ms a frame at 1080p from
 * planar YUV against 5.11 ms from RGB, because one is a chroma
 * subsample and the other is a colour-space conversion of every pixel.
 * No effect if no client is connected. */
void gst_webrtc_stream_push_video(GstWebrtcStream *g, const uint8_t *const plane[3],
                                  const int stride[3], int av_pixel_format, int width, int height);

/* To be called from the audio thread with the post-DSP samples,
 * interleaved S16LE, at the rate/format given at creation time. */
void gst_webrtc_stream_push_audio(GstWebrtcStream *g, const int16_t *pcm_interleaved, size_t frames);

/* Handles a new client's SDP offer (NUL-terminated string): creates its
 * webrtcbin, negotiates, waits for ICE gathering to complete, and returns
 * the full SDP answer (to be freed with free()), or NULL on failure.
 * Blocks the calling thread during negotiation (tens to hundreds of ms)
 * -- meant to run in the thread dedicated to that HTTP connection.
 *
 * `may_control` decides whether THIS client's gamepad DataChannel
 * messages are allowed to drive the console: 0 = viewer (video/audio
 * only, gamepad messages silently ignored), non-zero = player. This is
 * the real access control -- hiding the gamepad UI browser-side is only
 * cosmetic, since anyone can open devtools and write to the DataChannel
 * directly. Decided once here, at negotiation time, rather than per
 * message, so the input hot path stays exactly as fast as it was. */
/* `peer` is the address the SDP offer arrived from -- the same host the
 * media will be sent to, which is what lets the RTP packet size be sized
 * to that particular path (see RTP_MTU in gst_webrtc.c). Pass NULL to
 * fall back to the configured default. */
/* `out_id` receives the identity this client is known by; the page
 * sends it back to say it is still there, and to say goodbye. */
char *gst_webrtc_stream_handle_offer(GstWebrtcStream *g, const char *offer_sdp, int may_control,
                                     const struct sockaddr *peer, socklen_t peer_len,
                                     char *out_id, size_t out_id_size);

/* "Still here" / "leaving", from the page holding that id.
 *
 * A browser that vanishes leaves webrtcbin reading CONNECTED for as
 * long as this program runs -- never DISCONNECTED, never FAILED -- so
 * this is the only thing that actually notices it is gone. */
int gst_webrtc_stream_client_seen(GstWebrtcStream *g, const char *id);
int gst_webrtc_stream_client_bye(GstWebrtcStream *g, const char *id);

/* Number of clients currently connected; `max_clients` (may be NULL)
 * receives the hard limit. Slots are freed automatically when a browser
 * disconnects, so this reflects live viewers/players rather than a
 * running total. */
int gst_webrtc_stream_get_client_count(GstWebrtcStream *g, int *max_clients);

/* --- browser stream resolution --------------------------------------
 *
 * Changed while running, without restarting anything: the capture stays
 * at whatever the card delivers and the scale happens on the way to the
 * encoder. Below 1080p that also saves the encoder most of its work,
 * which is the point on a busy scene. */
void gst_webrtc_stream_set_browser_resolution(GstWebrtcStream *g, int width, int height);

/* The capture card's format, which is shared by everyone: the browsers
 * poll for it, and the native clients are told. Mirrored here only so
 * the native announcement can carry it. */
void gst_webrtc_stream_set_capture_mjpeg(GstWebrtcStream *g, int mjpeg);
void gst_webrtc_stream_get_browser_resolution(GstWebrtcStream *g, int *width, int *height);

/* --- the native (Switch) branch --------------------------------------
 *
 * A second, smaller encode for clients that are not browsers. It is fed
 * only while one is connected, so it costs nothing the rest of the time.
 */
typedef struct SwitchStream SwitchStream;
void gst_webrtc_stream_set_switch_output(GstWebrtcStream *g, SwitchStream *out);

/* The capture frame, scaled and encoded for the native client. `format`
 * is the AVPixelFormat the capture is delivering. */
void gst_webrtc_stream_push_video_switch(GstWebrtcStream *g, const uint8_t *const plane[3],
                                          const int stride[3], int av_pixel_format,
                                          int width, int height);

#endif
