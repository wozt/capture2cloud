#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <coreinit/mutex.h>
#include <coreinit/time.h>

/*
 * Ported from ../switch_homebrew/src/net.c, which is the same protocol
 * against the same host. Only three things are platform: the clock, the
 * mutex, and the fact that this console needs its network brought up by
 * hand before a socket will do anything (see net_init).
 *
 * Kept as a port rather than shared source because the two consoles'
 * system headers disagree about too much to paper over, and a file that
 * built for both would be mostly #ifdef.
 */

/* Reassembly starts small and grows to whatever the stream turns out to
 * need. Allocating C2S_MAX_PAYLOAD up front meant a 4 MB block at
 * startup, on a console whose homebrew heap is not generous -- and for a
 * 720p stream whose frames are a fraction of that. */
#define RX_INITIAL (256u * 1024u)

/* A silent connection still has to prove it is alive; the host drops a
 * client that says nothing. */
#define PING_INTERVAL_MS 2000

/* How long before a failed connection is tried again. */
#define RETRY_INTERVAL_MS 3000

/* How long a connect may take before it is called failed. Generous for a
 * LAN, short enough that a wrong address is obvious rather than looking
 * like a hang. */
#define CONNECT_TIMEOUT_MS 4000

/* The same idea for the one blocking exchange in this file, the login.
 * Longer, because it is a password check the user is watching for. */
#define LOGIN_TIMEOUT_MS 5000

typedef enum {
    LINK_NONE,        /* no socket */
    LINK_CONNECTING,  /* connect() in flight, waiting for writability */
    LINK_HANDSHAKE,   /* hello sent, waiting for the reply */
    LINK_UP
} LinkStage;

static int g_sock = -1;
static LinkStage g_stage = LINK_NONE;
static NetInfo g_info;

static char g_host[64];
static uint16_t g_port;
static char g_token[80];
static int g_want_connection = 0;
static uint64_t g_next_retry_ms = 0;
static uint64_t g_last_ping_ms = 0;
static uint64_t g_connect_started_ms = 0;

static uint8_t *g_rx = NULL;

/*
 * Valid bytes live in:
 *
 *   g_rx[g_rx_off .. g_rx_off + g_rx_len)
 *
 * Old code memmove()'d the entire remainder of this buffer after EVERY
 * C2S packet. With hundreds of Opus packets per second that could move
 * megabytes repeatedly for no useful reason.
 *
 * Now consuming a packet only advances g_rx_off. The buffer is compacted
 * only when the free tail is actually exhausted.
 */
static uint32_t g_rx_off = 0;
static uint32_t g_rx_len = 0;
static uint32_t g_rx_cap = 0;

static uint64_t now_ms(void) {
    return (uint64_t)OSTicksToMilliseconds(OSGetSystemTime());
}

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_info.status, sizeof(g_info.status), fmt, ap);
    va_end(ap);
    printf("net: %s\n", g_info.status);
}

/* What the status line cannot fit but a diagnosis needs. This runs on a
 * console with no shell: when it will not connect, these are the only
 * way to tell a wrong address from a refused port from a host that
 * answers and then says nothing. */
static void note_step(const char *step, int err) {
    snprintf(g_info.last_step, sizeof(g_info.last_step), "%s", step);
    g_info.last_errno = err;
}

/* The send side is now shared: the controller thread writes to the same
 * socket as the frame loop, which sends profile and codec changes. One
 * message half-written by one thread and finished by the other would put
 * the host's framing out of step permanently, so a message is written
 * whole or not at all. The receive side is untouched -- only the frame
 * loop reads. */
static OSMutex g_tx_lock;
static uint32_t g_local_ip;
static int g_tx_lock_ready = 0;

int net_init(void) {
    memset(&g_info, 0, sizeof(g_info));
    g_info.state = NET_IDLE;
    set_status("not connected");
    note_step("socket layer ready", 0);

    /*
     * WUT has already initialised the Wii U socket layer before main().
     * It also starts the normal system network asynchronously.
     *
     * Capture2Cloud therefore does not query nn::ac and does not select
     * an interface. It simply opens an ordinary socket:
     *
     *   native stack                     -> Wi-Fi
     *   transparent socket shim present -> whatever that shim routes
     *
     * The actual local address is obtained with getsockname() once the
     * socket connects.
     */
    g_local_ip = 0;
    g_rx_off = 0;
    g_rx_len = 0;

    if (!g_rx) {
        g_rx = malloc(RX_INITIAL);
        if (!g_rx) {
            set_status("out of memory");
            return -1;
        }
        g_rx_cap = RX_INITIAL;
    }

    if (!g_tx_lock_ready) {
        OSInitMutex(&g_tx_lock);
        g_tx_lock_ready = 1;
    }

    return 0;
}

/* The console's own address, for the status line. Worth showing: it is
 * the difference between "the host is unreachable" and "this console is
 * not on the network at all", and there is no shell here to ask. */
uint32_t net_local_ip(void) {
    return g_local_ip;
}

static void close_socket(void) {
    if (g_sock >= 0) {
        /*
         * Tell the peer explicitly that both halves are gone before
         * releasing the descriptor.
         *
         * With the AX shim this becomes lwip_shutdown(), so the host
         * does not have to wait for an idle timeout to notice that the
         * Wii U application disappeared.
         */
        shutdown(g_sock, SHUT_RDWR);
        close(g_sock);
        g_sock = -1;
    }
    g_rx_off = 0;
    g_rx_len = 0;
    g_stage = LINK_NONE;
}

void net_exit(void) {
    close_socket();

    free(g_rx);
    g_rx = NULL;
    g_rx_off = 0;
    g_rx_len = 0;
    g_rx_cap = 0;

    g_tx_lock_ready = 0;
    g_local_ip = 0;
}

void net_disconnect(void) {
    g_want_connection = 0;
    close_socket();
    g_info.state = NET_IDLE;
    set_status("not connected");
    note_step("disconnected by the user", 0);
}

void net_connect(const char *host, uint16_t port, const char *token) {
    snprintf(g_host, sizeof(g_host), "%s", host ? host : "");
    g_port = port;
    snprintf(g_token, sizeof(g_token), "%s", token ? token : "");
    g_want_connection = 1;
    g_next_retry_ms = 0;
    close_socket();
    g_info.state = NET_IDLE;
    /* Cleared, not carried over: until the new handshake answers, what
     * this connection is allowed to do is unknown, and the menu was
     * showing the previous connection's answer in the meantime. */
    g_info.may_control = 0;
    g_info.rx_bytes = g_info.tx_bytes = 0;
    g_info.attempts = 0;
    set_status("connecting to %s:%u", g_host, g_port);
}

const NetInfo *net_info(void) {
    return &g_info;
}

static void fail(const char *step, int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_info.status, sizeof(g_info.status), fmt, ap);
    va_end(ap);
    printf("net: %s (%s, errno %d: %s)\n", g_info.status, step, err,
           err ? strerror(err) : "-");
    note_step(step, err);
    close_socket();
    g_info.state = NET_FAILED;
    g_next_retry_ms = now_ms() + RETRY_INTERVAL_MS;
}

static int send_all(const void *data, size_t len) {
    const uint8_t *p = data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(g_sock, p + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            g_info.tx_bytes += (uint64_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Non-blocking and the buffer is full: the host is not
             * draining. Dropping this message beats stalling the frame
             * loop for it. */
            return -1;
        }
        return -1;
    }
    return 0;
}

static int send_message(uint8_t type, uint8_t flags, const void *payload, uint32_t size) {
    C2sFrameHeader h;
    h.type = type;
    h.flags = flags;
    h.reserved = c2s_le16(0);
    h.size = c2s_le32(size);

    if (g_tx_lock_ready) OSLockMutex(&g_tx_lock);
    int rc = 0;
    if (send_all(&h, sizeof(h)) != 0) {
        rc = -1;
    } else if (size && send_all(payload, size) != 0) {
        rc = -1;
    }
    if (g_tx_lock_ready) OSUnlockMutex(&g_tx_lock);
    return rc;
}

/* Opens the socket and starts the connect. Never blocks: a blocking
 * connect to a host that is switched off takes the console's TCP stack
 * over a minute to give up on, and the frame loop calls this -- the
 * whole interface would freeze for the duration. */
static void begin_connect(void) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);

    if (g_host[0] == '\0') {
        fail("no host configured", 0, "no host set (edit capture2switch.cfg)");
        return;
    }
    if (inet_pton(AF_INET, g_host, &addr.sin_addr) != 1) {
        fail("inet_pton", 0, "'%s' is not an IPv4 address", g_host);
        return;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        fail("socket", errno, "could not create a socket");
        return;
    }

    int one = 1;
    setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* A deliberately small receive buffer, set before connecting so it
     * fixes the window this end advertises.
     *
     * It is what keeps the host from running ahead. The host skips a
     * frame when its own socket is backed up, but on a fast link nothing
     * backs up: the frames simply pour into this end's buffer, and a
     * buffer sized in megabytes is seconds of video sitting between the
     * console and this screen. The picture then looks perfect and is
     * minutes behind, which is the one failure mode a controller cannot
     * live with. Refusing to take more than about a third of a second's
     * worth is what turns a slow link into dropped frames instead of
     * growing lag. */
    int rcvbuf = 60 * 1024;
    if (setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF,
                   &rcvbuf, sizeof(rcvbuf)) != 0) {
        printf("net: SO_RCVBUF %d refused, errno=%d\n", rcvbuf, errno);
    }

    if (fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL, 0) | O_NONBLOCK) != 0) {
        fail("fcntl O_NONBLOCK", errno, "could not set the socket non-blocking");
        return;
    }

    g_info.attempts++;
    int rc = connect(g_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK) {
        fail("connect", errno, "cannot reach %s:%u", g_host, g_port);
        return;
    }

    g_stage = LINK_CONNECTING;
    g_connect_started_ms = now_ms();
    g_info.state = NET_CONNECTING;
    set_status("connecting to %s:%u...", g_host, g_port);
    note_step("connect in flight", 0);
}

/* The socket becomes writable when the connect resolves -- successfully
 * or not. SO_ERROR is what says which. */
static void finish_connect(void) {
    struct pollfd pfd = {.fd = g_sock, .events = POLLOUT, .revents = 0};
    int r = poll(&pfd, 1, 0);
    if (r == 0) {
        if (now_ms() - g_connect_started_ms > CONNECT_TIMEOUT_MS) {
            fail("connect timed out", 0, "%s:%u did not answer", g_host, g_port);
        }
        return;
    }
    if (r < 0) {
        fail("poll", errno, "connection attempt failed");
        return;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(g_sock, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        fail("getsockopt SO_ERROR", errno, "connection attempt failed");
        return;
    }
    if (err != 0) {
        fail("connect refused", err,
             err == ECONNREFUSED ? "%s:%u refused it -- is capture2cloud running?"
                                 : "cannot reach %s:%u",
             g_host, g_port);
        return;
    }

    /*
     * Ask the socket itself which local address it is using.
     *
     * This deliberately contains no knowledge of Wi-Fi, USB Ethernet or
     * any shim. A native socket reports the native address; an
     * intercepted socket reports whatever address its socket
     * implementation exposes.
     */
    {
        struct sockaddr_in local;
        socklen_t local_len = sizeof(local);
        memset(&local, 0, sizeof(local));
        if (getsockname(g_sock, (struct sockaddr *)&local, &local_len) == 0 &&
            local.sin_family == AF_INET) {
            g_local_ip = ntohl(local.sin_addr.s_addr);
        }
    }

    uint8_t token_len = (uint8_t)strlen(g_token);
    C2sHello hello;
    hello.magic = c2s_le32(C2S_MAGIC);
    hello.version = C2S_VERSION;
    hello.token_len = token_len;

    /*
     * This console can consume the capture PCM directly.
     *
     * Older hosts simply see a non-zero reserved field and continue to
     * answer Opus; the reply's audio_codec tells us what was negotiated.
     */
    hello.reserved =
        c2s_le16(
            C2S_HELLO_CAP_PCM_S16LE);
    if (send_all(&hello, sizeof(hello)) != 0 ||
        (token_len && send_all(g_token, token_len) != 0)) {
        fail("sending hello", errno, "the host closed during the handshake");
        return;
    }

    g_stage = LINK_HANDSHAKE;
    g_connect_started_ms = now_ms();
    set_status("waiting for the host's reply...");
    note_step("hello sent", 0);
}

/* Grows the reassembly buffer, within what the protocol allows. */
static int fill_rx(void)
{
    for (;;) {
        uint32_t end =
            g_rx_off + g_rx_len;

        /*
         * No free tail left.
         *
         * First reclaim bytes already consumed. This is the ONLY normal
         * memmove in the receive path now.
         */
        if (end >= g_rx_cap) {
            if (g_rx_off > 0) {
                if (g_rx_len) {
                    memmove(
                        g_rx,
                        g_rx + g_rx_off,
                        g_rx_len);
                }

                g_rx_off = 0;
                end = g_rx_len;
            }

            /*
             * Still full: the current incomplete frame genuinely needs a
             * larger reassembly buffer.
             */
            if (end >= g_rx_cap) {
                uint32_t new_cap =
                    g_rx_cap
                        ? g_rx_cap * 2
                        : RX_INITIAL;

                if (new_cap >
                    C2S_MAX_PAYLOAD +
                    sizeof(C2sFrameHeader)) {

                    new_cap =
                        C2S_MAX_PAYLOAD +
                        sizeof(C2sFrameHeader);
                }

                if (new_cap <= g_rx_cap) {
                    return 0;
                }

                uint8_t *bigger =
                    realloc(g_rx, new_cap);

                if (!bigger) {
                    return 0;
                }

                g_rx = bigger;
                g_rx_cap = new_cap;
                end = g_rx_len;
            }
        }

        ssize_t n =
            recv(
                g_sock,
                g_rx + end,
                g_rx_cap - end,
                MSG_DONTWAIT);

        if (n > 0) {
            g_rx_len += (uint32_t)n;
            g_info.rx_bytes += (uint64_t)n;
            continue;
        }

        if (n == 0) {
            errno = 0;
            return -1;
        }

        if (errno == EAGAIN ||
            errno == EWOULDBLOCK) {
            return 0;
        }

        return -1;
    }
}

static void consume(uint32_t bytes)
{
    if (bytes > g_rx_len) {
        bytes = g_rx_len;
    }

    g_rx_off += bytes;
    g_rx_len -= bytes;

    /*
     * Canonical empty state.
     */
    if (g_rx_len == 0) {
        g_rx_off = 0;
    }
}

static void handle_handshake_reply(void) {
    if (g_rx_len < sizeof(C2sHelloAck)) {
        return;
    }
    C2sHelloAck ack;
    memcpy(
        &ack,
        g_rx + g_rx_off,
        sizeof(ack));

    consume(sizeof(ack));

    ack.magic = c2s_le32(ack.magic);
    ack.width = c2s_le16(ack.width);
    ack.height = c2s_le16(ack.height);
    ack.audio_rate = c2s_le16(ack.audio_rate);

    if (ack.magic != C2S_MAGIC) {
        fail("bad reply", 0, "whatever is on port %u is not capture2cloud", g_port);
        return;
    }
    if (ack.version != C2S_VERSION) {
        fail("version mismatch", 0, "host speaks version %u, this client %u",
             ack.version, C2S_VERSION);
        return;
    }
    if (!ack.accepted) {
        fail("refused", 0, "the host refused this client");
        return;
    }

    g_info.may_control = ack.may_control;
    g_info.width = ack.width;
    g_info.height = ack.height;
    g_info.video_codec = ack.video_codec;
    g_info.audio_codec = ack.audio_codec;
    g_info.audio_rate = ack.audio_rate;
    g_info.audio_channels = ack.audio_channels;
    g_info.state = NET_CONNECTED;
    g_stage = LINK_UP;
    g_last_ping_ms = now_ms();
    set_status("%ux%u, %s", ack.width, ack.height,
               ack.may_control ? "player" : "viewer (input ignored)");
    note_step("connected", 0);
}

void net_poll(void) {
    uint64_t t = now_ms();

    if (!g_want_connection) {
        return;
    }
    if (g_sock < 0) {
        if (t >= g_next_retry_ms) {
            begin_connect();
        }
        return;
    }
    if (g_stage == LINK_CONNECTING) {
        finish_connect();
        return;
    }

    if (fill_rx() != 0) {
        const int e = errno;
        if (e) {
            fail("recv", e, "receive failed");
        } else {
            fail("peer closed", 0, "the host closed the connection");
        }
        return;
    }

    if (g_stage == LINK_HANDSHAKE) {
        if (t - g_connect_started_ms > (uint64_t)CONNECT_TIMEOUT_MS * 2) {
            fail("handshake timed out", 0, "connected, but the host never replied");
            return;
        }
        handle_handshake_reply();
        return;
    }

    if (t - g_last_ping_ms >= PING_INTERVAL_MS) {
        g_last_ping_ms = t;
        send_message(C2S_MSG_PING, 0, NULL, 0);
    }
}

int net_take_frame(const uint8_t **payload,
                   uint32_t *size,
                   uint8_t *flags)
{
    if (g_stage != LINK_UP ||
        g_rx_len < sizeof(C2sFrameHeader)) {
        return 0;
    }

    uint8_t *base =
        g_rx + g_rx_off;

    C2sFrameHeader h;

    memcpy(
        &h,
        base,
        sizeof(h));

    h.size =
        c2s_le32(h.size);

    if (h.size > C2S_MAX_PAYLOAD) {
        fail(
            "bad frame size",
            0,
            "stream out of step, reconnecting");

        return 0;
    }

    const uint32_t total =
        (uint32_t)sizeof(h) +
        h.size;

    if (g_rx_len < total) {
        return 0;
    }

    /*
     * Zero-copy parser.
     *
     * The caller consumes payload before asking for the next message,
     * exactly matching net.h's "valid until the next call" contract.
     */
    *payload =
        base + sizeof(h);

    *size = h.size;
    *flags = h.flags;

    consume(total);

    return h.type;
}

void net_send_input(const PadState21 pad) {
    if (g_stage != LINK_UP || !g_info.may_control) {
        return;
    }
    send_message(C2S_MSG_INPUT, 0, pad, C2S_PAD_SLOTS);
}

void net_send_keyframe_request(void) {
    if (g_stage != LINK_UP) {
        return;
    }
    send_message(C2S_MSG_KEYFRAME, 0, NULL, 0);
}

void net_send_wake(void) {
    if (g_stage != LINK_UP || !g_info.may_control) {
        return;
    }
    send_message(C2S_MSG_WAKE, 0, NULL, 0);
}

void net_send_restart(void) {
    if (g_stage != LINK_UP || !g_info.may_control) {
        return;
    }
    send_message(C2S_MSG_RESTART, 0, NULL, 0);
}

void net_send_reset_dongle(void) {
    if (g_stage != LINK_UP || !g_info.may_control) {
        return;
    }
    send_message(C2S_MSG_RESET_DONGLE, 0, NULL, 0);
}

void net_send_profile(int width, int height, int fps, int bitrate_kbps) {
    if (g_stage != LINK_UP) {
        return;
    }
    C2sProfile p = {
        c2s_le16((uint16_t)width),
        c2s_le16((uint16_t)height),
        c2s_le16((uint16_t)fps),
        c2s_le16((uint16_t)bitrate_kbps)
    };
    send_message(C2S_MSG_PROFILE, 0, &p, sizeof(p));
}

/* A deliberately small HTTP client: one POST to /login, read the reply,
 * done. Pulling in a whole HTTP library for a single request that always
 * goes to the same place would be more code, not less.
 *
 * Blocking on purpose -- it is called from a menu action, where waiting
 * a moment is expected, not from the frame loop. */
int net_login(const char *host, uint16_t port, const char *password,
              char *token, size_t token_size, char *error, size_t error_size) {
    token[0] = '\0';
    error[0] = '\0';

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    /* The web server, not the native transport: /login lives there and
     * its port is the one the browser uses. */
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        snprintf(error, error_size, "'%s' is not an IPv4 address", host);
        return 0;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(error, error_size, "no socket");
        return 0;
    }
    /*
     * This console has no SO_RCVTIMEO or SO_SNDTIMEO -- the Switch port
     * used both. The deadline is kept, because without one a host that
     * accepts the connection and then says nothing hangs the console
     * with no shell to kill it from; it is enforced with poll() below
     * instead.
     */

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(error, error_size, "cannot reach %s:%u", host, port);
        close(fd);
        return 0;
    }

    char request[512];
    int n = snprintf(request, sizeof(request),
                     "POST /login HTTP/1.1\r\nHost: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     host, strlen(password), password);
    if (send(fd, request, (size_t)n, 0) != n) {
        snprintf(error, error_size, "could not send the password");
        close(fd);
        return 0;
    }

    char reply[1024];
    size_t got = 0;
    const uint64_t deadline = now_ms() + LOGIN_TIMEOUT_MS;
    while (got < sizeof(reply) - 1) {
        const uint64_t now = now_ms();
        if (now >= deadline) {
            break;   /* whatever arrived is judged below; silence reads
                      * as a refusal, which is the safe way round. */
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, (int)(deadline - now)) <= 0) {
            break;
        }
        ssize_t r = recv(fd, reply + got, sizeof(reply) - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    reply[got] = '\0';

    if (strncmp(reply, "HTTP/1.1 200", 12) != 0) {
        if (strncmp(reply, "HTTP/1.1 401", 12) == 0) {
            snprintf(error, error_size, "wrong password");
        } else if (strncmp(reply, "HTTP/1.1 429", 12) == 0) {
            snprintf(error, error_size, "too many attempts; wait 30s");
        } else if (strncmp(reply, "HTTP/1.1 400", 12) == 0) {
            snprintf(error, error_size, "the host has no password set");
        } else {
            snprintf(error, error_size, "the host refused the login");
        }
        return 0;
    }

    char *body = strstr(reply, "\r\n\r\n");
    if (!body) {
        snprintf(error, error_size, "no token in the reply");
        return 0;
    }
    body += 4;
    body[strcspn(body, "\r\n")] = '\0';
    if (strlen(body) != 64) {
        snprintf(error, error_size, "unexpected token (%zu chars)", strlen(body));
        return 0;
    }
    snprintf(token, token_size, "%s", body);
    return 1;
}

void net_send_codec(int codec) {
    if (g_stage != LINK_UP) {
        return;
    }
    uint8_t c = (uint8_t)codec;
    send_message(C2S_MSG_CODEC, 0, &c, 1);
}

void net_send_home(void) {
    if (g_stage != LINK_UP || !g_info.may_control) {
        return;
    }
    send_message(C2S_MSG_HOME, 0, NULL, 0);
}
