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
                                       void (*cb)(void *ctx, int w, int h, int fps, int bitrate_kbps),
                                       void *ctx);

/* The client asking for a different video codec. */
void switch_stream_set_codec_request(SwitchStream *s, void (*cb)(void *ctx, int codec), void *ctx);

/* Tells every connected client what the video stream is now. Sent after
 * a change, because the change takes effect some frames after the
 * request and a decoder re-initialised at the wrong moment sees the tail
 * of the old stream. */
void switch_stream_announce_stream(SwitchStream *s, uint16_t width, uint16_t height, uint8_t codec);

/* How many native clients are connected. The encoder branch for this
 * stream is only fed while this is above zero -- there is no point
 * encoding a second resolution for nobody. */
int switch_stream_client_count(SwitchStream *s);

/* Hands an encoded frame to every connected client. Non-blocking: a
 * client that cannot keep up loses the frame rather than stalling the
 * pipeline. */
void switch_stream_send_video(SwitchStream *s, const uint8_t *data, uint32_t size, int keyframe);
void switch_stream_send_audio(SwitchStream *s, const uint8_t *data, uint32_t size);

/* The size the stream is encoded at, announced in the handshake so the
 * client knows what to expect before the first frame. */
void switch_stream_set_video_size(SwitchStream *s, uint16_t width, uint16_t height);

#endif
