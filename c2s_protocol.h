#ifndef CAPTURE2CLOUD_C2S_PROTOCOL_H
#define CAPTURE2CLOUD_C2S_PROTOCOL_H

#include <stdint.h>

/*
 * The wire protocol between capture2cloud and a native client -- today,
 * the Switch homebrew in switch_homebrew/.
 *
 * ONE definition, included by both sides. The Switch Makefile adds the
 * parent directory to its include path for exactly that reason: a
 * protocol described in two places is a protocol that drifts, and the
 * failure mode is a stream that connects and then makes no sense.
 *
 * Why a second transport at all, when the browser has WebRTC: WebRTC
 * needs ICE, DTLS-SRTP and SCTP, none of which exist as a devkitPro
 * portlib. See switch_homebrew/ARCHITECTURE.md.
 *
 * Everything is little-endian, which both ends are natively (x86-64 and
 * aarch64 as configured here), so nothing is byte-swapped.
 */

#define C2S_MAGIC       0x57533243u /* "C2SW" little-endian */
#define C2S_VERSION     1
#define C2S_DEFAULT_PORT 5081

/* Sizes are u32 and the sender never exceeds this, so a receiver can
 * reject a malformed length instead of trying to allocate it. A 720p
 * VP8 keyframe is far below this; the margin is for a scene change on a
 * badly-behaved encoder. */
#define C2S_MAX_PAYLOAD (4u * 1024u * 1024u)

/* Session tokens are 32 random bytes hex-encoded; the field is a u8 so
 * nothing longer can even be announced. */
#define C2S_MAX_TOKEN_LEN 64

/* --- client -> server, once, immediately after connecting ---------- */

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* C2S_MAGIC */
    uint8_t  version;      /* C2S_VERSION */
    uint8_t  token_len;    /* 0 when no password is configured host-side */
    uint16_t reserved;
    /* followed by token_len bytes of the session token */
} C2sHello;

/* --- server -> client, once, in reply ------------------------------ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  accepted;     /* 0 = refused; the connection then closes */
    uint8_t  may_control;  /* 0 = picture and sound only, input dropped */
    uint8_t  reserved;
    uint16_t width;
    uint16_t height;
    uint8_t  video_codec;  /* C2sCodec */
    uint8_t  audio_codec;  /* C2sCodec */
    uint16_t audio_rate;
    uint8_t  audio_channels;
    uint8_t  reserved2[3];
} C2sHelloAck;

typedef enum {
    C2S_CODEC_VP8  = 1,
    C2S_CODEC_OPUS = 2,
    C2S_CODEC_H264 = 3
} C2sCodec;

/* --- framing, both directions -------------------------------------- */

typedef enum {
    C2S_MSG_VIDEO = 1,  /* one encoded video frame */
    C2S_MSG_AUDIO = 2,  /* one encoded audio packet */
    C2S_MSG_INPUT = 16, /* C2S_PAD_SLOTS bytes, client -> server */
    C2S_MSG_PING  = 17, /* empty; keeps a silent connection alive */
    C2S_MSG_HOME  = 18, /* empty; ask the host to send HOME to the console */
    C2S_MSG_PROFILE = 19, /* C2sProfile; what this client can decode */
    C2S_MSG_CODEC   = 20, /* one byte, a C2sCodec: which video codec to send */
    /* server -> client: C2sStreamInfo, sent whenever the video stream's
     * shape changes. The client cannot re-initialise its decoder on a
     * request it made, because the change takes effect some frames
     * later; this says exactly when, and for what. */
    C2S_MSG_STREAM_INFO = 21,
    /* client -> server, all empty:
     *
     * KEYFRAME asks for one now. The client sends it after dropping a
     * backlog: a predictive codec resumed mid-stream shows garbage until
     * the next one, and waiting out the interval is seconds of it.
     *
     * WAKE and RESET_DONGLE are the two buttons the browser has that
     * this transport did not. There is no browser on the console, and
     * needing one to wake the console the client exists to show is a
     * poor joke. Both are players-only, as they are on the page. */
    C2S_MSG_KEYFRAME     = 22,
    C2S_MSG_WAKE         = 23,
    C2S_MSG_RESET_DONGLE = 24
} C2sMsgType;

#define C2S_FLAG_KEYFRAME 0x01

typedef struct __attribute__((packed)) {
    uint8_t  type;   /* C2sMsgType */
    uint8_t  flags;  /* C2S_FLAG_* */
    uint16_t reserved;
    uint32_t size;   /* payload bytes that follow; <= C2S_MAX_PAYLOAD */
} C2sFrameHeader;

/* What a native client asks the host to encode for it.
 *
 * Software VP8 decoding on this hardware does not reach 720p60, and a
 * client that cannot keep up has to skip frames -- which on a predictive
 * codec is far worse than simply receiving fewer. Asking for something
 * decodable is the fix; skipping is the symptom. */
typedef struct __attribute__((packed)) {
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t bitrate_kbps; /* 0 = leave it as the host has it */
} C2sProfile;

/* What the video stream is now. Sent by the host after any change, and
 * the client re-initialises its decoder on it. */
typedef struct __attribute__((packed)) {
    uint16_t width;
    uint16_t height;
    uint8_t  video_codec; /* C2sCodec */
    uint8_t  reserved[3];
} C2sStreamInfo;

/*
 * The controller state carried by C2S_MSG_INPUT.
 *
 * These are GAMEPAD_XB360_* from gamepad_bridge.h, same order, same
 * -100..100 range -- the identical array the browser sends over its data
 * channel. Keeping one representation means the host applies native and
 * browser input through the same code path, so a button behaves the same
 * whichever client pressed it.
 */
#define C2S_PAD_SLOTS 21

/* The structs above are packed and read byte-for-byte off a socket by
 * two independently built programs. A field silently changing size on
 * one side would produce a stream that connects and then makes no sense,
 * so the sizes are pinned here: a mismatch fails the build instead. */
_Static_assert(sizeof(C2sHello) == 8, "C2sHello must stay 8 bytes on the wire");
_Static_assert(sizeof(C2sHelloAck) == 20, "C2sHelloAck must stay 20 bytes on the wire");
_Static_assert(sizeof(C2sStreamInfo) == 8, "C2sStreamInfo must stay 8 bytes on the wire");
_Static_assert(sizeof(C2sProfile) == 8, "C2sProfile must stay 8 bytes on the wire");
_Static_assert(sizeof(C2sFrameHeader) == 8, "C2sFrameHeader must stay 8 bytes on the wire");

#endif
