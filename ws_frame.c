#include "ws_frame.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

/* The constant every WebSocket handshake concatenates the client's key
 * with before hashing. It is in RFC 6455 and it is not a secret; it
 * exists so a server cannot be tricked into completing a handshake it
 * did not understand. */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

size_t ws_accept_response(const char *key, char *out, size_t out_size) {
    if (!key || !*key || !out) {
        return 0;
    }
    /* A key is 24 base64 characters; anything else is not a browser
     * doing a handshake. */
    const size_t key_len = strlen(key);
    if (key_len == 0 || key_len > 128) {
        return 0;
    }

    char joined[160];
    const int n = snprintf(joined, sizeof(joined), "%s%s", key, WS_GUID);
    if (n <= 0 || (size_t)n >= sizeof(joined)) {
        return 0;
    }

    /* GLib rather than a hand-rolled SHA-1: it is already linked, and a
     * hash written for one use is a hash nobody reviews. */
    gsize digest_len = 20;
    guint8 digest[20];
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
    if (!sum) {
        return 0;
    }
    g_checksum_update(sum, (const guchar *)joined, n);
    g_checksum_get_digest(sum, digest, &digest_len);
    g_checksum_free(sum);

    gchar *accept = g_base64_encode(digest, digest_len);
    if (!accept) {
        return 0;
    }
    const int written = snprintf(out, out_size,
                                 "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: %s\r\n\r\n",
                                 accept);
    g_free(accept);
    if (written <= 0 || (size_t)written >= out_size) {
        return 0;
    }
    return (size_t)written;
}

size_t ws_binary_header(size_t len, uint8_t out[10]) {
    out[0] = 0x80 | WS_OP_BINARY; /* FIN, and one frame per message */
    if (len < 126) {
        out[1] = (uint8_t)len;
        return 2;
    }
    if (len <= 0xffff) {
        out[1] = 126;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)len;
        return 4;
    }
    out[1] = 127;
    for (int i = 0; i < 8; i++) {
        out[2 + i] = (uint8_t)(len >> (56 - 8 * i));
    }
    return 10;
}

long ws_take_frame(uint8_t *buf, size_t len, uint8_t *opcode,
                   uint8_t **payload, size_t *payload_len) {
    if (len < 2) {
        return 0;
    }
    const uint8_t op = buf[0] & 0x0f;
    const int masked = (buf[1] & 0x80) != 0;
    uint64_t plen = buf[1] & 0x7f;
    size_t at = 2;

    if (plen == 126) {
        if (len < at + 2) return 0;
        plen = ((uint64_t)buf[at] << 8) | buf[at + 1];
        at += 2;
    } else if (plen == 127) {
        if (len < at + 8) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | buf[at + i];
        }
        at += 8;
    }

    /* A browser masks; anything that does not is not one, and decoding
     * it anyway would mean trusting whatever sent it. */
    if (!masked) {
        return -1;
    }
    /* Bounded before it is believed: a length is a promise from the
     * other end, and this one arrives before the bytes do. */
    if (plen > (uint64_t)16 * 1024 * 1024) {
        return -1;
    }
    if (len < at + 4) return 0;
    const uint8_t *mask = buf + at;
    at += 4;
    if (len < at + plen) return 0;

    uint8_t *data = buf + at;
    for (uint64_t i = 0; i < plen; i++) {
        data[i] ^= mask[i & 3];
    }

    if (opcode) *opcode = op;
    if (payload) *payload = data;
    if (payload_len) *payload_len = (size_t)plen;
    return (long)(at + plen);
}
