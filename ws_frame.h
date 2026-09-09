#ifndef CAPTURE2CLOUD_WS_FRAME_H
#define CAPTURE2CLOUD_WS_FRAME_H

#include <stddef.h>
#include <stdint.h>

/*
 * A WebSocket, so a browser can read the same bytes the phone and the
 * console read.
 *
 * The native clients already speak c2s_protocol.h over TCP. A browser
 * cannot open a TCP socket, and that -- not the protocol -- is the only
 * reason the page has its own transport at all. A WebSocket removes the
 * difference: above the handshake, ONE binary frame carries EXACTLY one
 * C2S message, header and payload, in the same order and layout the TCP
 * framing uses.
 *
 * What that buys is not elegance. The media in the WebRTC path travels
 * peer-to-peer over UDP and never touches the HTTP chain, which is why
 * it does not survive a reverse proxy: see the "remote access" note in
 * WORKINPROGRESS.md. A WebSocket over HTTPS is ordinary web traffic that
 * Cloudflare, Nginx and Authelia relay without being told anything.
 *
 * The cost is TCP's, and it is real: a lost packet stalls everything
 * behind it instead of costing one frame. On a LAN that is invisible.
 * Over a lossy link it trades artefacts for pauses, which is the wrong
 * way round for something being played on -- hence a choice, not a
 * replacement.
 */

/* Longest control frame payload the spec allows, and all we ever send. */
#define WS_MAX_CONTROL 125

/*
 * Answers the upgrade. `key` is the Sec-WebSocket-Key header's value.
 * Writes the whole 101 response to `out` and returns its length, or 0 if
 * the key is unusable.
 */
size_t ws_accept_response(const char *key, char *out, size_t out_size);

/*
 * Writes the frame header for a binary message of `len` bytes into
 * `out` (10 bytes are always enough) and returns its length.
 *
 * Server-to-client frames are never masked, which is what the spec says
 * and also what makes this two lines rather than a loop.
 */
size_t ws_binary_header(size_t len, uint8_t out[10]);

/* Opcodes, as far as this needs them. */
#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xa

/*
 * Takes one frame out of a receive buffer.
 *
 * Returns the number of bytes consumed and fills in the opcode and the
 * payload (unmasked in place, inside `buf`). Returns 0 when the frame is
 * not complete yet -- the caller reads more and asks again -- and -1 on
 * a frame that cannot be honoured, which is a protocol error and should
 * close the connection.
 *
 * Client-to-server frames MUST be masked; one that is not is rejected
 * rather than accepted leniently, because accepting it would mean
 * decoding whatever a non-browser decided to send here.
 */
long ws_take_frame(uint8_t *buf, size_t len, uint8_t *opcode,
                   uint8_t **payload, size_t *payload_len);

#endif /* CAPTURE2CLOUD_WS_FRAME_H */
