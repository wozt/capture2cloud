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

#include <SDL2/SDL.h>
#include <switch.h>

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
static uint32_t g_rx_len = 0;
static uint32_t g_rx_cap = 0;

/* The reassembly buffer shifts on every consume, so a payload cannot be
 * handed out as a pointer into it: it is copied here, valid until the
 * next call, which is what net.h promises. */
static uint8_t *g_scratch = NULL;
static uint32_t g_scratch_cap = 0;

static uint64_t now_ms(void) {
    return (uint64_t)armTicksToNs(armGetSystemTick()) / 1000000ull;
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
static SDL_mutex *g_tx_lock = NULL;

int net_init(void) {
    memset(&g_info, 0, sizeof(g_info));
    g_info.state = NET_IDLE;
    set_status("not connected");
    note_step("idle", 0);

    g_rx = malloc(RX_INITIAL);
    if (!g_rx) {
        set_status("out of memory");
        return -1;
    }
    g_rx_cap = RX_INITIAL;

    g_tx_lock = SDL_CreateMutex();
    if (!g_tx_lock) {
        set_status("out of memory");
        return -1;
    }
    return 0;
}

static void close_socket(void) {
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    g_rx_len = 0;
    g_stage = LINK_NONE;
}

void net_exit(void) {
    close_socket();
    free(g_rx);
    g_rx = NULL;
    g_rx_cap = 0;
    free(g_scratch);
    g_scratch = NULL;
    g_scratch_cap = 0;
    if (g_tx_lock) {
        SDL_DestroyMutex(g_tx_lock);
        g_tx_lock = NULL;
    }
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
    h.reserved = 0;
    h.size = size;

    if (g_tx_lock) SDL_LockMutex(g_tx_lock);
    int rc = 0;
    if (send_all(&h, sizeof(h)) != 0) {
        rc = -1;
    } else if (size && send_all(payload, size) != 0) {
        rc = -1;
    }
    if (g_tx_lock) SDL_UnlockMutex(g_tx_lock);
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
    int rcvbuf = 192 * 1024;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

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

    uint8_t token_len = (uint8_t)strlen(g_token);
    C2sHello hello;
    hello.magic = C2S_MAGIC;
    hello.version = C2S_VERSION;
    hello.token_len = token_len;
    hello.reserved = 0;
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
static int rx_reserve(uint32_t needed) {
    if (needed <= g_rx_cap) {
        return 1;
    }
    if (needed > C2S_MAX_PAYLOAD + sizeof(C2sFrameHeader)) {
        return 0;
    }
    uint32_t cap = g_rx_cap ? g_rx_cap : RX_INITIAL;
    while (cap < needed) cap *= 2;
    uint8_t *bigger = realloc(g_rx, cap);
    if (!bigger) {
        return 0;
    }
    g_rx = bigger;
    g_rx_cap = cap;
    return 1;
}

static int fill_rx(void) {
    for (;;) {
        if (g_rx_len >= g_rx_cap && !rx_reserve(g_rx_cap * 2)) {
            return 0; /* full; the caller consumes before more fits */
        }
        ssize_t n = recv(g_sock, g_rx + g_rx_len, g_rx_cap - g_rx_len, 0);
        if (n > 0) {
            g_rx_len += (uint32_t)n;
            g_info.rx_bytes += (uint64_t)n;
            continue;
        }
        if (n == 0) {
            return -1; /* the host closed */
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
}

static void consume(uint32_t bytes) {
    memmove(g_rx, g_rx + bytes, g_rx_len - bytes);
    g_rx_len -= bytes;
}

static void handle_handshake_reply(void) {
    if (g_rx_len < sizeof(C2sHelloAck)) {
        return;
    }
    C2sHelloAck ack;
    memcpy(&ack, g_rx, sizeof(ack));
    consume(sizeof(ack));

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
        fail("connection lost", errno, "the host closed the connection");
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

int net_take_frame(const uint8_t **payload, uint32_t *size, uint8_t *flags) {
    if (g_stage != LINK_UP || g_rx_len < sizeof(C2sFrameHeader)) {
        return 0;
    }
    C2sFrameHeader h;
    memcpy(&h, g_rx, sizeof(h));

    if (h.size > C2S_MAX_PAYLOAD) {
        fail("bad frame size", 0, "stream out of step, reconnecting");
        return 0;
    }
    if (!rx_reserve((uint32_t)sizeof(h) + h.size)) {
        fail("frame too large", 0, "a %u-byte frame does not fit", h.size);
        return 0;
    }
    if (g_rx_len < sizeof(h) + h.size) {
        return 0; /* still arriving */
    }

    if (h.size > g_scratch_cap) {
        uint8_t *bigger = realloc(g_scratch, h.size);
        if (!bigger) {
            return 0;
        }
        g_scratch = bigger;
        g_scratch_cap = h.size;
    }
    if (h.size) {
        memcpy(g_scratch, g_rx + sizeof(h), h.size);
    }
    consume((uint32_t)sizeof(h) + h.size);

    *payload = g_scratch;
    *size = h.size;
    *flags = h.flags;
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
    C2sProfile p = {(uint16_t)width, (uint16_t)height, (uint16_t)fps, (uint16_t)bitrate_kbps};
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
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

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
    while (got < sizeof(reply) - 1) {
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
