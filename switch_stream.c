#include "switch_stream.h"

#include "ws_frame.h"

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
/*
 * Three streams, not two codecs.
 *
 * The routing key used to be the codec, because the only two audiences
 * were the native VP8 clients and the native H.264 ones. A browser
 * arriving over a WebSocket is a third: it wants H.264 like the console
 * does, but at the size a monitor wants rather than the size a handheld
 * wants, so it cannot share that encode. Naming the slots after the
 * streams rather than the codecs is what lets a third exist at all.
 *
 * A client belongs to exactly one, for its whole life.
 */
/* SS_STREAM_VP8, _H264, _WEB and _DRC are in the header: the pipeline
 * routes by them too. What they mean:
 *
 *   VP8   native, VP8
 *   H264  native, H.264, the size a handheld wants
 *   WEB   browsers over the WebSocket, H.264, the size a monitor wants
 *//*
 * And a fourth, for a real Wii U GamePad.
 *
 * It cannot share the console's H.264 for a better reason than size:
 * the pad's decoder cannot read ordinary H.264 at all. It needs DRH
 * slicing -- macroblock rows, no slice header -- which only drc-x264
 * produces, at 864x480 and a quantiser of 32, none of it negotiable.
 * Nothing else can be on this stream and it can be on nothing else.
 */
#define SS_STREAM_COUNT 5

/*
 * Four was sized for native clients, back when a browser could not
 * reach this transport at all. A browser can now, and a reloaded page
 * holds a slot until it is noticed gone -- so four was a server that
 * refused the fourth reload of the afternoon.
 *
 * Eight, not more. The encoders are shared, so a client costs bandwidth
 * rather than a core, but every one of them is a full stream going out
 * of this machine and there is no point pretending otherwise.
 */
#define SS_MAX_CLIENTS 8

/*
 * And at most this many of them may be browsers.
 *
 * Sharing one pool was wrong, and it broke the thing that matters most:
 * a page that reconnects in a loop filled all eight slots, and the
 * console and the phone -- which have nowhere else to go -- were then
 * refused with "the host closed the connection". A browser has another
 * transport available and a native client does not, so when the two
 * compete the browser is the one that waits.
 */
#define SS_MAX_WS_CLIENTS 4

/* A client that has said nothing for this long is gone, whatever the
 * socket thinks. Every client pings -- the page included, which is not
 * optional: a browser that is only watching sends nothing at all, so
 * without a ping it would be dropped and reconnected every ten seconds
 * for as long as somebody watched it. */
#define SS_IDLE_TIMEOUT_MS 10000
/*
 * How long a client may be quiet before we ASK whether it is still
 * there, rather than concluding that it is not.
 *
 * The timeout above counts bytes received, and a native client's only
 * unprompted traffic is input -- which it sends when something MOVED,
 * because the host applies a state rather than a change. So a pad
 * resting on a table for ten seconds looked exactly like a pad that had
 * crashed, and the session was dropped.
 *
 * Measured in /dev/shm/capture2cloud.log with three clients connected:
 * every single freeze recovery ended in "silent too long" -- the pad is
 * away reassociating, so nothing moves, so nothing is sent. A stutter
 * the client could have recovered from became a whole session restart
 * twelve times in twelve minutes.
 *
 * A ping costs five bytes and the clients already answer it. Only a
 * client that answers nothing is now reaped.
 */
#define SS_IDLE_PROBE_MS    3000

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

/*
 * Audio is never intentionally sacrificed just because a video message
 * is partially written. Bound the exceptional backlog anyway: a client
 * that is this far behind is better reconnected than given seconds of
 * stale sound.
 */
#define SS_AUDIO_PENDING_MAX_BYTES 262144

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
    /* Arrived through the HTTP server's WebSocket upgrade rather than
     * on this transport's own port. Above the handshake the two are the
     * same stream of C2S messages; the difference is one frame header
     * per message, and which encode it is fed. */
    int is_ws;
    int on_drc_port;   /* arrived on the GamePad's own port */
    int on_wiiu_port;  /* arrived on the Wii U console's own port */

    /* This client explicitly negotiated raw S16LE audio. */
    int pcm_audio;
    int pcm_udp;

    /*
     * Native peer IPv4 in network byte order.
     *
     * If the same Wii U reconnects after an RPX crash/relaunch, its
     * new socket supersedes the old half-dead one immediately.
     */
    uint32_t peer_ipv4;

    uint32_t last_seen_ms;
    uint32_t last_probe_ms;   /* when we last asked a quiet client if it is there */
    uint8_t rx[SS_RX_CAPACITY];
    uint32_t rx_len;

    /* For a browser only: what came OUT of the WebSocket frames, which
     * is what the C2S parser reads. Two buffers rather than unwrapping
     * in place, because a partial frame at the end of rx would otherwise
     * sit in the middle of bytes already unwrapped, and telling the two
     * apart is more state than a second small buffer. */
    uint8_t *ws_rx;
    uint32_t ws_rx_len, ws_rx_cap;

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
    int drc_listen_fd;   /* the GamePad's own port */
    int wiiu_listen_fd;  /* the Wii U console's own TCP port */

    /*
     * Connectionless PCM output to Wii U consoles.
     *
     * The TCP connection already authenticates/identifies the console
     * and gives us its peer IPv4 address; audio itself does not need
     * another handshake.
     */
    int audio_udp_fd;
    uint32_t pcm_sequence;

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
    volatile int live[SS_STREAM_COUNT];
    volatile int demand_dirty;
    /* Whether the GamePad's encode can be offered at all. */
    volatile int drc_available;

    uint32_t max_frame_bytes_by_slot[SS_STREAM_COUNT];
    C2sShared shared[SS_STREAM_COUNT];
    int shared_known[SS_STREAM_COUNT];
    uint16_t group_width[SS_STREAM_COUNT], group_height[SS_STREAM_COUNT];
    int group_stream_known[SS_STREAM_COUNT];

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
    void (*profile_cb)(void *ctx, int slot, int w, int h, int fps, int bitrate_kbps);
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
/* Which stream a native client asking for this codec belongs to.
 * Anything else is not a codec this transport carries. */
static int codec_slot(int codec) {
    if (codec == C2S_CODEC_DRC_H264) return SS_STREAM_DRC;
    return codec == C2S_CODEC_H264 ? SS_STREAM_H264 : SS_STREAM_VP8;
}

/* And the one a client is actually on, which for a browser is decided
 * by how it connected rather than by what it asked for. */
static int client_slot(const SsClient *c) {
    if (c->is_ws) return SS_STREAM_WEB;
    /* A pad stays on its own stream whatever codec it asks for. The
     * port it arrived on is the decision; the codec only says which of
     * the two encodes that stream should carry. Letting the codec move
     * it put the pad back on the console's chain, where a handheld
     * asking for 480p30 took the pad down to 30 with it. */
    if (c->on_drc_port) return SS_STREAM_DRC;
    /* Same rule, same reason: a console on a television wants
     * 720p60 and a phone on 5081 may not. */
    if (c->on_wiiu_port) return SS_STREAM_WIIU;
    return codec_slot(c->codec);
}

/* What a stream is encoded in. The browsers' stream and the console's
 * are both H.264; they differ in size, not in codec. */
static uint8_t slot_codec(int slot) {
    if (slot == SS_STREAM_VP8) return C2S_CODEC_VP8;
    if (slot == SS_STREAM_DRC) return C2S_CODEC_DRC_H264;
    return C2S_CODEC_H264;
}
/* slot_filter < 0 means every client; otherwise only those on that
 * stream. Video is always filtered -- handing a client another
 * encode's bytes produces a picture, and the picture is bright pink. */
static void broadcast_filtered(SwitchStream *s, int slot_filter,
                               uint8_t type, uint8_t flags,
                               const uint8_t *data, uint32_t size,
                               int pcm_filter);

static void broadcast(SwitchStream *s, int slot_filter,
                      uint8_t type, uint8_t flags,
                      const uint8_t *data, uint32_t size);
static void send_group_state(SwitchStream *s, int index);
static int send_msg_now(SsClient *c, uint8_t type, const void *data, uint32_t size);
static void recount(SwitchStream *s);

void switch_stream_set_keyframe_request(SwitchStream *s, SwitchKeyframeRequest cb, void *ctx) {
    if (s) { s->keyframe_cb = cb; s->keyframe_ctx = ctx; }
}

/* Announces the shape of ONE of the two streams, to the clients on it.
 *
 * The codec is no longer a property of the server, so neither is this:
 * telling a VP8 client that the stream is now 720p H.264 would make it
 * rebuild its decoder for an encode it is not being sent. */
void switch_stream_announce_stream(SwitchStream *s, int slot,
                                   uint16_t width, uint16_t height) {
    if (!s || slot < 0 || slot >= SS_STREAM_COUNT) return;
    C2sStreamInfo info;
    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    info.video_codec = slot_codec(slot);
    s->group_width[slot] = width;
    s->group_height[slot] = height;
    s->group_stream_known[slot] = 1;
    broadcast(s, slot, C2S_MSG_STREAM_INFO, 0, (const uint8_t *)&info, sizeof(info));
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
void switch_stream_announce_shared(SwitchStream *s, int slot, uint16_t width, uint16_t height,
                                   uint16_t fps, uint16_t bitrate_kbps,
                                   uint8_t capture_mjpeg) {
    if (!s || slot < 0 || slot >= SS_STREAM_COUNT) return;
    const uint8_t codec = slot_codec(slot);
    C2sShared sh;
    memset(&sh, 0, sizeof(sh));
    sh.width = width;
    sh.height = height;
    sh.fps = fps;
    sh.bitrate_kbps = bitrate_kbps;
    sh.video_codec = codec;
    sh.capture_mjpeg = capture_mjpeg;
    s->shared[slot] = sh;
    s->shared_known[slot] = 1;
    broadcast(s, slot, C2S_MSG_SHARED, 0, (const uint8_t *)&sh, sizeof(sh));
}

void switch_stream_set_profile_request(SwitchStream *s,
                                       void (*cb)(void *ctx, int slot, int w, int h, int fps,
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
    free(s->clients[i].ws_rx);
    memset(&s->clients[i], 0, sizeof(s->clients[i]));
    s->clients[i].fd = -1;
    /* If that was the last client on its codec, the chain it was
     * watching has nobody left and stops being encoded. */
    recount(s);
}

static void broadcast_filtered(SwitchStream *s, int slot_filter,
                               uint8_t type, uint8_t flags,
                               const uint8_t *data, uint32_t size,
                               int pcm_filter) {
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
        /* Per stream: a 1080p browser keyframe, a 6 Mb/s H.264 one and a
         * 1 Mb/s VP8 one are not the same size, and one allowance for
         * all three would size the small stream's backlog for the large
         * stream's frames. */
        uint32_t *largest = &s->max_frame_bytes_by_slot[slot_filter];
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
        if (slot_filter >= 0 && client_slot(c) != slot_filter) {
            continue;
        }

        /*
         * -1 = no audio-format filter
         *  0 = Opus
         *  1 = all PCM
         *  2 = PCM/TCP fallback only
         */
        if (pcm_filter == 0 &&
            c->pcm_audio) {
            continue;
        }

        if (pcm_filter == 1 &&
            !c->pcm_audio) {
            continue;
        }

        if (pcm_filter == 2 &&
            (!c->pcm_audio ||
             c->pcm_udp)) {
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

            if (ioctl(c->fd, TIOCOUTQ, &unsent) == 0 &&
                unsent > (int)allowance) {
                skipped = 1;
                continue;
            }
        }

        uint32_t pending_prefix = 0;

        if (c->pending_len) {
            /*
             * Video remains expendable: staying at the live edge matters
             * more than delivering a stale picture.
             *
             * Audio is different. A missing PCM packet literally removes
             * 5 ms of waveform, so append it behind the bytes already in
             * flight instead of intentionally creating a hole.
             */
            if (type != C2S_MSG_AUDIO) {
                skipped = 1;
                continue;
            }

            if (c->pending_sent) {
                const uint32_t remain =
                    c->pending_len -
                    c->pending_sent;

                memmove(c->pending,
                        c->pending + c->pending_sent,
                        remain);

                c->pending_len = remain;
                c->pending_sent = 0;
            }

            pending_prefix =
                c->pending_len;
        }

        /* Header and payload are one message: a header whose payload
         * never follows would leave the client permanently out of step,
         * so the remainder is buffered rather than abandoned. */
        /* A browser reads WebSocket frames, so one goes in front of the
         * message. Nothing else about the bytes changes: above the
         * handshake the page reads exactly what the console reads. */
        uint8_t wsh[10];
        const size_t wsh_len = c->is_ws ? ws_binary_header(total, wsh) : 0;
        const uint32_t on_wire =
            total +
            (uint32_t)wsh_len;

        const uint32_t needed =
            pending_prefix +
            on_wire;

        if (type == C2S_MSG_AUDIO &&
            needed > SS_AUDIO_PENDING_MAX_BYTES) {

            drop_client(
                s,
                i,
                "audio backlog");

            continue;
        }

        if (needed > c->pending_cap) {
            uint8_t *bigger =
                realloc(
                    c->pending,
                    needed);

            if (!bigger) {
                continue;
            }

            c->pending =
                bigger;

            c->pending_cap =
                needed;
        }

        uint8_t *dst =
            c->pending +
            pending_prefix;

        if (wsh_len) {
            memcpy(
                dst,
                wsh,
                wsh_len);
        }

        memcpy(
            dst + wsh_len,
            &h,
            sizeof(h));

        if (size) {
            memcpy(
                dst +
                    wsh_len +
                    sizeof(h),
                data,
                size);
        }

        c->pending_len =
            needed;

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

static void broadcast(SwitchStream *s, int slot_filter,
                      uint8_t type, uint8_t flags,
                      const uint8_t *data, uint32_t size)
{
    broadcast_filtered(
        s,
        slot_filter,
        type,
        flags,
        data,
        size,
        -1);
}


void switch_stream_send_video(SwitchStream *s, int slot, const uint8_t *data, uint32_t size,
                              int keyframe) {
    /* Only to the clients on that stream. The others are watching a
     * different encode and would decode these bytes as their own. */
    broadcast(s, slot, C2S_MSG_VIDEO, keyframe ? C2S_FLAG_KEYFRAME : 0, data, size);
}

/* How many clients are watching one of the two codecs. The pipeline
 * asks, so a chain nobody is reading is not encoded at all. */
/*
 * Takes over a connection the HTTP server has already upgraded.
 *
 * The WebSocket handshake IS the greeting: by the time this is called
 * the browser has said who it is and been answered, so there is no
 * C2sHello to wait for. What it still needs is the ack -- the size, the
 * codec, whether it may drive the console -- and it gets it as the
 * first binary frame, in exactly the layout a native client reads.
 *
 * Returns 0 once the connection belongs to this transport, which then
 * owns the fd and will close it. Non-zero means it was refused and the
 * caller still owns it.
 */
int switch_stream_adopt_websocket(SwitchStream *s, int fd, int may_control) {
    if (!s || fd < 0) return -1;

    SDL_LockMutex(s->mutex);
    int index = -1, browsers = 0;
    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
        if (s->clients[i].in_use && s->clients[i].is_ws) browsers++;
        if (index < 0 && !s->clients[i].in_use) index = i;
    }
    if (index < 0 || browsers >= SS_MAX_WS_CLIENTS) {
        SDL_UnlockMutex(s->mutex);
        fprintf(stderr, "switch_stream: a browser was refused (%d already, %d slots free) -- "
                        "the console and the phone come first\n",
                browsers, index < 0 ? 0 : 1);
        return -1;
    }

    SsClient *c = &s->clients[index];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->in_use = 1;
    c->is_ws = 1;
    c->handshake_done = 1;
    c->may_control = may_control ? 1 : 0;
    c->codec = C2S_CODEC_H264;   /* the browsers' stream is H.264 */
    c->last_seen_ms = now_ms();
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int sndbuf = SS_SOCKET_SNDBUF;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }

    const int slot = SS_STREAM_WEB;
    C2sHelloAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic = C2S_MAGIC;
    ack.version = C2S_VERSION;
    ack.accepted = 1;
    ack.may_control = c->may_control;
    ack.width = s->group_stream_known[slot] ? s->group_width[slot] : s->width;
    ack.height = s->group_stream_known[slot] ? s->group_height[slot] : s->height;
    ack.video_codec = C2S_CODEC_H264;
    ack.audio_codec = C2S_CODEC_OPUS;
    ack.audio_rate = 48000;
    ack.audio_channels = 2;

    /* Sent as a message like any other, so the page's reader has one
     * shape to handle rather than a special first frame. */
    const int sent = send_msg_now(c, C2S_MSG_HELLO_ACK, &ack, sizeof(ack));
    if (sent != 0) {
        memset(c, 0, sizeof(*c));
        c->fd = -1;
        SDL_UnlockMutex(s->mutex);
        return -1;
    }

    send_group_state(s, index);
    /* It has seen no picture, so the next one has to be a keyframe. */
    s->keyframe_pending = 1;
    recount(s);
    SDL_UnlockMutex(s->mutex);

    fprintf(stderr, "switch_stream: a browser connected as %s, on the web stream\n",
            c->may_control ? "PLAYER" : "viewer");
    return 0;
}

int switch_stream_stream_client_count(SwitchStream *s, int slot) {
    if (!s || slot < 0 || slot >= SS_STREAM_COUNT) return 0;
    return s->live[slot];
}

void switch_stream_set_demand_changed(SwitchStream *s, void (*cb)(void *ctx), void *ctx) {
    if (s) { s->demand_cb = cb; s->demand_ctx = ctx; }
}

/* Outside the client mutex, always: the callback reaches into the
 * pipeline, which is the other side of a lock this thread may hold. */
static void note_demand_changed(SwitchStream *s) {
    if (s && s->demand_cb) s->demand_cb(s->demand_ctx);
}

/* Which encode the clients on a stream are asking for. The GamePad's
 * stream can carry either: drc-x264 chunks it hands straight to its
 * radio, or ordinary H.264 it decodes and encodes again. */
int switch_stream_stream_codec(SwitchStream *s, int slot) {
    if (!s || slot < 0 || slot >= SS_STREAM_COUNT) return 0;
    int codec = 0;
    SDL_LockMutex(s->mutex);
    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
        if (s->clients[i].in_use && s->clients[i].handshake_done &&
            client_slot(&s->clients[i]) == slot) {
            codec = s->clients[i].codec;
            break;
        }
    }
    SDL_UnlockMutex(s->mutex);
    return codec;
}

void switch_stream_set_drc_available(SwitchStream *s, int available) {
    if (!s) return;
    s->drc_available = available ? 1 : 0;
}

void switch_stream_send_audio(SwitchStream *s,
                              const uint8_t *data,
                              uint32_t size)
{
    /*
     * Opus goes to every legacy/non-PCM client, including browsers,
     * Switch and GamePad clients.
     */
    broadcast_filtered(
        s,
        -1,
        C2S_MSG_AUDIO,
        0,
        data,
        size,
        0);
}


void switch_stream_send_audio_pcm(SwitchStream *s,
                                  const uint8_t *data,
                                  uint32_t size)
{
    if (!s ||
        !data ||
        !size ||
        (size & 3u) != 0) {
        return;
    }

    /*
     * Compatibility fallback for PCM clients without UDP support.
     */
    broadcast_filtered(
        s,
        SS_STREAM_WIIU,
        C2S_MSG_AUDIO,
        0,
        data,
        size,
        2);

    if (s->audio_udp_fd < 0) {
        return;
    }

    const uint32_t frames =
        size / 4u;

    if (!frames ||
        frames > C2S_PCM_UDP_MAX_FRAMES) {
        return;
    }

    uint32_t peers[SS_MAX_CLIENTS];
    int peer_count = 0;
    uint32_t sequence = 0;

    SDL_LockMutex(s->mutex);

    for (int i = 0;
         i < SS_MAX_CLIENTS;
         ++i) {

        const SsClient *c =
            &s->clients[i];

        if (c->in_use &&
            c->handshake_done &&
            c->on_wiiu_port &&
            c->pcm_audio &&
            c->pcm_udp &&
            c->peer_ipv4 != 0) {

            peers[peer_count++] =
                c->peer_ipv4;
        }
    }

    if (peer_count) {
        sequence =
            s->pcm_sequence++;
    }

    SDL_UnlockMutex(s->mutex);

    if (!peer_count) {
        return;
    }

    uint8_t packet[
        sizeof(C2sPcmUdpHeader) +
        C2S_PCM_UDP_MAX_FRAMES * 4u];

    C2sPcmUdpHeader h;

    h.magic =
        c2s_le32(C2S_PCM_UDP_MAGIC);

    h.sequence =
        c2s_le32(sequence);

    h.frames =
        c2s_le16((uint16_t)frames);

    h.reserved =
        c2s_le16(0);

    memcpy(packet, &h, sizeof(h));
    memcpy(packet + sizeof(h), data, size);

    const size_t packet_size =
        sizeof(h) + size;

    for (int i = 0;
         i < peer_count;
         ++i) {

        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));

        dst.sin_family =
            AF_INET;

        dst.sin_addr.s_addr =
            peers[i];

        dst.sin_port =
            htons(C2S_WIIU_AUDIO_PORT);

        (void)sendto(
            s->audio_udp_fd,
            packet,
            packet_size,
            MSG_DONTWAIT | MSG_NOSIGNAL,
            (struct sockaddr *)&dst,
            sizeof(dst));
    }
}


int switch_stream_opus_audio_client_count(SwitchStream *s)
{
    if (!s) {
        return 0;
    }

    int count = 0;

    SDL_LockMutex(s->mutex);

    for (int i = 0;
         i < SS_MAX_CLIENTS;
         ++i) {

        const SsClient *c =
            &s->clients[i];

        if (c->in_use &&
            c->handshake_done &&
            !c->pcm_audio) {
            count++;
        }
    }

    SDL_UnlockMutex(s->mutex);

    return count;
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
    /*
     * A pad is a player, always, and no password is involved.
     *
     * It did not arrive over the network: it came in on its own port,
     * from a radio this machine runs, having paired with this machine.
     * Somebody holding it is already somebody standing in the room. A
     * password would protect nothing and would mean putting one where
     * a program with no keyboard could read it.
     *
     * The codec is set at accept time from which port the connection
     * came in on, so this is that decision and not a second one.
     */
    ack.may_control = (c->codec == C2S_CODEC_DRC_H264)
                          ? 1
                          : (web_stream_may_control(s->web, token) ? 1 : 0);
    /* H.264 to start with, and the client says otherwise if it wants to.
     * Both native clients decode H.264 in hardware and ask for it; VP8
     * as the opening codec meant every connection began by spinning up
     * a software encoder that was about to be abandoned.
     *
     * Unless the door already decided. A connection accepted on the
     * GamePad's own port is on that stream before a byte of its
     * handshake is read, and overwriting it here put the client on
     * ordinary H.264 for as long as it took to ask -- which it then
     * did, reporting a stream that "is not what this client asked
     * for" twice on the way to being right. */
    if (c->codec != C2S_CODEC_DRC_H264) {
        c->codec = C2S_CODEC_H264;
    }
    {
        /*
         * The port decides the stream before the handshake.
         *
         * A Wii U console uses SS_STREAM_WIIU even though its codec is
         * ordinary H.264; using codec_slot() here accidentally announced
         * the phone/Switch H.264 profile in the initial ACK.
         */
        const int slot = client_slot(c);
        ack.width = s->group_stream_known[slot] ? s->group_width[slot] : s->width;
        ack.height = s->group_stream_known[slot] ? s->group_height[slot] : s->height;
    }
    /*
     * Legacy clients leave C2sHello.reserved at zero and therefore keep
     * receiving Opus exactly as before.
     *
     * Only the Wii U console currently advertises raw PCM support.
     */
    const uint16_t audio_caps =
        c2s_le16(hello.reserved);

    c->pcm_audio =
        c->on_wiiu_port &&
        ((audio_caps &
          C2S_HELLO_CAP_PCM_S16LE) != 0);

    c->pcm_udp =
        c->pcm_audio &&
        s->audio_udp_fd >= 0 &&
        ((audio_caps &
          C2S_HELLO_CAP_PCM_UDP) != 0);

    ack.video_codec = c->codec;

    ack.audio_codec =
        c->pcm_audio
            ? C2S_CODEC_PCM_S16LE
            : C2S_CODEC_OPUS;

    if (c->pcm_udp) {
        ack.reserved |=
            C2S_ACK_FLAG_PCM_UDP;
    }

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
            c->on_wiiu_port ? "wii u console" :
            c->on_drc_port ? "wii u gamepad" :
            c->codec == C2S_CODEC_H264 ? "h264" : "vp8");
}

/* Recomputes who is watching what. Called with the lock held, whenever
 * a client arrives, leaves, or changes codec -- the three things that
 * can turn a chain on or off. */
static void recount(SwitchStream *s) {
    int n[SS_STREAM_COUNT] = {0};
    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
        const SsClient *c = &s->clients[i];
        if (c->in_use && c->handshake_done) n[client_slot(c)]++;
    }
    int changed = 0;
    for (int k = 0; k < SS_STREAM_COUNT; k++) {
        if (n[k] != s->live[k]) changed = 1;
    }
    if (changed) {
        for (int k = 0; k < SS_STREAM_COUNT; k++) s->live[k] = n[k];
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
/* One message, sent now and whole, wrapped for a browser if that is
 * what this client is. Used for the handshake and for the two
 * announcements that follow it, where blocking briefly is fine and a
 * partial write would leave the client out of step. */
static int send_msg_now(SsClient *c, uint8_t type, const void *data, uint32_t size) {
    C2sFrameHeader h = {.type = type, .flags = 0, .reserved = 0, .size = size};
    uint8_t buf[10 + sizeof(h) + 256];
    if (size > sizeof(buf) - 10 - sizeof(h)) {
        return -1;
    }
    size_t at = 0;
    if (c->is_ws) {
        at += ws_binary_header(sizeof(h) + size, buf);
    }
    memcpy(buf + at, &h, sizeof(h));
    at += sizeof(h);
    if (size) {
        memcpy(buf + at, data, size);
        at += size;
    }
    return send_all_now(c->fd, buf, at);
}

static void send_group_state(SwitchStream *s, int index) {
    SsClient *c = &s->clients[index];
    const int slot = client_slot(c);

    if (s->group_stream_known[slot]) {
        C2sStreamInfo info;
        memset(&info, 0, sizeof(info));
        info.width = s->group_width[slot];
        info.height = s->group_height[slot];
        info.video_codec = slot_codec(slot);
        send_msg_now(c, C2S_MSG_STREAM_INFO, &info, sizeof(info));
    }
    if (s->shared_known[slot]) {
        send_msg_now(c, C2S_MSG_SHARED, &s->shared[slot], sizeof(C2sShared));
    }
}

/*
 * Moves whatever complete WebSocket frames are in rx into ws_rx, where
 * the C2S parser will find them, and answers the control frames.
 *
 * Returns -1 when the connection has to go: a close, or a frame that
 * cannot be honoured. A browser that sends something malformed here is
 * not a browser.
 */
static int unwrap_ws(SwitchStream *s, int index) {
    SsClient *c = &s->clients[index];
    uint32_t at = 0;

    while (at < c->rx_len) {
        uint8_t op = 0;
        uint8_t *payload = NULL;
        size_t plen = 0;
        const long used = ws_take_frame(c->rx + at, c->rx_len - at, &op, &payload, &plen);
        if (used == 0) break;      /* the rest of the frame has not arrived */
        if (used < 0) return -1;

        if (op == WS_OP_CLOSE) {
            return -1;
        } else if (op == WS_OP_PING) {
            /* A pong, unmasked and with the same payload, which is what
             * the spec asks and what keeps a proxy from timing the
             * connection out. */
            uint8_t hdr[10];
            const size_t hl = ws_binary_header(plen, hdr);
            hdr[0] = 0x80 | WS_OP_PONG;
            if (send_all_now(c->fd, hdr, hl) == 0 && plen) {
                send_all_now(c->fd, payload, plen);
            }
        } else if (op == WS_OP_BINARY || op == WS_OP_CONT) {
            if (c->ws_rx_len + plen > c->ws_rx_cap) {
                const uint32_t want = c->ws_rx_len + (uint32_t)plen;
                uint8_t *bigger = realloc(c->ws_rx, want);
                if (!bigger) return -1;
                c->ws_rx = bigger;
                c->ws_rx_cap = want;
            }
            memcpy(c->ws_rx + c->ws_rx_len, payload, plen);
            c->ws_rx_len += (uint32_t)plen;
        }
        /* Text frames are ignored: this protocol is binary, and a page
         * that sends text here has nothing to say that is understood. */
        at += (uint32_t)used;
    }

    if (at) {
        memmove(c->rx, c->rx + at, c->rx_len - at);
        c->rx_len -= at;
    }
    return 0;
}

static void handle_messages(SwitchStream *s, int index) {
    SsClient *c = &s->clients[index];
    int codec_changed = 0;
    for (;;) {
        /* A browser's messages have already been taken out of their
         * WebSocket frames and put in ws_rx; everyone else's are still
         * where they were read. The parsing below is the same either
         * way, which is the whole point of carrying the same framing. */
        uint8_t *buf = c->is_ws ? c->ws_rx : c->rx;
        uint32_t *buf_len = c->is_ws ? &c->ws_rx_len : &c->rx_len;
        const uint32_t cap = c->is_ws ? c->ws_rx_cap : SS_RX_CAPACITY;

        if (!buf || *buf_len < sizeof(C2sFrameHeader)) {
            return;
        }
        C2sFrameHeader h;
        memcpy(&h, buf, sizeof(h));
        if (h.size > cap - sizeof(h)) {
            drop_client(s, index, "oversized message");
            return;
        }
        if (*buf_len < sizeof(h) + h.size) {
            return;
        }
        const uint8_t *payload = buf + sizeof(h);

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
                        s->profile_cb(s->profile_ctx, client_slot(c),
                                      p.width, p.height, p.fps,
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
                /* The GamePad's codec is refused unless the host can
                 * actually make it -- no drc-x264, or the setting off.
                 * Refusing is what lets the client fall back; accepting
                 * and then sending nothing would look like a dead
                 * stream. */
                if (h.size == 1 && payload[0] == C2S_CODEC_DRC_H264 && !s->drc_available) {
                    fprintf(stderr, "switch_stream: client %d asked for the wii u "
                                    "encode; this host cannot make it\n", index);
                    break;
                }
                if (h.size == 1
                    && (payload[0] == C2S_CODEC_VP8 || payload[0] == C2S_CODEC_H264
                        || payload[0] == C2S_CODEC_DRC_H264)
                    && c->codec != payload[0]) {
                    c->codec = payload[0];
                    fprintf(stderr, "switch_stream: client %d now on %s\n", index,
                            c->codec == C2S_CODEC_DRC_H264 ? "wii u" :
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
                /* Players only, exactly as POST /wake is. The host's
                 * configured reset method owns wake + controller recovery;
                 * native clients deliberately do not know how it is done. */
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
                    fprintf(stderr, "switch_stream: client %d asks for controller-output recovery\n", index);
                    gamepad_bridge_reset();
                }
                break;
            case C2S_MSG_PING:
                break;
            default:
                break;
        }
        {
            const uint32_t used = (uint32_t)sizeof(h) + h.size;
            memmove(buf, buf + used, *buf_len - used);
            *buf_len -= used;
        }
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

        /* -2 rather than -1: the accept below has to know which door a
         * client came in through, because that is what decides the
         * stream it is on. */
        const int drc_pfd = (s->drc_listen_fd >= 0) ? n : -1;
        if (s->drc_listen_fd >= 0) {
            pfds[n].fd = s->drc_listen_fd;
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            map[n] = -2;
            n++;
        }
        const int wiiu_pfd = (s->wiiu_listen_fd >= 0) ? n : -1;
        if (s->wiiu_listen_fd >= 0) {
            pfds[n].fd = s->wiiu_listen_fd;
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            map[n] = -2;
            n++;
        }

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

        /* A timeout is not a reason to skip the housekeeping below.
         *
         * This used to `continue` here, which skipped everything after
         * the per-socket handling -- including the notification that
         * says a chain now has an audience. With only a browser
         * connected, on the OTHER server's port, nothing ever made this
         * poll return, so the encoder for it was never started and the
         * page received sound and no picture. */
        const int ready = poll(pfds, n, 200);
        if (ready > 0) {

        for (int door = 0; door < 3; door++) {
            const int pfd = (door == 0) ? 0 : (door == 1) ? drc_pfd : wiiu_pfd;
            if (pfd < 0 || !(pfds[pfd].revents & POLLIN)) {
                continue;
            }
            const int is_drc = (door == 1);
            const int is_wiiu = (door == 2);

            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            memset(&peer, 0, sizeof(peer));

            int fd = accept(is_drc ? s->drc_listen_fd
                                   : is_wiiu ? s->wiiu_listen_fd : s->listen_fd,
                            (struct sockaddr *)&peer,
                            &peer_len);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                int sndbuf = SS_SOCKET_SNDBUF;
                setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

                SDL_LockMutex(s->mutex);

                /*
                 * Port 5083 belongs only to Wii U console clients.
                 *
                 * One physical console cannot legitimately have two
                 * Capture2Cloud applications alive at once, so a fresh
                 * connection from the same address makes an older one
                 * stale by definition.
                 */
                if (is_wiiu &&
                    peer.sin_family == AF_INET &&
                    peer.sin_addr.s_addr != 0) {

                    for (int i = 0; i < SS_MAX_CLIENTS; i++) {
                        SsClient *old_client = &s->clients[i];

                        if (old_client->in_use &&
                            old_client->on_wiiu_port &&
                            old_client->peer_ipv4 ==
                                peer.sin_addr.s_addr) {

                            drop_client(
                                s,
                                i,
                                "replaced by a new Wii U session");
                        }
                    }
                }

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
                    s->clients[slot].peer_ipv4 =
                        peer.sin_family == AF_INET
                            ? peer.sin_addr.s_addr
                            : 0;
                    s->clients[slot].last_seen_ms = now_ms();
                    if (is_wiiu) {
                        /* The port is the choice, as it is for the pad:
                         * nothing else is served here, so the console
                         * never spends a moment on another client's
                         * stream while a codec request crosses. */
                        s->clients[slot].codec = C2S_CODEC_H264;
                        s->clients[slot].on_wiiu_port = 1;
                    }
                    if (is_drc) {
                        /* The port is the choice: nothing else can be
                         * served here, and nothing served here can be
                         * decoded by anything else. */
                        s->clients[slot].codec = C2S_CODEC_DRC_H264;
                        s->clients[slot].on_drc_port = 1;
                    }
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
                const uint32_t quiet = now_ms() - c->last_seen_ms;
                if (quiet > SS_IDLE_TIMEOUT_MS) {
                    drop_client(s, i, "silent too long");
                } else if (quiet > SS_IDLE_PROBE_MS &&
                           now_ms() - c->last_probe_ms > SS_IDLE_PROBE_MS) {
                    /* Asked, not assumed. The answer arrives as bytes,
                     * which is what refreshes the clock above. */
                    c->last_probe_ms = now_ms();
                    send_msg_now(c, C2S_MSG_PING, NULL, 0);
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

            if (c->is_ws) {
                /* The upgrade was the handshake, so there is no hello to
                 * wait for -- only frames to unwrap. */
                if (unwrap_ws(s, i) != 0) {
                    drop_client(s, i, "websocket closed");
                    continue;
                }
            } else if (!c->handshake_done) {
                handle_hello(s, i);
            }
            if (s->clients[i].in_use && s->clients[i].handshake_done) {
                handle_messages(s, i);
            }
        }
        SDL_UnlockMutex(s->mutex);
        } /* if (ready > 0) */

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
    s->audio_udp_fd = -1;
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

    /*
     * SOCK_CLOEXEC, and it matters more than it looks.
     *
     * This host forks and execs the GamePad client, which inherited
     * every listening socket -- `ss -tlnp` showed wiiu_pad holding
     * 5081, 5082 and 5083 alongside the host. SO_REUSEADDR lets a new
     * bind past a socket in TIME_WAIT; it does NOT let one past another
     * process that is actively listening. So a host that died while its
     * child lived on left all three ports held by a program that cannot
     * answer on them, and the next start could not bind -- which looks
     * exactly like "it will not launch" and has nothing in the log to
     * say why.
     */
    s->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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

    /*
     * The GamePad's door, which is allowed to fail.
     *
     * A machine that cannot bind it still serves the console, the phone
     * and the browsers perfectly well -- so this reports and carries on
     * rather than taking the whole transport down with it. Something
     * else already holding that port is the usual reason, and a pad is
     * the one client that can be told to use another.
     */
    s->drc_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s->drc_listen_fd >= 0) {
        setsockopt(s->drc_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in drc_addr;
        memset(&drc_addr, 0, sizeof(drc_addr));
        drc_addr.sin_family = AF_INET;
        drc_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        drc_addr.sin_port = htons(C2S_DRC_PORT);
        if (bind(s->drc_listen_fd, (struct sockaddr *)&drc_addr, sizeof(drc_addr)) != 0 ||
            listen(s->drc_listen_fd, 2) != 0) {
            fprintf(stderr, "switch_stream: no wii u port %u (%s); the other "
                            "clients are unaffected\n",
                    C2S_DRC_PORT, strerror(errno));
            close(s->drc_listen_fd);
            s->drc_listen_fd = -1;
        } else {
            fprintf(stderr, "switch_stream: wii u gamepads on port %u\n", C2S_DRC_PORT);
        }
    }

    /* The Wii U console's door, allowed to fail for the same reason the
     * one above is: a machine that cannot bind it serves everything
     * else perfectly well. */
    s->wiiu_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s->wiiu_listen_fd >= 0) {
        setsockopt(s->wiiu_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in wiiu_addr;
        memset(&wiiu_addr, 0, sizeof(wiiu_addr));
        wiiu_addr.sin_family = AF_INET;
        wiiu_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        wiiu_addr.sin_port = htons(C2S_WIIU_PORT);
        if (bind(s->wiiu_listen_fd, (struct sockaddr *)&wiiu_addr, sizeof(wiiu_addr)) != 0 ||
            listen(s->wiiu_listen_fd, 2) != 0) {
            fprintf(stderr, "switch_stream: no wii u console port %u (%s); the other "
                            "clients are unaffected\n",
                    C2S_WIIU_PORT, strerror(errno));
            close(s->wiiu_listen_fd);
            s->wiiu_listen_fd = -1;
        } else {
            fprintf(stderr, "switch_stream: wii u consoles on port %u\n", C2S_WIIU_PORT);
        }
    }

    /*
     * UDP needs no listen/bind on the host: sendto() will choose the
     * correct local interface according to the Wii U peer address.
     */
    s->audio_udp_fd =
        socket(
            AF_INET,
            SOCK_DGRAM | SOCK_CLOEXEC,
            0);

    if (s->audio_udp_fd >= 0) {
        fcntl(
            s->audio_udp_fd,
            F_SETFL,
            fcntl(
                s->audio_udp_fd,
                F_GETFL,
                0) |
                O_NONBLOCK);

        int sndbuf = 64 * 1024;

        setsockopt(
            s->audio_udp_fd,
            SOL_SOCKET,
            SO_SNDBUF,
            &sndbuf,
            sizeof(sndbuf));

    } else {
        fprintf(
            stderr,
            "switch_stream: Wii U PCM UDP unavailable: %s\n",
            strerror(errno));
    }

    s->running = 1;
    s->thread = SDL_CreateThread(accept_thread, "switch-stream", s);
    if (!s->thread) {
        if (s->audio_udp_fd >= 0) {
            close(s->audio_udp_fd);
            s->audio_udp_fd = -1;
        }

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

    if (s->audio_udp_fd >= 0) {
        close(s->audio_udp_fd);
        s->audio_udp_fd = -1;
    }

    if (s->wiiu_listen_fd >= 0) {
        close(s->wiiu_listen_fd);
        s->wiiu_listen_fd = -1;
    }
    if (s->drc_listen_fd >= 0) {
        close(s->drc_listen_fd);
    }
    SDL_DestroyMutex(s->mutex);
    free(s);
}
