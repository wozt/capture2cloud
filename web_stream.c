#define _GNU_SOURCE

#include "web_stream.h"

#include "app_config.h"
#include "gamepad_bridge.h"
#include "video_capture.h"

#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WS_ACCEPT_BACKLOG 8
#define WS_POLL_TIMEOUT_MS 200
#define WS_REQUEST_TIMEOUT_S 5
#define WS_MAX_OFFER_SIZE (1 << 20) /* 1 MiB, very generous for an SDP offer */
#define WS_MAX_STATIC_FILE_SIZE (4 << 20) /* 4 MiB cap when serving page.html/app.js from disk */

/* --- "viewer by default, log in to play" ---
 * Anyone who loads the page can watch; driving the console requires
 * logging in. The password lives in a git-ignored file next to the
 * sources, NEVER in this source or in the served page. It lives in the
 * same git-ignored .env as the Home Assistant credentials, so there is
 * exactly one file holding this project's secrets:
 *
 *   capture2cloud/scripts/.env   ->   PLAYER_PASSWORD=...
 *
 * If that entry is missing or empty, control is left OPEN to everyone --
 * i.e. exactly the behavior this project had before login existed.
 *
 * Sessions are opaque random tokens kept in memory only: they die with
 * the process, which is the right lifetime for "who may play right
 * now". No expiry beyond that -- a fixed small table, oldest entry
 * reused when full. */
#define WS_PASSWORD_KEY "PLAYER_PASSWORD"
#define WS_MAX_PASSWORD_LEN 256
#define WS_TOKEN_HEX_LEN 64 /* 32 random bytes, hex-encoded */
#define WS_LOGIN_MAX_FAILS 5
#define WS_LOGIN_LOCKOUT_SECONDS 30
#define WS_MAX_SESSIONS 16
#define WS_LOGIN_FAIL_DELAY_US (500 * 1000) /* throttles password guessing */

struct WebStream {
    GstWebrtcStream *webrtc;

    SDL_mutex *state_mutex;
    int running;
    int stop_requested;
    int listen_fd;
    int port;
    SDL_Thread *accept_thread;

    /* Guards the session table below (touched from every per-connection
     * thread). Separate from state_mutex: unrelated concerns, and this
     * one is taken on the /login and /offer paths. */
    SDL_mutex *auth_mutex;
    char sessions[WS_MAX_SESSIONS][WS_TOKEN_HEX_LEN + 1];
    int session_count;
    int session_next; /* round-robin slot to reuse once full */

    /* Failed-login throttling, server-wide. Also under auth_mutex. */
    int login_fails;
    time_t login_locked_until;
};

typedef struct {
    WebStream *ws;
    int fd;
} ClientCtx;

WebStream *web_stream_create(GstWebrtcStream *webrtc) {
    WebStream *ws = calloc(1, sizeof(*ws));
    if (!ws) {
        return NULL;
    }
    ws->webrtc = webrtc;
    ws->listen_fd = -1;
    ws->state_mutex = SDL_CreateMutex();
    ws->auth_mutex = SDL_CreateMutex();
    if (!ws->state_mutex || !ws->auth_mutex) {
        if (ws->state_mutex) SDL_DestroyMutex(ws->state_mutex);
        if (ws->auth_mutex) SDL_DestroyMutex(ws->auth_mutex);
        free(ws);
        return NULL;
    }
    return ws;
}

void web_stream_destroy(WebStream *ws) {
    if (!ws) {
        return;
    }
    web_stream_stop(ws);
    SDL_DestroyMutex(ws->state_mutex);
    SDL_DestroyMutex(ws->auth_mutex);
    free(ws);
}

int web_stream_is_running(WebStream *ws) {
    SDL_LockMutex(ws->state_mutex);
    int running = ws->running;
    SDL_UnlockMutex(ws->state_mutex);
    return running;
}

int web_stream_get_port(WebStream *ws) {
    SDL_LockMutex(ws->state_mutex);
    int port = ws->port;
    SDL_UnlockMutex(ws->state_mutex);
    return port;
}

static ssize_t send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static int read_line(int fd, char *buf, size_t bufsize) {
    size_t i = 0;
    while (i + 1 < bufsize) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return 0;
}

static int read_exact(int fd, char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void send_404(int fd) {
    static const char body[] = "not found";
    char header[128];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      sizeof(body) - 1);
    send_all(fd, header, (size_t)n);
    send_all(fd, body, sizeof(body) - 1);
}

static void send_400(int fd, const char *msg) {
    size_t len = strlen(msg);
    char header[192];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      len);
    send_all(fd, header, (size_t)n);
    send_all(fd, msg, len);
}

static void send_204(int fd) {
    static const char response[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    send_all(fd, response, sizeof(response) - 1);
}

/* Serves page.html/app.js straight from disk, next to this source file.
 *
 * Editing the front-end used to mean re-running an escaping script that
 * regenerated two big C string literals and splicing them back into this
 * file at hand-computed line numbers, then recompiling -- a fragile,
 * easy-to-half-skip process. Reading from disk means a browser refresh
 * is enough to pick up a change.
 *
 * There is deliberately NO built-in fallback copy: a second copy of the
 * page inside the binary would inevitably drift out of sync with the
 * real files and could silently serve a months-old version the day the
 * files went missing -- a confusing failure mode, and a worse one than
 * an obvious 404. Missing/unreadable files are reported as 404 instead.
 *
 * No in-process caching either, on purpose: always re-reading is what
 * makes edit-then-refresh work, and the cost is nil -- these files are
 * a few tens of KB, read once per page load (not per frame), and the
 * kernel keeps them in its page cache after the first read anyway, so
 * subsequent reads never actually touch the disk. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || size > WS_MAX_STATIC_FILE_SIZE) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

static void send_static(int fd, const char *filename, const char *content_type) {
    char path[PATH_MAX];
    app_path(path, sizeof(path), filename);

    size_t body_len = 0;
    char *body = read_file(path, &body_len);
    if (!body) {
        fprintf(stderr, "web_stream: cannot read %s\n", path);
        send_404(fd);
        return;
    }

    char header[200];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      content_type, body_len);
    send_all(fd, header, (size_t)n);
    send_all(fd, body, body_len);
    free(body);
}

static void send_html_page(int fd) {
    send_static(fd, "page.html", "text/html; charset=utf-8");
}

static void send_app_js(int fd) {
    send_static(fd, "app.js", "application/javascript; charset=utf-8");
}

/* Reads PLAYER_PASSWORD from the .env, or returns 0 if it's absent or
 * empty -- in which case control stays open to everyone, preserving the
 * pre-login behavior. Read on every call rather than cached, so editing
 * the file takes effect without a restart. */
static int load_player_password(char *out, size_t out_size) {
    return config_get(WS_PASSWORD_KEY, out, out_size);
}

static int password_required(void) {
    char pw[WS_MAX_PASSWORD_LEN];
    return load_player_password(pw, sizeof(pw));
}

/* Constant-time comparison: a plain strcmp() returns as soon as two
 * bytes differ, so its timing leaks how many leading characters were
 * right, which is enough to recover a password one character at a time
 * over enough attempts. */
/* Failed logins are counted for the whole server, not per connection.
 *
 * The half-second penalty on a wrong password only ever slowed the
 * connection that made the guess. Measured against this server: 30
 * guesses fired in parallel finished in 505 ms rather than 15 s, so the
 * delay was worth almost nothing against anyone willing to open more
 * than one socket -- roughly 60 guesses a second, against a default
 * password of "changeme".
 *
 * The counter is shared, so parallelism no longer helps: five failures
 * from anywhere stop login attempts for everyone briefly. That does let
 * someone lock the real user out for half a minute at a time, which on a
 * LAN/Tailscale service is the better half of the trade -- they could
 * already have been guessing instead. */
static int login_is_locked(WebStream *ws) {
    SDL_LockMutex(ws->auth_mutex);
    int locked = ws->login_locked_until > time(NULL);
    SDL_UnlockMutex(ws->auth_mutex);
    return locked;
}

static void login_record_failure(WebStream *ws) {
    SDL_LockMutex(ws->auth_mutex);
    if (++ws->login_fails >= WS_LOGIN_MAX_FAILS) {
        ws->login_fails = 0;
        ws->login_locked_until = time(NULL) + WS_LOGIN_LOCKOUT_SECONDS;
        fprintf(stderr, "web_stream: %d failed logins, refusing further attempts for %d s\n",
                WS_LOGIN_MAX_FAILS, WS_LOGIN_LOCKOUT_SECONDS);
    }
    SDL_UnlockMutex(ws->auth_mutex);
}

static void login_record_success(WebStream *ws) {
    SDL_LockMutex(ws->auth_mutex);
    ws->login_fails = 0;
    ws->login_locked_until = 0;
    SDL_UnlockMutex(ws->auth_mutex);
}

static int secure_equals(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* 32 bytes from the kernel CSPRNG, hex-encoded. rand() would be
 * guessable from a couple of observed tokens; this is not. */
static int generate_token(char *out_hex) {
    unsigned char raw[WS_TOKEN_HEX_LEN / 2];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) {
        return 0;
    }
    size_t got = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (got != sizeof(raw)) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(out_hex + i * 2, 3, "%02x", raw[i]);
    }
    out_hex[WS_TOKEN_HEX_LEN] = '\0';
    return 1;
}

static void session_add(WebStream *ws, const char *token) {
    SDL_LockMutex(ws->auth_mutex);
    int slot;
    if (ws->session_count < WS_MAX_SESSIONS) {
        slot = ws->session_count++;
    } else {
        slot = ws->session_next;
        ws->session_next = (ws->session_next + 1) % WS_MAX_SESSIONS;
    }
    snprintf(ws->sessions[slot], sizeof(ws->sessions[slot]), "%s", token);
    SDL_UnlockMutex(ws->auth_mutex);
}

static int session_is_valid(WebStream *ws, const char *token) {
    if (!token || !token[0]) {
        return 0;
    }
    SDL_LockMutex(ws->auth_mutex);
    int found = 0;
    for (int i = 0; i < ws->session_count; i++) {
        if (secure_equals(ws->sessions[i], token)) {
            found = 1;
            break;
        }
    }
    SDL_UnlockMutex(ws->auth_mutex);
    return found;
}

/* Whether this request may drive the console: yes if no password is
 * configured at all (open setup), otherwise only with a valid session
 * token. */
static int request_may_control(WebStream *ws, const char *token) {
    if (!password_required()) {
        return 1;
    }
    return session_is_valid(ws, token);
}

/* POST /login, body = the password. On success returns the session token
 * as plain text; the browser then sends it back on /offer. */
static void handle_login(WebStream *ws, int fd, long content_length) {
    if (content_length <= 0 || content_length > WS_MAX_PASSWORD_LEN) {
        send_400(fd, "invalid password length");
        return;
    }

    char submitted[WS_MAX_PASSWORD_LEN + 1];
    if (read_exact(fd, submitted, (size_t)content_length) != 0) {
        return;
    }
    submitted[content_length] = '\0';
    submitted[strcspn(submitted, "\r\n")] = '\0';

    /* Checked after reading the body so the response is deliverable, but
     * before comparing anything: during a lockout even the right
     * password is refused, or guessing through it would still work. */
    if (login_is_locked(ws)) {
        static const char response[] =
            "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 30\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        return;
    }

    char expected[WS_MAX_PASSWORD_LEN];
    if (!load_player_password(expected, sizeof(expected))) {
        /* No password configured: control is already open, so there is
         * nothing to log into. Say so explicitly rather than handing out
         * a token that grants nothing extra. */
        send_400(fd, "no player password configured (control is open)");
        return;
    }

    if (!secure_equals(submitted, expected)) {
        login_record_failure(ws);
        /* Still worth keeping alongside the shared counter: it is what
         * slows a patient single-threaded attacker between lockouts. */
        usleep(WS_LOGIN_FAIL_DELAY_US);
        static const char response[] = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        return;
    }

    char token[WS_TOKEN_HEX_LEN + 1];
    if (!generate_token(token)) {
        send_400(fd, "could not generate a session token");
        return;
    }
    login_record_success(ws);
    session_add(ws, token);

    char header[200];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      strlen(token));
    send_all(fd, header, (size_t)n);
    send_all(fd, token, strlen(token));
}

/* GET /clients -> "2/8", the number of connected clients over the hard
 * limit. Shown in the page's stats line so it's obvious when someone
 * else is watching, and when the limit is being approached (past it,
 * /offer starts refusing connections). */
static void handle_clients(WebStream *ws, int fd) {
    int max_clients = 0;
    int count = gst_webrtc_stream_get_client_count(ws->webrtc, &max_clients);

    char body[32];
    int body_len = snprintf(body, sizeof(body), "%d/%d", count, max_clients);
    char header[200];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      body_len);
    send_all(fd, header, (size_t)n);
    send_all(fd, body, (size_t)body_len);
}

/* GET /auth-status -> "required" or "open", so the page knows whether to
 * show a login button at all. Deliberately reveals nothing else. */
static void handle_auth_status(int fd) {
    const char *body = password_required() ? "required" : "open";
    char header[200];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      strlen(body));
    send_all(fd, header, (size_t)n);
    send_all(fd, body, strlen(body));
}

static void handle_offer(WebStream *ws, int fd, long content_length, const char *token) {
    if (content_length <= 0 || content_length > WS_MAX_OFFER_SIZE) {
        send_400(fd, "invalid offer size");
        return;
    }

    char *body = malloc((size_t)content_length + 1);
    if (!body) {
        send_400(fd, "out of memory");
        return;
    }
    if (read_exact(fd, body, (size_t)content_length) != 0) {
        free(body);
        return;
    }
    body[content_length] = '\0';

    /* Viewer vs player is decided once, here, and baked into the client
     * slot -- not re-checked per gamepad message, so the input path
     * keeps its current latency. An unauthorized client still gets a
     * perfectly normal video/audio stream. */
    int may_control = request_may_control(ws, token);
    fprintf(stderr, "web_stream: new client connecting as %s\n", may_control ? "PLAYER" : "viewer");
    /* Where this offer came from is also where the media will go, so it
     * tells the RTP payloader which path it has to fit through. */
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    int have_peer = getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0;
    char *answer = gst_webrtc_stream_handle_offer(ws->webrtc, body, may_control,
                                                  have_peer ? (struct sockaddr *)&peer : NULL,
                                                  have_peer ? peer_len : 0);
    free(body);

    if (!answer) {
        send_400(fd, "WebRTC negotiation failed");
        return;
    }

    size_t answer_len = strlen(answer);
    /* Tell the page what it actually got. Session tokens live only in
     * this process's memory, so a restart invalidates every one of them
     * while browsers happily keep theirs: without this the page would
     * still show "player", the server would treat it as a viewer, and
     * every input would be dropped in silence -- which is exactly as
     * confusing as it sounds. The page compares this against what it
     * believes and corrects itself. */
    char header[220];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: application/sdp\r\nX-Player-Granted: %d\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      may_control ? 1 : 0, answer_len);
    send_all(fd, header, (size_t)n);
    send_all(fd, answer, answer_len);
    free(answer);
}

/* Players only. Re-enumerates the adapter by hand -- the same thing the
 * wake path does once the picture comes back, exposed as a button
 * because the adapter occasionally needs it after the console has been
 * fiddled with in ways this program never sees. */
int web_stream_may_control(WebStream *ws, const char *token) {
    return request_may_control(ws, token);
}

/* Reports per second reaching the adapter. Read-only and open to
 * anyone: it is one number about this machine's own USB traffic, it says
 * nothing about who is connected or what they pressed, and it is the
 * only way to observe from outside that input is actually getting
 * through. */
static void handle_gamepad_rate(int fd) {
    char body[32];
    int n = snprintf(body, sizeof(body), "%.1f", gamepad_bridge_report_rate());
    char header[128];
    int hn = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n", n);
    send_all(fd, header, (size_t)hn);
    send_all(fd, body, (size_t)n);
}

static void handle_reset_dongle(WebStream *ws, int fd, const char *token) {
    if (!request_may_control(ws, token)) {
        static const char response[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        return;
    }
    fprintf(stderr, "web_stream: adapter reset requested from the page\n");
    gamepad_bridge_reset();
    send_204(fd);
}

/* Players only, and global like the bitrate: one encoder feeds every
 * browser client, so the resolution is shared. Only the browser stream
 * is affected -- the capture stays at the card's own size, and the
 * native (Switch) branch has its own. */
static void handle_resolution(WebStream *ws, int fd, long content_length, const char *token) {
    if (!request_may_control(ws, token)) {
        static const char response[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        return;
    }
    if (content_length <= 0 || content_length > 16) {
        send_400(fd, "invalid resolution");
        return;
    }
    char body[17];
    if (read_exact(fd, body, (size_t)content_length) != 0) {
        return;
    }
    body[content_length] = '\0';
    body[strcspn(body, "\r\n")] = '\0';

    /* 16:9 throughout, and every dimension even -- VP8 rejects odd ones,
     * and a half-pixel chroma plane is not a thing. */
    int w = 0, h = 0;
    if (strcmp(body, "1080") == 0)      { w = 1920; h = 1080; }
    else if (strcmp(body, "720") == 0)  { w = 1280; h = 720; }
    else if (strcmp(body, "480") == 0)  { w = 854;  h = 480; }
    else {
        send_400(fd, "resolution must be 1080, 720 or 480");
        return;
    }
    gst_webrtc_stream_set_browser_resolution(ws->webrtc, w, h);
    send_204(fd);
}

/* Readable by anyone: it is what the page shows. */
static void handle_resolution_get(WebStream *ws, int fd) {
    int w = 0, h = 0;
    gst_webrtc_stream_get_browser_resolution(ws->webrtc, &w, &h);
    const char *body = (h == 480) ? "480" : (h == 720) ? "720" : "1080";
    char header[160];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      strlen(body));
    send_all(fd, header, (size_t)n);
    send_all(fd, body, strlen(body));
}

/* Players only, and global for the same reason as the bitrate: there is
 * one capture device. Exists so the two capture formats can be compared
 * over hours of real use without editing the .env and restarting --
 * which is the only way to tell whether YUYV is behind the freezes. */
static void handle_capture_format(WebStream *ws, int fd, long content_length, const char *token) {
    if (!request_may_control(ws, token)) {
        static const char response[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        return;
    }
    if (content_length <= 0 || content_length > 16) {
        send_400(fd, "invalid format");
        return;
    }
    char body[17];
    if (read_exact(fd, body, (size_t)content_length) != 0) {
        return;
    }
    body[content_length] = '\0';
    body[strcspn(body, "\r\n")] = '\0';

    if (strcmp(body, "yuyv") == 0) {
        video_capture_request_format(VIDEO_FORMAT_YUYV);
    } else if (strcmp(body, "mjpeg") == 0) {
        video_capture_request_format(VIDEO_FORMAT_MJPEG);
    } else {
        send_400(fd, "format must be yuyv or mjpeg");
        return;
    }
    send_204(fd);
}

/* Readable by anyone: it is what the page shows, and it says what is
 * actually running rather than what was last asked for. */
static void handle_capture_format_get(int fd) {
    const char *body = video_format_name(video_capture_active_format());
    char header[160];
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      strlen(body));
    send_all(fd, header, (size_t)n);
    send_all(fd, body, strlen(body));
}

/* Players only. There is ONE shared encoder feeding every client, so the
 * bitrate is global: a viewer lowering it would degrade the picture for
 * the person actually playing. Enforced here and not merely by hiding
 * the slider, for the same reason as everywhere else -- anyone can POST
 * to this endpoint directly. */
static void handle_quality(WebStream *ws, int fd, long content_length, const char *token) {
    if (!request_may_control(ws, token)) {
        static const char response[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        return;
    }
    if (content_length <= 0 || content_length > 32) {
        send_400(fd, "invalid bitrate");
        return;
    }

    char body[33];
    if (read_exact(fd, body, (size_t)content_length) != 0) {
        return;
    }
    body[content_length] = '\0';

    int bitrate_kbps = atoi(body);
    if (bitrate_kbps < 1000 || bitrate_kbps > 50000) {
        send_400(fd, "bitrate out of range");
        return;
    }

    gst_webrtc_stream_set_video_bitrate(ws->webrtc, bitrate_kbps);
    send_204(fd);
}

/* Wakes the console from sleep by power-cycling its smart plug via the
 * standalone scripts/wake_console.sh -- kept as an external script
 * rather than reimplemented in C so the plug/service can be swapped
 * later without touching this file. Backgrounded with a trailing "&":
 * the script polls Home Assistant until each state change is confirmed
 * and can take several seconds, and this request must not hold the HTTP
 * response (or this connection's thread) open that long. Manually
 * triggered only (a button on the page), never automatic.
 *
 * Players only: cutting the console's power is at least as disruptive as
 * pressing its buttons, so it sits behind the same gate as gamepad
 * input. Enforced here rather than only by hiding the button, for the
 * same reason as everywhere else -- anyone can POST to this endpoint
 * directly. */
/* Runs the wake script to completion, then re-enumerates the adapter.
 *
 * The adapter last handshook with a console that was asleep, and coming
 * out of standby is not enough for it to be seen again -- it has to
 * re-enumerate, which is what a physical unplug/replug would do. Done
 * here, after the script returns, rather than by the script itself: the
 * adapter is held open by this process, so only this process can reset
 * it.
 *
 * On its own thread because the script polls Home Assistant until each
 * state change is confirmed and takes several seconds; the HTTP response
 * must not wait for that. */
static int wake_then_reset_thread(void *arg) {
    char *cmd = arg;
    int rc = system(cmd);
    free(cmd);
    if (rc != 0) {
        fprintf(stderr, "web_stream: wake_console.sh exited with %d\n", rc);
    }
    /* The adapter is NOT reset here. Resetting as soon as the script
     * returned was too early: the console is still around ten seconds
     * from drawing anything, the USB link came back while it was not
     * listening, and the adapter never re-attached -- the gamepad simply
     * did not come back. The capture loop resets it instead, on the
     * frame where the picture stops being the "no signal" pattern. */
    video_capture_watch_for_change();
    return 0;
}

/* The wake itself, without the HTTP around it. Shared so the native
 * transport can offer the same button: there is no browser on the
 * console, and needing one to wake the console the client exists to show
 * would be a poor joke. Returns 0 when the work was started. */
int web_stream_wake_console(WebStream *ws) {
    (void)ws;
    char script[PATH_MAX];
    app_path(script, sizeof(script), "scripts/wake_console.sh");

    char *cmd = malloc(PATH_MAX + 64);
    if (!cmd) {
        return -1;
    }
    snprintf(cmd, PATH_MAX + 64, "/bin/bash '%s' >/dev/null 2>&1", script);

    SDL_Thread *t = SDL_CreateThread(wake_then_reset_thread, "wake-console", cmd);
    if (!t) {
        free(cmd);
        return -1;
    }
    SDL_DetachThread(t);
    return 0;
}

static void handle_wake(WebStream *ws, int fd, const char *token) {
    if (!request_may_control(ws, token)) {
        static const char response[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, response, sizeof(response) - 1);
        return;
    }

    if (web_stream_wake_console(ws) != 0) {
        send_400(fd, "failed to launch wake_console.sh");
        return;
    }
    send_204(fd);
}

static int client_thread(void *arg) {
    ClientCtx *ctx = arg;
    WebStream *ws = ctx->ws;
    int fd = ctx->fd;
    free(ctx);

    char line[512];
    if (read_line(fd, line, sizeof(line)) == 0) {
        char method[8] = {0};
        char path[256] = {0};
        sscanf(line, "%7s %255s", method, path);

        char *query = strchr(path, '?');
        if (query) *query = '\0';

        long content_length = 0;
        char token[WS_TOKEN_HEX_LEN + 1] = {0};
        for (;;) {
            if (read_line(fd, line, sizeof(line)) != 0) break;
            if (line[0] == '\0') break; /* empty line = end of headers */
            long cl;
            if (sscanf(line, "Content-Length: %ld", &cl) == 1 || sscanf(line, "content-length: %ld", &cl) == 1) {
                content_length = cl;
            }
            /* Session token carried in its own header rather than in the
             * body, so /offer's body stays pure SDP. */
            char tok[WS_TOKEN_HEX_LEN + 1];
            if (sscanf(line, "X-Player-Token: %64s", tok) == 1 || sscanf(line, "x-player-token: %64s", tok) == 1) {
                snprintf(token, sizeof(token), "%s", tok);
            }
        }

        if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
            send_html_page(fd);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/app.js") == 0) {
            send_app_js(fd);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/auth-status") == 0) {
            handle_auth_status(fd);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/resolution") == 0) {
            handle_resolution_get(ws, fd);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/resolution") == 0) {
            handle_resolution(ws, fd, content_length, token);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/capture-format") == 0) {
            handle_capture_format_get(fd);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/capture-format") == 0) {
            handle_capture_format(ws, fd, content_length, token);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/clients") == 0) {
            handle_clients(ws, fd);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/gamepad-rate") == 0) {
            handle_gamepad_rate(fd);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/login") == 0) {
            handle_login(ws, fd, content_length);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/offer") == 0) {
            handle_offer(ws, fd, content_length, token);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/quality") == 0) {
            handle_quality(ws, fd, content_length, token);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/reset-dongle") == 0) {
            handle_reset_dongle(ws, fd, token);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/wake") == 0) {
            handle_wake(ws, fd, token);
        } else {
            send_404(fd);
        }
    }

    close(fd);
    return 0;
}

static int accept_thread_func(void *arg) {
    WebStream *ws = arg;

    for (;;) {
        SDL_LockMutex(ws->state_mutex);
        int stop = ws->stop_requested;
        int fd = ws->listen_fd;
        SDL_UnlockMutex(ws->state_mutex);
        if (stop) break;

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, WS_POLL_TIMEOUT_MS);
        if (pr <= 0) continue;

        int client_fd = accept(fd, NULL, NULL);
        if (client_fd < 0) continue;

        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv = {.tv_sec = WS_REQUEST_TIMEOUT_S, .tv_usec = 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ClientCtx *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            close(client_fd);
            continue;
        }
        ctx->ws = ws;
        ctx->fd = client_fd;

        SDL_Thread *t = SDL_CreateThread(client_thread, "ws-client", ctx);
        if (t) {
            SDL_DetachThread(t);
        } else {
            close(client_fd);
            free(ctx);
        }
    }

    SDL_LockMutex(ws->state_mutex);
    close(ws->listen_fd);
    ws->listen_fd = -1;
    SDL_UnlockMutex(ws->state_mutex);
    return 0;
}

int web_stream_start(WebStream *ws, int port, char *errbuf, size_t errbuf_len) {
    SDL_LockMutex(ws->state_mutex);
    if (ws->running) {
        SDL_UnlockMutex(ws->state_mutex);
        if (errbuf) snprintf(errbuf, errbuf_len, "already started");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (errbuf) snprintf(errbuf, errbuf_len, "socket: %s", strerror(errno));
        SDL_UnlockMutex(ws->state_mutex);
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errbuf) snprintf(errbuf, errbuf_len, "bind port %d: %s", port, strerror(errno));
        close(fd);
        SDL_UnlockMutex(ws->state_mutex);
        return -1;
    }

    if (listen(fd, WS_ACCEPT_BACKLOG) < 0) {
        if (errbuf) snprintf(errbuf, errbuf_len, "listen: %s", strerror(errno));
        close(fd);
        SDL_UnlockMutex(ws->state_mutex);
        return -1;
    }

    ws->listen_fd = fd;
    ws->port = port;
    ws->stop_requested = 0;
    ws->running = 1;
    SDL_UnlockMutex(ws->state_mutex);

    ws->accept_thread = SDL_CreateThread(accept_thread_func, "ws-accept", ws);
    if (!ws->accept_thread) {
        if (errbuf) snprintf(errbuf, errbuf_len, "SDL_CreateThread failed");
        SDL_LockMutex(ws->state_mutex);
        close(ws->listen_fd);
        ws->listen_fd = -1;
        ws->running = 0;
        SDL_UnlockMutex(ws->state_mutex);
        return -1;
    }

    return 0;
}

void web_stream_stop(WebStream *ws) {
    SDL_LockMutex(ws->state_mutex);
    if (!ws->running) {
        SDL_UnlockMutex(ws->state_mutex);
        return;
    }
    ws->stop_requested = 1;
    SDL_UnlockMutex(ws->state_mutex);

    if (ws->accept_thread) {
        SDL_WaitThread(ws->accept_thread, NULL);
        ws->accept_thread = NULL;
    }

    SDL_LockMutex(ws->state_mutex);
    ws->running = 0;
    SDL_UnlockMutex(ws->state_mutex);
}
