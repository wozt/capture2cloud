#include "switch_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "c2s_protocol.h"
#include "app_config.h"
#include "gamepad_bridge.h"
#include "web_stream.h"

/* Small on purpose: this is one console in one room, not a broadcast. */
#define SS_MAX_CLIENTS 4

/* A client that has said nothing for this long is gone, whatever the
 * socket thinks. The client pings every 2 s, so this is generous. */
#define SS_IDLE_TIMEOUT_MS 10000

/* Per-client receive buffer. Only input, pings and the handshake arrive
 * this way, all tiny. */
#define SS_RX_CAPACITY 512

/* How much unsent video may sit in one client's socket before a new
 * frame is skipped instead of queued.
 *
 * The app-level check below only sees a frame it could not hand to the
 * kernel. But send() succeeds long before anything reaches the client:
 * the socket's own send buffer is megabytes, so on a link that cannot
 * keep up the app believed every frame was delivered while several
 * seconds of them sat in the kernel. The picture was whole and minutes
 * of play behind -- the worst possible failure for something you are
 * holding a controller for.
 *
 * The allowance is derived from the largest frame recently sent rather
 * than being a fixed number of bytes, and that is the whole point.
 *
 * A flat 64 KB looked reasonable -- several average frames at the
 * bitrates used here -- and was smaller than a single keyframe, which
 * measured 66 KB at 720p. So every keyframe put the socket over the
 * limit by itself and the frames right after it were skipped; skipping
 * asks for a keyframe; and the console showed a permanent smear. The
 * console's wifi made it worse: measured, its round trip swings from
 * 0.8 ms to 116 ms, and one 116 ms pause at 6 Mbit/s parks 87 KB in the
 * socket with nothing wrong at all.
 *
 * Two of the largest recent frames, plus a little, is a backlog that a
 * keyframe and a wifi hiccup can both fit inside, and still an order of
 * magnitude below the seconds a kernel buffer would happily hold. */
#define SS_INFLIGHT_MIN_BYTES 65536
#define SS_INFLIGHT_MAX_BYTES 524288
#define SS_INFLIGHT_HEADROOM 32768

/* Shortest gap between two "the link is behind" lines. */
#define SS_SKIP_REPORT_INTERVAL_MS 5000

/* The kernel is told not to hold much more than the largest allowance,
 * so the measurement cannot be defeated by a buffer that simply absorbs
 * everything. */
#define SS_SOCKET_SNDBUF 262144

/* Shortest gap between forced keyframes. See last_keyframe_ms. */
#define SS_KEYFRAME_MIN_INTERVAL_MS 1000

typedef struct {
    int fd;
    int in_use;
    int handshake_done;
    int may_control;
    /* Which video codec THIS client is being sent.
     *
     * It used to be one setting for the whole server: a client asking
     * for H.264 moved everyone to H.264, including whoever was happily
     * decoding VP8. Two chains are fed now, so this is the client's own
     * business and nobody else's. */
    uint8_t codec;
    uint32_t last_seen_ms;
    uint8_t rx[SS_RX_CAPACITY];
    uint32_t rx_len;

    /* What a send could not finish. A frame is header+payload and must
     * arrive whole or the client loses its place in the stream, so a
     * partial write is held here and flushed before the next frame
     * rather than abandoned mid-message. */
    uint8_t *pending;
    uint32_t pending_len, pending_sent, pending_cap;
} SsClient;

struct SwitchStream {
    /* Borrowed, not owned: the session table lives there, and asking it
     * is how this transport gets the same answer as the browser about
     * who may control. */
    WebStream *web;
    int listen_fd;
    uint16_t port;
    uint16_t width, height;

    /* What the pipeline is actually encoding. The handshake used to
     * answer VP8 unconditionally, so a client arriving while the stream
     * was H.264 opened a VP8 decoder and saw nothing until something
     * else happened to change the codec. */
    uint8_t video_codec;

    /* The last shared settings announced, per codec, kept so a client
     * that arrives later is told the same thing as everyone already on
     * ITS stream -- and so it has no reason to announce its own.
     *
     * Indexed by codec_slot(): the two groups share nothing, because
     * they are not watching the same encode. */
    /* How many clients each codec has, kept up to date rather than
     * counted per frame: the video path reads this sixty times a second
     * and must not take the client lock to do it. */
    volatile int live[2];
    volatile int demand_dirty;

    uint32_t max_frame_bytes_by_codec[2];
    C2sShared shared[2];
    int shared_known[2];
    uint16_t group_width[2], group_height[2];
    int group_stream_known[2];

    /* Told whenever the set of codecs anybody is watching changes, so
     * the pipeline can start feeding a chain or stop wasting a core on
     * one nobody is reading. */
    void (*demand_cb)(void *ctx);
    void *demand_ctx;

    SDL_mutex *mutex;
    SsClient clients[SS_MAX_CLIENTS];

    SDL_Thread *thread;
    volatile int running;

    SwitchKeyframeRequest keyframe_cb;
    void *keyframe_ctx;
    void (*profile_cb)(void *ctx, int codec, int w, int h, int fps, int bitrate_kbps);
    void *profile_ctx;

    /* When the last forced keyframe went out. Forcing one per skipped
     * frame turned a client that was slightly behind into one that could
     * never catch up: every skip produced a keyframe, keyframes are the
     * largest frames there are, and the extra bytes caused the next
     * skip. The picture went to a few frames a second and then stopped.
     * One a second is enough to recover from a gap without becoming the
     * reason for the next one. */
    uint32_t last_keyframe_ms;

    /* The largest video frame sent recently, decayed so that dropping to
     * a smaller resolution or bitrate is followed rather than remembered
     * forever. Sets the in-flight allowance above. */
    uint32_t max_frame_bytes;
    uint32_t skipped_frames;
    uint32_t last_skip_report_ms;

    /* Set while the client lock is held, acted on once it is released:
     * the callback reaches into the pipeline, and the pipeline's own
     * thread takes this lock to deliver frames. */
    volatile int keyframe_pending;
};

static uint32_t now_ms(void) {
    return SDL_GetTicks();
}

/* Defined further down, with the rest of the sending. */
/* VP8 in slot 0, H.264 in slot 1. Anything else is not a codec this
 * transport carries, and is treated as the default rather than indexing
 * past the end of an array. */
static int codec_slot(int codec) {
    return codec == C2S_CODEC_H264 ? 1 : 0;
}
/* codec_filter 0 means every client; otherwise only those on that
 * codec. Video is always filtered -- handing a client the other
 * chain's bytes produces a picture, and the picture is bright pink. */
static void broadcast(SwitchStream *s, int codec_filter, uint8_t type, uint8_t flags,
                      const uint8_t *data, uint32_t size);
static void recount(SwitchStream *s);
static void send_group_state(SwitchStream *s, int index);

void switch_stream_set_keyframe_request(SwitchStream *s, SwitchKeyframeRequest cb, void *ctx) {
    if (s) { s->keyframe_cb = cb; s->keyframe_ctx = ctx; }
}

/* Announces the shape of ONE of the two streams, to the clients on it.
 *
 * The codec is no longer a property of the server, so neither is this:
 * telling a VP8 client that the stream is now 720p H.264 would make it
 * rebuild its decoder for an encode it is not being sent. */
void switch_stream_announce_stream(SwitchStream *s, uint8_t codec,
                                   uint16_t width, uint16_t height) {
    if (!s) return;
    C2sStreamInfo info;
    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    info.video_codec = codec;
    const int slot = codec_slot(codec);
    s->group_width[slot] = width;
    s->group_height[slot] = height;
    s->group_stream_known[slot] = 1;
    broadcast(s, codec, C2S_MSG_STREAM_INFO, 0, (const uint8_t *)&info, sizeof(info));
}

/* Tells one codec's clients what the settings they have in common now
 * are.
 *
 * Sent on every change rather than polled, and sent to a client the
 * moment it finishes its handshake: the alternative is a client that
 * shows its own saved values while receiving somebody else's stream,
 * with no way of telling which of the two is the truth.
 *
 * "In common" stops at the codec boundary. The two groups are watching
 * two different encodes with their own size, rate and bitrate, so a
 * VP8 viewer dropping to 480p30 has no business moving an H.264
 * viewer's menu -- which is exactly what one shared set of values did. */
void switch_stream_announce_shared(SwitchStream *s, uint16_t width, uint16_t height,
                                   uint16_t fps, uint16_t bitrate_kbps,
                                   uint8_t codec, uint8_t capture_mjpeg) {
    if (!s) return;
    C2sShared sh;
    memset(&sh, 0, sizeof(sh));
    sh.width = width;
    sh.height = height;
    sh.fps = fps;
    sh.bitrate_kbps = bitrate_kbps;
    sh.video_codec = codec;
    sh.capture_mjpeg = capture_mjpeg;
    const int slot = codec_slot(codec);
    s->shared[slot] = sh;
    s->shared_known[slot] = 1;
    broadcast(s, codec, C2S_MSG_SHARED, 0, (const uint8_t *)&sh, sizeof(sh));
}

void switch_stream_set_profile_request(SwitchStream *s,
                                       void (*cb)(void *ctx, int codec, int w, int h, int fps,
                                                  int bitrate_kbps),
                                       void *ctx) {
    if (s) { s->profile_cb = cb; s->profile_ctx = ctx; }
}

/* --- sending -------------------------------------------------------- */

/* Sends what it can without blocking. Returns bytes written, or -1 if
 * the connection is gone. A short count is normal and not an error: the
 * socket buffer is full because the client is momentarily behind. */
static ssize_t send_some(int fd, const void *data, size_t len) {
    ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
    if (n >= 0) {
        return n;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    return -1;
}

/* Sends everything, or fails. Used for the handshake reply, which is 20
 * bytes and has nowhere to be buffered. */
static int send_all_now(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send_some(fd, p + sent, len - sent);
        if (n < 0) return -1;
        if (n == 0) return -1; /* 20 bytes that will not fit means gone */
        sent += (size_t)n;
    }
    return 0;
}

/* Pushes out whatever is left of a partly-sent frame. Returns 0 while
 * the client is fine (whether or not it finished), -1 once it is gone. */
static int flush_pending(SsClient *c) {
    while (c->pending_sent < c->pending_len) {
        ssize_t n = send_some(c->fd, c->pending + c->pending_sent,
                              c->pending_len - c->pending_sent);
        if (n < 0) return -1;
        if (n == 0) return 0; /* still full; try again next frame */
        c->pending_sent += (uint32_t)n;
    }
    c->pending_len = c->pending_sent = 0;
    return 0;
}

static void drop_client(SwitchStream *s, int i, const char *why) {
    if (!s->clients[i].in_use) {
        return;
    }
    fprintf(stderr, "switch_stream: client %d disconnected (%s)\n", i, why);
    /* Whatever it was holding goes with it, rather than staying pressed
     * on the console until something else happens to overwrite it. */
    gamepad_bridge_forget(GAMEPAD_SOURCE_NATIVE(i));
    close(s->clients[i].fd);
    free(s->clients[i].pending);
    memset(&s->clients[i], 0, sizeof(s->clients[i]));
    s->clients[i].fd = -1;
    /* If that was the last client on its codec, the chain it was
     * watching has nobody left and stops being encoded. */
    recount(s);
}

static void broadcast(SwitchStream *s, int codec_filter, uint8_t type, uint8_t flags,
                      const uint8_t *data, uint32_t size) {
    if (!s || size > C2S_MAX_PAYLOAD) {
        return;
    }
    C2sFrameHeader h = {.type = type, .flags = flags, .reserved = 0, .size = size};
    const uint32_t total = (uint32_t)sizeof(h) + size;
    int skipped = 0;

    SDL_LockMutex(s->mutex);

    /* Decays by a sixteenth per frame -- about a second at 60 fps -- so
     * the allowance follows a change of profile within a keyframe
     * interval instead of staying sized for a resolution nobody is
     * watching any more. */
    if (type == C2S_MSG_VIDEO) {
        /* Per codec: a 6 Mb/s H.264 keyframe and a 1 Mb/s VP8 one are
         * not the same size, and one allowance for both would size the
         * small stream's backlog for the large stream's frames. */
        uint32_t *largest = &s->max_frame_bytes_by_codec[codec_slot(codec_filter)];
        *largest -= *largest / 16;
        if (size > *largest) {
            *largest = size;
        }
        s->max_frame_bytes = *largest;
    }
    uint32_t allowance = s->max_frame_bytes * 2 + SS_INFLIGHT_HEADROOM;
    if (allowance < SS_INFLIGHT_MIN_BYTES) allowance = SS_INFLIGHT_MIN_BYTES;
    if (allowance > SS_INFLIGHT_MAX_BYTES) allowance = SS_INFLIGHT_MAX_BYTES;
    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
        SsClient *c = &s->clients[i];
        if (!c->in_use || !c->handshake_done) {
            continue;
        }
        if (codec_filter && c->codec != codec_filter) {
            continue;
        }

        if (flush_pending(c) != 0) {
            drop_client(s, i, "connection gone");
            continue;
        }
        /* What the kernel has accepted but not yet put on the wire. A
         * frame added on top of a backlog arrives late by definition, so
         * it is skipped and the next one takes its place -- the client
         * stays at the live edge rather than falling steadily further
         * behind it. */
        if (type == C2S_MSG_VIDEO) {
            int unsent = 0;
            if (ioctl(c->fd, TIOCOUTQ, &unsent) == 0 && unsent > (int)allowance) {
                skipped = 1;
                continue;
            }
        }

        if (c->pending_len) {
            /* Still catching up on the previous frame, so this one is
             * skipped -- but VP8 is predictive, and a gap leaves every
             * frame after it decoding against something the client never
             * received. The picture then stays broken until the next
             * keyframe, which at the usual interval is five seconds
             * away. Asking for one now is the difference between a
             * dropped frame and five seconds of smeared garbage. */
            skipped = 1;
            continue;
        }

        /* Header and payload are one message: a header whose payload
         * never follows would leave the client permanently out of step,
         * so the remainder is buffered rather than abandoned. */
        if (total > c->pending_cap) {
            uint8_t *bigger = realloc(c->pending, total);
            if (!bigger) {
                continue; /* skip this frame; the client stays */
            }
            c->pending = bigger;
            c->pending_cap = total;
        }
        memcpy(c->pending, &h, sizeof(h));
        if (size) {
            memcpy(c->pending + sizeof(h), data, size);
        }
        c->pending_len = total;
        c->pending_sent = 0;

        if (flush_pending(c) != 0) {
            drop_client(s, i, "connection gone");
        }
    }
    SDL_UnlockMutex(s->mutex);

    /* Outside the lock: the callback reaches into the pipeline, and
     * holding the client mutex across that would invite a deadlock with
     * the thread delivering encoded frames. */
    if (skipped && type == C2S_MSG_VIDEO) {
        uint32_t t = now_ms();
        /* Counted always, printed only when asked for.
         *
         * A link that is behind is behind for many frames in a row, so
         * this is a periodic writer by nature -- exactly the kind that
         * was just taken out of the rest of the program. The client
         * shows its own count of late frames on screen, which is where
         * anyone actually looking at this problem is looking. */
        s->skipped_frames++;
        if (app_verbose() && t - s->last_skip_report_ms >= SS_SKIP_REPORT_INTERVAL_MS) {
            s->last_skip_report_ms = t;
            fprintf(stderr, "switch_stream: %u frames skipped so far -- the client's link is "
                            "behind (allowance %u B, largest frame %u B)\n",
                    s->skipped_frames, allowance, s->max_frame_bytes);
        }
        if (s->keyframe_cb && t - s->last_keyframe_ms >= SS_KEYFRAME_MIN_INTERVAL_MS) {
            s->last_keyframe_ms = t;
            s->keyframe_cb(s->keyframe_ctx);
        }
    }
}

void switch_stream_send_video(SwitchStream *s, int codec, const uint8_t *data, uint32_t size,
                              int keyframe) {
    /* Only to the clients on that codec. The other group is watching a
     * different encode and would decode these bytes as their own. */
    broadcast(s, codec, C2S_MSG_VIDEO, keyframe ? C2S_FLAG_KEYFRAME : 0, data, size);
}

/* How many clients are watching one of the two codecs. The pipeline
 * asks, so a chain nobody is reading is not encoded at all. */
int switch_stream_codec_client_count(SwitchStream *s, int codec) {
    return s ? s->live[codec_slot(codec)] : 0;
}

void switch_stream_set_demand_changed(SwitchStream *s, void (*cb)(void *ctx), void *ctx) {
    if (s) { s->demand_cb = cb; s->demand_ctx = ctx; }
}

/* Outside the client mutex, always: the callback reaches into the
 * pipeline, which is the other side of a lock this thread may hold. */
static void note_demand_changed(SwitchStream *s) {
    if (s && s->demand_cb) s->demand_cb(s->demand_ctx);
}

void switch_stream_send_audio(SwitchStream *s, const uint8_t *data, uint32_t size) {
    /* One encode, everybody: sound has no codec groups. */
    broadcast(s, 0, C2S_MSG_AUDIO, 0, data, size);
}

void switch_stream_set_video_size(SwitchStream *s, uint16_t width, uint16_t height) {
    if (!s) return;
    SDL_LockMutex(s->mutex);
    s->width = width;
    s->height = height;
    SDL_UnlockMutex(s->mutex);
}

int switch_stream_max_clients(void) {
    return SS_MAX_CLIENTS;
}

int switch_stream_client_count(SwitchStream *s) {
    if (!s) return 0;
    SDL_LockMutex(s->mutex);
    int n = 0;
    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
        if (s->clients[i].in_use && s->clients[i].handshake_done) n++;
    }
    SDL_UnlockMutex(s->mutex);
    return n;
}

/* --- receiving ------------------------------------------------------ */

static void client_consume(SsClient *c, uint32_t bytes) {
    memmove(c->rx, c->rx + bytes, c->rx_len - bytes);
    c->rx_len -= bytes;
}

/* The handshake decides whether this client may drive the console. The
 * check is the same one the browser goes through, so there is one answer
 * to "who may control" rather than two that can disagree. */
static void handle_hello(SwitchStream *s, int index) {
    SsClient *c = &s->clients[index];
    if (c->rx_len < sizeof(C2sHello)) {
        return;
    }
    C2sHello hello;
    memcpy(&hello, c->rx, sizeof(hello));
    if (c->rx_len < sizeof(hello) + hello.token_len) {
        return; /* the token is still arriving */
    }

    char token[C2S_MAX_TOKEN_LEN + 1] = {0};
    if (hello.token_len) {
        uint8_t n = hello.token_len;
        if (n > C2S_MAX_TOKEN_LEN) n = C2S_MAX_TOKEN_LEN;
        memcpy(token, c->rx + sizeof(hello), n);
    }
    client_consume(c, sizeof(hello) + hello.token_len);

    C2sHelloAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic = C2S_MAGIC;
    ack.version = C2S_VERSION;

    if (hello.magic != C2S_MAGIC || hello.version != C2S_VERSION) {
        ack.accepted = 0;
        send_all_now(c->fd, &ack, sizeof(ack));
        drop_client(s, index, "protocol mismatch");
        return;
    }

    ack.accepted = 1;
    ack.may_control = web_stream_may_control(s->web, token) ? 1 : 0;
    /* H.264 to start with, and the client says otherwise if it wants to.
     * Both native clients decode H.264 in hardware and ask for it; VP8
     * as the opening codec meant every connection began by spinning up
     * a software encoder that was about to be abandoned. */
    c->codec = C2S_CODEC_H264;
    {
        const int slot = codec_slot(c->codec);
        ack.width = s->group_stream_known[slot] ? s->group_width[slot] : s->width;
        ack.height = s->group_stream_known[slot] ? s->group_height[slot] : s->height;
    }
    ack.video_codec = c->codec;
    ack.audio_codec = C2S_CODEC_OPUS;
    ack.audio_rate = 48000;
    ack.audio_channels = 2;

    if (send_all_now(c->fd, &ack, sizeof(ack)) != 0) {
        drop_client(s, index, "could not answer the handshake");
        return;
    }
    c->handshake_done = 1;
    c->may_control = ack.may_control;

    /* This client has seen no picture at all, so the next one has to be
     * a keyframe -- otherwise it decodes against frames that went out
     * before it arrived and shows garbage until the interval elapses. */
    s->keyframe_pending = 1;

    /* Before it can ask for anything: what the room is already doing.
     * A client told this has no reason to push its own saved settings,
     * which is what used to change the picture for everyone the moment
     * somebody else joined. */
    send_group_state(s, index);

    /* One more client on this codec -- and possibly the first, which is
     * what starts the chain encoding at all. */
    recount(s);

    fprintf(stderr, "switch_stream: client %d connected as %s, on %s\n", index,
            c->may_control ? "PLAYER" : "viewer",
            c->codec == C2S_CODEC_H264 ? "h264" : "vp8");
}

/* Recomputes who is watching what. Called with the lock held, whenever
 * a client arrives, leaves, or changes codec -- the three things that
 * can turn a chain on or off. */
static void recount(SwitchStream *s) {
    int n[2] = {0, 0};
    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
        const SsClient *c = &s->clients[i];
        if (c->in_use && c->handshake_done) n[codec_slot(c->codec)]++;
    }
    if (n[0] != s->live[0] || n[1] != s->live[1]) {
        s->live[0] = n[0];
        s->live[1] = n[1];
        /* Not called from here: the pipeline is on the other side of a
         * lock this thread is holding. The poll loop fires it once it
         * has let go. */
        s->demand_dirty = 1;
    }
}

/* Tells one client the shape and the settings of the stream it is on.
 *
 * Sent when it connects and again whenever it changes codec, because
 * both are moments where it is about to receive an encode it knows
 * nothing about. */
static void send_group_state(SwitchStream *s, int index) {
    SsClient *c = &s->clients[index];
    const int slot = codec_slot(c->codec);

    if (s->group_stream_known[slot]) {
        C2sStreamInfo info;
        memset(&info, 0, sizeof(info));
        info.width = s->group_width[slot];
        info.height = s->group_height[slot];
        info.video_codec = c->codec;
        C2sFrameHeader h = {.type = C2S_MSG_STREAM_INFO, .flags = 0, .reserved = 0,
                            .size = sizeof(info)};
        if (send_all_now(c->fd, &h, sizeof(h)) == 0) {
            send_all_now(c->fd, &info, sizeof(info));
        }
    }
    if (s->shared_known[slot]) {
        C2sFrameHeader h = {.type = C2S_MSG_SHARED, .flags = 0, .reserved = 0,
                            .size = sizeof(C2sShared)};
        if (send_all_now(c->fd, &h, sizeof(h)) == 0) {
            send_all_now(c->fd, &s->shared[slot], sizeof(s->shared[slot]));
        }
    }
}

static void handle_messages(SwitchStream *s, int index) {
    SsClient *c = &s->clients[index];
    int codec_changed = 0;
    for (;;) {
        if (c->rx_len < sizeof(C2sFrameHeader)) {
            return;
        }
        C2sFrameHeader h;
        memcpy(&h, c->rx, sizeof(h));
        if (h.size > SS_RX_CAPACITY - sizeof(h)) {
            drop_client(s, index, "oversized message");
            return;
        }
        if (c->rx_len < sizeof(h) + h.size) {
            return;
        }
        const uint8_t *payload = c->rx + sizeof(h);

        switch (h.type) {
            case C2S_MSG_INPUT:
                /* THE access-control point for this transport, exactly
                 * as on_gamepad_message() is for the browser's: a viewer's
                 * input is dropped here, before it can reach the console.
                 * Anything client-side is a convenience, not a gate. */
                if (c->may_control && h.size == C2S_PAD_SLOTS) {
                    int8_t state[C2S_PAD_SLOTS];
                    memcpy(state, payload, sizeof(state));
                    gamepad_bridge_update(GAMEPAD_SOURCE_NATIVE(index), state);
                }
                break;
            case C2S_MSG_HOME:
                if (c->may_control) {
                    gamepad_bridge_press_home();
                }
                break;
            case C2S_MSG_PROFILE:
                /* Players only, like the browser's /quality and
                 * /resolution: one encoder feeds this client's whole
                 * codec group, so it is not a per-viewer preference --
                 * a viewer asking for 360p would set it for the person
                 * playing. It stops at the group, though: the other
                 * codec's viewers are watching a different encode. */
                if (c->may_control && h.size == sizeof(C2sProfile)) {
                    C2sProfile p;
                    memcpy(&p, payload, sizeof(p));
                    fprintf(stderr, "switch_stream: client %d (%s) asks for %ux%u@%u, %u kbps\n",
                            index, c->codec == C2S_CODEC_H264 ? "h264" : "vp8",
                            p.width, p.height, p.fps, p.bitrate_kbps);
                    if (s->profile_cb) {
                        s->profile_cb(s->profile_ctx, c->codec, p.width, p.height, p.fps,
                                      p.bitrate_kbps);
                    }
                }
                break;
            case C2S_MSG_CODEC:
                /* This client's own choice, and no longer players-only:
                 * it changes what THIS connection is sent and nothing
                 * else. It used to move every native client at once,
                 * which is why it was gated -- and why a phone that
                 * wanted H.264 could not have it while somebody else
                 * was on VP8. */
                if (h.size == 1
                    && (payload[0] == C2S_CODEC_VP8 || payload[0] == C2S_CODEC_H264)
                    && c->codec != payload[0]) {
                    c->codec = payload[0];
                    fprintf(stderr, "switch_stream: client %d now on %s\n", index,
                            c->codec == C2S_CODEC_H264 ? "h264" : "vp8");
                    /* Its decoder has to be rebuilt for the other
                     * encode, and the first thing it must see there is
                     * a keyframe -- everything else predicts from
                     * pictures it never received. */
                    send_group_state(s, index);
                    s->keyframe_pending = 1;
                    codec_changed = 1;
                }
                break;
            case C2S_MSG_KEYFRAME:
                /* The client dropped a backlog and resumed mid-stream.
                 * Rate-limited like every other request for one: a
                 * client that keeps falling behind must not be answered
                 * with the largest frames there are. */
                if (now_ms() - s->last_keyframe_ms >= SS_KEYFRAME_MIN_INTERVAL_MS) {
                    s->keyframe_pending = 1;
                }
                break;
            case C2S_MSG_WAKE:
                /* Players only, exactly as POST /wake is: this switches
                 * mains power to a console. */
                if (c->may_control && s->web) {
                    fprintf(stderr, "switch_stream: client %d asks to wake the console\n", index);
                    web_stream_wake_console(s->web);
                }
                break;
            case C2S_MSG_RESTART:
                if (c->may_control) {
                    fprintf(stderr, "switch_stream: client %d asks for a restart\n", index);
                    app_request_restart();
                }
                break;
            case C2S_MSG_RESET_DONGLE:
                if (c->may_control) {
                    fprintf(stderr, "switch_stream: client %d asks for an adapter reset\n", index);
                    gamepad_bridge_reset();
                }
                break;
            case C2S_MSG_PING:
                break;
            default:
                break;
        }
        client_consume(c, sizeof(h) + h.size);
        if (codec_changed) {
            codec_changed = 0;
            recount(s);
        }
    }
}

/* --- the accept/poll thread ----------------------------------------- */

static int accept_thread(void *arg) {
    SwitchStream *s = arg;

    while (s->running) {
        struct pollfd pfds[SS_MAX_CLIENTS + 1];
        int map[SS_MAX_CLIENTS + 1];
        int n = 0;

        pfds[n].fd = s->listen_fd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        map[n] = -1;
        n++;

        SDL_LockMutex(s->mutex);
        for (int i = 0; i < SS_MAX_CLIENTS; i++) {
            if (s->clients[i].in_use) {
                pfds[n].fd = s->clients[i].fd;
                pfds[n].events = POLLIN;
                pfds[n].revents = 0;
                map[n] = i;
                n++;
            }
        }
        SDL_UnlockMutex(s->mutex);

        if (poll(pfds, n, 200) <= 0) {
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            int fd = accept(s->listen_fd, NULL, NULL);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                int sndbuf = SS_SOCKET_SNDBUF;
                setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

                SDL_LockMutex(s->mutex);
                int slot = -1;
                for (int i = 0; i < SS_MAX_CLIENTS; i++) {
                    if (!s->clients[i].in_use) { slot = i; break; }
                }
                if (slot < 0) {
                    close(fd);
                    fprintf(stderr, "switch_stream: refusing a client, all %d slots in use\n",
                            SS_MAX_CLIENTS);
                } else {
                    memset(&s->clients[slot], 0, sizeof(s->clients[slot]));
                    s->clients[slot].fd = fd;
                    s->clients[slot].in_use = 1;
                    s->clients[slot].last_seen_ms = now_ms();
                }
                SDL_UnlockMutex(s->mutex);
            }
        }

        SDL_LockMutex(s->mutex);
        for (int p = 1; p < n; p++) {
            int i = map[p];
            if (i < 0 || !s->clients[i].in_use) continue;
            SsClient *c = &s->clients[i];

            if (pfds[p].revents & (POLLHUP | POLLERR)) {
                drop_client(s, i, "socket closed");
                continue;
            }
            if (!(pfds[p].revents & POLLIN)) {
                if (now_ms() - c->last_seen_ms > SS_IDLE_TIMEOUT_MS) {
                    drop_client(s, i, "silent too long");
                }
                continue;
            }

            ssize_t got = recv(c->fd, c->rx + c->rx_len, SS_RX_CAPACITY - c->rx_len, 0);
            if (got <= 0) {
                if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    drop_client(s, i, got == 0 ? "closed" : "receive failed");
                }
                continue;
            }
            c->rx_len += (uint32_t)got;
            c->last_seen_ms = now_ms();

            if (!c->handshake_done) {
                handle_hello(s, i);
            }
            if (s->clients[i].in_use && s->clients[i].handshake_done) {
                handle_messages(s, i);
            }
        }
        SDL_UnlockMutex(s->mutex);

        /* Outside the lock, for the same reason the keyframe request
         * below is: this reaches into the pipeline. */
        if (s->demand_dirty) {
            s->demand_dirty = 0;
            note_demand_changed(s);
        }

        if (s->keyframe_pending) {
            s->keyframe_pending = 0;
            if (s->keyframe_cb) {
                s->last_keyframe_ms = now_ms();
                s->keyframe_cb(s->keyframe_ctx);
            }
        }
    }
    return 0;
}

SwitchStream *switch_stream_start(WebStream *ws, uint16_t port) {
    SwitchStream *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->web = ws;
    s->port = port ? port : C2S_DEFAULT_PORT;
    s->video_codec = C2S_CODEC_VP8;
    s->width = 1280;
    s->height = 720;
    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
        s->clients[i].fd = -1;
    }

    s->mutex = SDL_CreateMutex();
    if (!s->mutex) {
        free(s);
        return NULL;
    }

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        perror("switch_stream: socket");
        SDL_DestroyMutex(s->mutex);
        free(s);
        return NULL;
    }
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(s->port);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s->listen_fd, SS_MAX_CLIENTS) != 0) {
        fprintf(stderr, "switch_stream: cannot listen on port %u: %s\n", s->port, strerror(errno));
        close(s->listen_fd);
        SDL_DestroyMutex(s->mutex);
        free(s);
        return NULL;
    }

    s->running = 1;
    s->thread = SDL_CreateThread(accept_thread, "switch-stream", s);
    if (!s->thread) {
        close(s->listen_fd);
        SDL_DestroyMutex(s->mutex);
        free(s);
        return NULL;
    }

    fprintf(stderr, "switch_stream: listening on port %u\n", s->port);
    return s;
}

void switch_stream_stop(SwitchStream *s) {
    if (!s) {
        return;
    }
    s->running = 0;
    SDL_WaitThread(s->thread, NULL);

    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
        if (s->clients[i].in_use) {
            close(s->clients[i].fd);
        }
    }
    close(s->listen_fd);
    SDL_DestroyMutex(s->mutex);
    free(s);
}
