#ifndef CAPTURE2SWITCH_NET_H
#define CAPTURE2SWITCH_NET_H

#include <stddef.h>
#include <stdint.h>

#include "c2s_protocol.h"
#include "input.h"

/*
 * The connection to the capture2cloud host.
 *
 * Deliberately blocking-free: every call returns immediately, because
 * this runs from the frame loop and a stall here would freeze the
 * picture. Connecting happens in the background and is retried on its
 * own, so an unreachable host costs a status line rather than a hang.
 */

typedef enum {
    NET_IDLE,        /* not asked to connect yet */
    NET_CONNECTING,  /* socket open, waiting for the handshake */
    NET_CONNECTED,   /* streaming */
    NET_FAILED       /* will retry; net_status() says why */
} NetState;

typedef struct {
    NetState state;
    int      may_control;   /* 0 when the host accepted us as a viewer */
    uint16_t width, height;
    uint8_t  video_codec;   /* what the host is encoding right now */
    uint16_t audio_rate;
    uint8_t  audio_channels;
    char     status[96];    /* human-readable, for the menu */

    /* Diagnostics. There is no shell on this console, so when it will
     * not connect these are the only way to tell a wrong address from a
     * refused port from a host that answers and then says nothing. */
    char     last_step[48]; /* how far it got: "connect in flight", ... */
    int      last_errno;    /* 0 when the failure was not a syscall */
    uint64_t rx_bytes, tx_bytes;
    unsigned attempts;
} NetInfo;

/* `token` may be NULL or empty when the host has no password set. */
int  net_init(void);
void net_exit(void);
void net_connect(const char *host, uint16_t port, const char *token);
void net_disconnect(void);

/* Drives the connection. Call once per frame. */
void net_poll(void);

const NetInfo *net_info(void);

/* Hands over the next complete media frame, if one has arrived.
 * `*payload` points into an internal buffer valid until the next call.
 * Returns the C2sMsgType, or 0 when nothing is ready. */
int net_take_frame(const uint8_t **payload, uint32_t *size, uint8_t *flags);

/* Queues the controller state. Dropped silently when not connected --
 * the caller has nothing useful to do about it. */
void net_send_input(const PadState21 pad);

/* Asks the host to press HOME on the remote console. */
void net_send_home(void);

/* Asks for a keyframe now. Sent after dropping a backlog: a predictive
 * codec resumed mid-stream shows garbage until the next one, and waiting
 * out the interval is seconds of it. The host rate-limits these, so
 * asking often is not a way to make things worse. */
void net_send_keyframe_request(void);

/* The two buttons the browser has and this did not. Players only, and
 * refused server-side for anyone else -- WAKE switches mains power to a
 * console, and RESET_DONGLE re-enumerates the adapter every player
 * shares. */
void net_send_wake(void);
void net_send_reset_dongle(void);

/* Stops and starts the host program. Players only, and refused
 * server-side for anyone else: it takes the stream away from everyone
 * watching. The connection drops and comes back on its own -- the
 * retry loop is already there for a host that was switched off. */
void net_send_restart(void);

/* Asks the host to encode with a different codec (C2S_CODEC_VP8 or
 * C2S_CODEC_H264). The change takes effect some frames later and is
 * announced back with C2S_MSG_STREAM_INFO, which is when the decoder is
 * re-initialised -- doing it on the request would catch the tail of the
 * old stream. */
void net_send_codec(int codec);

/* Tells the host what this client can actually decode. Software VP8 on
 * this hardware does not reach 720p60; a client that cannot keep up has
 * to skip frames, and skipping a predictive codec is far worse than
 * receiving fewer. */
void net_send_profile(int width, int height, int fps, int bitrate_kbps);

/* Exchanges the host password for a session token over HTTP, the same
 * /login the browser uses. Returns 1 on success and fills `token`.
 * Blocking, and called from the menu rather than the frame loop. */
int net_login(const char *host, uint16_t port, const char *password,
              char *token, size_t token_size, char *error, size_t error_size);

#endif
