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

/*
 * And a second port, for a Wii U GamePad only.
 *
 * Not a tidiness choice. The console, the phone and the browsers share
 * eight client slots, and a pad that reconnects in a loop -- which is
 * what a pad does while its radio is being brought up -- would take
 * them from clients that have nowhere else to go. That exact failure
 * has already happened once here, between the browsers and the console,
 * and the answer then was to stop them sharing a pool.
 *
 * It also settles the codec without asking: arriving on this port IS
 * the request, so there is no round trip where the client is briefly on
 * a stream it cannot decode.
 */
#define C2S_DRC_PORT 5082

/*
 * And a third, for a client running ON a Wii U console.
 *
 * Same reasoning as the port above, plus one that is specific to it:
 * `C2sShared` makes the size, the frame rate and the bitrate belong to
 * every client on a port at once. On 5081 a handheld asking for 480p30
 * took the GamePad down to 30 with it -- measured, and the reason 5082
 * exists. A console on a television wants 720p60 and a phone on a train
 * wants neither, so they do not share a port.
 *
 * ../SHARED_SETTINGS.md has the rule; wiiu_console/SPEC.md has why this
 * client in particular could not live with it.
 */
#define C2S_WIIU_PORT 5083

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
    C2S_CODEC_H264 = 3,
    /*
     * H.264 as a Wii U GamePad can decode it, which ordinary H.264 is
     * not: DRH slicing, macroblock rows instead of NAL units, and no
     * slice header at all. Only the drc-x264 fork produces it.
     *
     * A payload is one C2sDrcFrame followed by its five chunks, because
     * the chunk boundaries are the message -- the pad's packetiser
     * wants exactly five and cannot find them by scanning for start
     * codes: there are none.
     *
     * A host without the fork refuses this codec rather than sending
     * something that looks close, and the client falls back to decoding
     * ordinary H.264 and encoding it again itself.
     */
    C2S_CODEC_DRC_H264 = 4
} C2sCodec;

/* Fixed by the pad's protocol, not chosen: the panel libdrc feeds is
 * 864x480 and the slicing is always five chunks. */
#define C2S_DRC_WIDTH   864
#define C2S_DRC_HEIGHT  480
#define C2S_DRC_CHUNKS  5

/* The header on a C2S_CODEC_DRC_H264 video payload. The five chunks
 * follow it, in order, packed. */
typedef struct __attribute__((packed)) {
    uint8_t  chunks;          /* always C2S_DRC_CHUNKS; a guard, not a choice */
    uint8_t  reserved[3];
    uint32_t size[C2S_DRC_CHUNKS];
} C2sDrcFrame;

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
    C2S_MSG_RESET_DONGLE = 24,
    /* Restarts the host program in place. Players only, like the two
     * above: it takes the stream away from everyone watching. */
    C2S_MSG_RESTART      = 25,
    /* server -> client: C2sShared, the settings that are not this
     * client's alone to keep.
     *
     * One encoder feeds every native client, so the resolution, the
     * frame rate, the bitrate and the codec belong to the connection as
     * a whole rather than to whoever asked last. A client that arrives
     * with its own saved values and pushes them changes the picture for
     * everyone already watching, which is a strange thing to do to
     * someone -- so it is told what the stream is instead, and moves its
     * own controls to match.
     *
     * Sent once after the handshake and again on every change. Distinct
     * from STREAM_INFO, which says "re-initialise your decoder now" and
     * must stay exactly that: this one moves sliders and changes no
     * pictures. */
    C2S_MSG_SHARED       = 26,
    /*
     * server -> client: C2sHelloAck, as a message.
     *
     * On this transport's own port the ack is the bare struct, written
     * before any framing exists -- it is the reply to the hello and
     * there is nothing yet to frame it with. A browser's handshake is
     * the WebSocket upgrade instead, which is over before this
     * transport hears about the connection, so the ack has to arrive
     * afterwards like everything else does: as a message, with a header
     * in front of it, so the page's reader has one shape to handle
     * rather than a special first frame.
     */
    C2S_MSG_HELLO_ACK    = 27
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

/* The settings one native client cannot change without changing them
 * for all of them. Everything absent from this struct -- volume, the
 * picture adjustments, the virtual pad, the stick shaping -- is the
 * client's own business and is never sent anywhere.
 *
 * capture_mjpeg is the odd one out: it belongs to the capture card, so
 * it is shared with the browsers too and not merely between native
 * clients. */
typedef struct __attribute__((packed)) {
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t bitrate_kbps;
    uint8_t  video_codec;   /* C2sCodec */
    uint8_t  capture_mjpeg; /* 1 = MJPEG from the card, 0 = raw YUYV */
    uint8_t  reserved[2];
} C2sShared;

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
 * so the sizes are pinned here: a mismatch fails the build instead.
 *
 * Spelled through a macro because one of those programs is C++ now --
 * the Wii U GamePad client, which has to be, because libdrc is. The
 * keyword is the only thing that differs between the two languages
 * here; everything else in this file is types and macros that both
 * read the same way. */
#ifdef __cplusplus
#define C2S_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define C2S_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

C2S_STATIC_ASSERT(sizeof(C2sHello) == 8, "C2sHello must stay 8 bytes on the wire");
C2S_STATIC_ASSERT(sizeof(C2sHelloAck) == 20, "C2sHelloAck must stay 20 bytes on the wire");
C2S_STATIC_ASSERT(sizeof(C2sStreamInfo) == 8, "C2sStreamInfo must stay 8 bytes on the wire");
C2S_STATIC_ASSERT(sizeof(C2sProfile) == 8, "C2sProfile must stay 8 bytes on the wire");
C2S_STATIC_ASSERT(sizeof(C2sShared) == 12, "C2sShared must stay 12 bytes on the wire");
C2S_STATIC_ASSERT(sizeof(C2sFrameHeader) == 8, "C2sFrameHeader must stay 8 bytes on the wire");

#endif
