#ifndef CAPTURE2CLOUD_SWITCH_STREAM_H
#define CAPTURE2CLOUD_SWITCH_STREAM_H

#include <stdint.h>

/*
 * The native transport, for clients that are not browsers.
 *
 * The browser talks WebRTC. The Switch homebrew cannot: ICE, DTLS-SRTP
 * and SCTP have no devkitPro portlib, and porting them would be weeks
 * before a single frame appeared. So native clients get a plain TCP
 * connection carrying the same encoded frames -- see c2s_protocol.h for
 * the wire format, and switch_homebrew/ARCHITECTURE.md for the reasoning.
 *
 * The browser path is untouched by any of this.
 */

#include "web_stream.h"

typedef struct SwitchStream SwitchStream;

/* Starts listening. `port` 0 uses C2S_DEFAULT_PORT. Returns NULL and
 * reports on stderr if the port cannot be bound. */
SwitchStream *switch_stream_start(WebStream *ws, uint16_t port);
void switch_stream_stop(SwitchStream *s);

/* Called when a frame had to be skipped for a client. Skipping breaks
 * VP8's prediction chain, so the picture stays corrupt until the next
 * keyframe -- up to five seconds at the usual interval. The host answers
 * by forcing one immediately instead. */
typedef void (*SwitchKeyframeRequest)(void *ctx);
void switch_stream_set_keyframe_request(SwitchStream *s, SwitchKeyframeRequest cb, void *ctx);

/* The size and rate the native stream is encoded at. Clients ask for
 * what they can actually decode: a console that cannot keep up at 720p60
 * produces exactly the symptom skipping causes. */
void switch_stream_set_profile_request(SwitchStream *s,
                                       void (*cb)(void *ctx, int codec, int w, int h, int fps,
                                                  int bitrate_kbps),
                                       void *ctx);

/* The client asking for a different video codec. */
/* How many clients are watching one codec (C2S_CODEC_VP8 / _H264).
 * Read per frame by the pipeline, so a chain nobody is on is not
 * encoded at all. */
int switch_stream_codec_client_count(SwitchStream *s, int codec);

/* Called whenever that count changes for either codec -- a client
 * arriving, leaving, or switching. Fired outside the client lock. */
void switch_stream_set_demand_changed(SwitchStream *s, void (*cb)(void *ctx), void *ctx);

/* Tells every connected client what the video stream is now. Sent after
 * a change, because the change takes effect some frames after the
 * request and a decoder re-initialised at the wrong moment sees the tail
 * of the old stream. */
void switch_stream_announce_stream(SwitchStream *s, uint8_t codec,
                                   uint16_t width, uint16_t height);

/* Announces the settings every native client shares -- the stream's
 * shape, its codec, its bitrate, and the capture format. Sent on change
 * and to each client as it connects; see SHARED_SETTINGS.md for what is
 * shared and what is deliberately not. */
void switch_stream_announce_shared(SwitchStream *s, uint16_t width, uint16_t height,
                                   uint16_t fps, uint16_t bitrate_kbps,
                                   uint8_t codec, uint8_t capture_mjpeg);

/* How many native clients are connected. The encoder branch for this
 * stream is only fed while this is above zero -- there is no point
 * encoding a second resolution for nobody. */
int switch_stream_client_count(SwitchStream *s);

/* How many could connect at once. A constant, but one the rest of the
 * program should not have to know the name of. */
int switch_stream_max_clients(void);

/* Hands an encoded frame to every connected client. Non-blocking: a
 * client that cannot keep up loses the frame rather than stalling the
 * pipeline. */
/* Sends one encoded frame to the clients on that codec, and only them:
 * the other group is watching a different encode, and bytes from the
 * wrong chain do not fail cleanly -- they decode into a picture. */
void switch_stream_send_video(SwitchStream *s, int codec, const uint8_t *data, uint32_t size,
                              int keyframe);
void switch_stream_send_audio(SwitchStream *s, const uint8_t *data, uint32_t size);

/* The size the stream is encoded at, announced in the handshake so the
 * client knows what to expect before the first frame. */
void switch_stream_set_video_size(SwitchStream *s, uint16_t width, uint16_t height);

#endif
