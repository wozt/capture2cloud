#define GST_USE_UNSTABLE_API

#include "gst_webrtc.h"
#include "switch_stream.h"
#include "c2s_protocol.h"

#include <gst/video/video.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ifaddrs.h>

#include "app_config.h"
#include "gamepad_bridge.h"

#include <SDL2/SDL.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hard ceiling on simultaneous clients: the slot array is sized for
 * this, and the runtime limit (MAX_CLIENTS in the .env) is clamped to
 * it. A cap exists because each client adds its own queue + RTP
 * payloader + webrtcbin to a live pipeline; letting the config ask for
 * an unbounded number would just turn a typo into an out-of-memory. */
#define MAX_CLIENTS_CEILING 32
#define DEFAULT_MAX_CLIENTS 8

/* RTP payload size. GStreamer's payloaders default to 1400, which only
 * survives a path with a 1500-byte MTU: over a tunnel it produces
 * packets nothing will carry. Tailscale/WireGuard use 1280, and since
 * ICE, DTLS and the gamepad DataChannel all send small packets, the
 * connection comes up perfectly -- only the video, whose packets are all
 * full-size, is silently dropped. 1200 is what libwebrtc itself uses for
 * the browser's own outgoing RTP, for exactly this reason: it clears a
 * 1280 tunnel with room for the SRTP and IP headers, and costs about 1%
 * more header overhead on a plain LAN. */
#define DEFAULT_RTP_MTU 1200
#define MIN_RTP_MTU 576
#define MAX_RTP_MTU 1400

/* Headroom subtracted from the link MTU to get the RTP packet size:
 * IPv6 header (40, the worst case; IPv4 needs only 20) + UDP (8) + the
 * SRTP authentication tag and any ICE/TURN framing. 80 leaves a little
 * slack on top of that rather than sitting exactly on the limit. */
#define RTP_HEADER_ALLOWANCE 80

/* Frames between forced keyframes. WebRTC receivers ask for one when
 * they actually need it (PLI), so this is only a safety net for a
 * request that never arrives -- it does not need to be frequent.
 *
 * It used to be 30, i.e. two keyframes a SECOND at 60 fps. Measured over
 * 230 s with one client, counting gaps between presented frames:
 *
 *     30 (0.5 s):  57 hitches, 487 frames dropped by the browser
 *    300 (5 s)  :  19 hitches, 203 frames dropped
 *    600 (10 s) :  27 hitches, 178 frames dropped
 *
 * A 1080p keyframe is far more expensive to decode than a P-frame, and
 * at a fixed CBR budget it also steals bitrate from the 59 frames
 * around it. Nearly all of the gain is in leaving 30 behind; past 5 s
 * the difference is noise, while recovery from a lost keyframe request
 * only gets slower. */
/* The native client's stream. 720p is the handheld screen's own
 * resolution and what software VP8 decode can keep up with on a Tegra
 * X1; sending 1080p would be decoded at great cost only to be scaled
 * back down. The bitrate is sized for that resolution rather than
 * inherited from the 1080p stream. */
#define SWITCH_VIDEO_WIDTH 1280
#define SWITCH_VIDEO_HEIGHT 720
#define SWITCH_VIDEO_BITRATE_KBPS 6000

/* Which H.264 encoder the native branch uses.
 *
 * Both GPUs in this machine have a video encode block that does H.264,
 * and x264 was costing 6.4 ms of CPU per 720p frame -- 38% of a core at
 * 60 fps, on top of the MJPEG decode, the 1080p VP8 encode for the
 * browser and two colour conversions. That is why 720p60 to the console
 * only worked once the browser stream had been turned down: they were
 * competing for the same cores. On the GPU the same frame costs 1.4 ms
 * of CPU, and the encode itself no longer runs on a core at all.
 *
 * The integrated GPU is preferred over the discrete one. Not because it
 * is faster -- it is not -- but because the discrete card is driving the
 * display and whatever is being played on it, while the integrated one
 * sits idle. Measured: the same encode costs a third of the system time
 * there.
 *
 * VA is H.264 only. Neither of these GPUs can encode VP8 -- AMD's video
 * engine has never supported it -- so that branch stays on the CPU, and
 * no amount of hardware here changes that.
 *
 * Set SWITCH_H264_ENCODER in the .env to force one ("x264enc" to stay on
 * the CPU); the default tries each of these in order and takes the first
 * that this build of GStreamer actually has. */
static const char *const H264_ENCODER_PREFERENCE[] = {
    "varenderD129h264enc", /* integrated GPU: idle, and cheapest in system time */
    "vah264enc",           /* discrete GPU: also fine, but it has other work */
    "x264enc",             /* no GPU encoder available: back to the CPU */
};

#define DEFAULT_KEYFRAME_MAX_DIST 300
#define MIN_KEYFRAME_MAX_DIST 15
#define MAX_KEYFRAME_MAX_DIST 3000

typedef struct {
    int in_use;
    /* Whether this client is allowed to drive the console (see
     * `may_control` in gst_webrtc.h). Read in on_gamepad_message() on
     * every incoming gamepad message, written once at negotiation time
     * before the DataChannel can possibly exist -- no lock needed. */
    int may_control;
    GstWebrtcStream *g;
    GstElement *webrtcbin;
    GstElement *vqueue, *aqueue;
    /* RTP payloader DEDICATED to this client (not shared): each client can
     * negotiate a different payload number and RTP session
     * (sequence/SSRC), which a single payloader shared across all of them
     * cannot satisfy correctly. */
    GstElement *vpay, *apay;
    GstPad *vtee_pad, *atee_pad;
    /* The video tee this client was linked to -- needed to release the
     * request pad again on teardown. */
    GstElement *vtee;
    /* Set once a teardown has been scheduled, so a burst of state
     * changes (disconnected -> failed -> closed) can't queue several
     * teardowns for the same client. Guarded by clients_mutex. */
    int teardown_scheduled;
} WebrtcClient;

struct GstWebrtcStream {
    GstElement *pipeline;
    /* Single video encoding chain: software VP8.
     *
     * There used to be a second, hardware H264 chain alongside it, but
     * it was only ever selected when the browser offered NO VP8
     * (`vp8_pt < 0`) -- and every browser that can do WebRTC offers VP8,
     * so in practice it never ran. It cost a permanently-present appsrc,
     * encoder, parser and tee in the pipeline, plus a whole second
     * colour conversion (RGB -> NV12) kept alive for it. Removed: one
     * chain, one conversion, one format. */
    GstElement *vsrc_vp8, *venc_vp8, *vtee_vp8;
    /* What the browser stream is encoded at. Changeable while running;
     * the capture stays 1080p and the scale happens on the way to the
     * encoder, which costs under a millisecond a frame (measured) and
     * saves the encoder most of its work at the lower sizes. */
    int browser_width, browser_height;
    GstElement *vscale_caps;
    /* The native branch: a smaller encode for the Switch client, fed
     * only while one is connected. */
    GstElement *vsrc_switch, *venc_switch, *switchsink, *switchasink;
    GstElement *vscale_switch_caps;
    /* The H.264 alternative: its own appsrc and its own chain, rather
     * than a second branch off a tee. A tee made both chains share one
     * negotiation, and the appsrc then hung in an allocation query that
     * the closed branch never answered -- the native stream stopped
     * dead. Two chains have nothing to agree on, and the one not in use
     * is never fed, so it costs nothing. */
    GstElement *vsrc_switch264, *venc_switch_h264, *switchsink264;
    GstElement *vscale_switch264_caps;
    /* The VA encoders take NV12 and nothing else, x264enc takes I420.
     * The conversion happens in the scaler that feeds the branch either
     * way, so this costs nothing -- but the appsrc has to declare the
     * right one. */
    int switch264_nv12;
    const char *switch264_encoder;
    int switch_width, switch_height, switch_fps;
    int switch_codec; /* C2sCodec */
    /* Kept, not merely applied: a client that connects later has to be
     * told the rate everyone is already on. */
    int switch_bitrate_kbps;
    /* The capture card's format, shared with the browsers too. Mirrored
     * here so the native announcement can carry it. */
    int capture_mjpeg;
    struct SwsContext *sws_switch;
    enum AVPixelFormat sws_switch_src_format;
    enum AVPixelFormat sws_switch_dst_format;
    uint8_t *switch_i420_buf;
    size_t switch_i420_size;
    uint64_t switch_frame;
    SwitchStream *switch_out;
    GstElement *asrc, *atee;
    GMainContext *ctx; /* private GLib context, shared with every webrtcbin created by handle_offer() -- see gst_thread_main() */
    GMainLoop *loop;
    SDL_Thread *thread;
    guint bus_watch_id;

    int width, height;
    int audio_rate, audio_channels;

    int vp8_active; /* set once a first client is connected: no encoding happens before that */
    int video_bitrate_kbps;
    guint64 video_frame;
    guint64 audio_frames;

    struct SwsContext *sws_i420; /* capture format -> I420, what vp8enc consumes */
    enum AVPixelFormat sws_src_format; /* what sws_i420 was built for */
    uint8_t *i420_buf;
    size_t i420_size;

    SDL_mutex *clients_mutex;
    /* Sized for the ceiling; only the first `max_clients` are ever
     * handed out (see g_max_clients / config_get_int below). */
    WebrtcClient clients[MAX_CLIENTS_CEILING];
    int max_clients;
};

/* Defined further down, next to the rest of the native branch. */
static GstFlowReturn on_switch_video_sample(GstElement *sink, gpointer user_data);
static GstFlowReturn on_switch_audio_sample(GstElement *sink, gpointer user_data);

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    GstWebrtcStream *g = user_data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            fprintf(stderr, "gst_webrtc: pipeline error: %s (%s)\n", err->message, dbg ? dbg : "?");
            g_error_free(err);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_warning(msg, &err, &dbg);
            fprintf(stderr, "gst_webrtc: warning: %s (%s)\n", err->message, dbg ? dbg : "?");
            g_error_free(err);
            g_free(dbg);
            break;
        }
        default:
            break;
    }
    (void)g;
    return TRUE;
}

static int gst_thread_main(void *arg) {
    GstWebrtcStream *g = arg;
    /* Private context (not the global default one): this thread must not
     * compete for sources (bus watch, idle...) with the GTK loop, which
     * runs on the global default context on its own thread. Without this,
     * g_idle_add() (always attached to the global context) could be
     * serviced by THIS thread instead of the GTK thread, even before the
     * GTK widgets exist.
     *
     * This same context (g->ctx) is also pushed as thread-default by
     * gst_webrtc_stream_handle_offer() before creating each client
     * webrtcbin (see there): without that, webrtcbin would schedule its
     * own internal signals (including "on-data-channel"/"on-message-data"
     * -- critical for gamepad latency) on the global default context,
     * i.e. competing with all GTK activity (rendering, docking polling...)
     * instead of on this dedicated loop, which does nothing else. */
    g_main_context_push_thread_default(g->ctx);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(g->pipeline));
    g->bus_watch_id = gst_bus_add_watch(bus, on_bus_message, g);
    gst_object_unref(bus);

    g->loop = g_main_loop_new(g->ctx, FALSE);
    g_main_loop_run(g->loop);

    g_main_context_pop_thread_default(g->ctx);
    g_main_loop_unref(g->loop);
    g->loop = NULL;
    return 0;
}

GstWebrtcStream *gst_webrtc_stream_create(int width, int height, int audio_rate, int audio_channels) {
    static int gst_inited = 0;
    if (!gst_inited) {
        gst_init(NULL, NULL);
        gst_inited = 1;
    }

    GstWebrtcStream *g = calloc(1, sizeof(*g));
    if (!g) {
        return NULL;
    }
    g->width = width;
    g->height = height;
    g->audio_rate = audio_rate;
    g->audio_channels = audio_channels;
    g->video_bitrate_kbps = 12000;
    /* Runtime client limit, clamped to what the slot array can hold. */
    g->max_clients = (int)config_get_int("MAX_CLIENTS", DEFAULT_MAX_CLIENTS, 1, MAX_CLIENTS_CEILING);
    g->clients_mutex = SDL_CreateMutex();
    if (!g->clients_mutex) {
        free(g);
        return NULL;
    }

    int keyframe_max_dist = (int)config_get_int("KEYFRAME_MAX_DIST", DEFAULT_KEYFRAME_MAX_DIST,
                                                MIN_KEYFRAME_MAX_DIST, MAX_KEYFRAME_MAX_DIST);

    /* Asked for by name, then checked: an encoder configured in the .env
     * that this build does not have would otherwise take the whole
     * pipeline down at parse time, over a preference. */
    char forced[64];
    g->switch264_encoder = NULL;
    if (config_get("SWITCH_H264_ENCODER", forced, sizeof(forced)) && forced[0]) {
        GstElementFactory *f = gst_element_factory_find(forced);
        if (f) {
            gst_object_unref(f);
            for (size_t i = 0; i < sizeof(H264_ENCODER_PREFERENCE) / sizeof(*H264_ENCODER_PREFERENCE); i++) {
                if (strcmp(forced, H264_ENCODER_PREFERENCE[i]) == 0) {
                    g->switch264_encoder = H264_ENCODER_PREFERENCE[i];
                }
            }
            if (!g->switch264_encoder) {
                fprintf(stderr, "gst_webrtc: SWITCH_H264_ENCODER=%s is not one this knows how to "
                                "configure, choosing automatically\n", forced);
            }
        } else {
            fprintf(stderr, "gst_webrtc: SWITCH_H264_ENCODER=%s is not installed, "
                            "choosing automatically\n", forced);
        }
    }
    for (size_t i = 0; !g->switch264_encoder &&
                       i < sizeof(H264_ENCODER_PREFERENCE) / sizeof(*H264_ENCODER_PREFERENCE); i++) {
        GstElementFactory *f = gst_element_factory_find(H264_ENCODER_PREFERENCE[i]);
        if (f) {
            gst_object_unref(f);
            g->switch264_encoder = H264_ENCODER_PREFERENCE[i];
        }
    }
    g->switch264_nv12 = g->switch264_encoder && strcmp(g->switch264_encoder, "x264enc") != 0;

    /* The two families disagree on the name of nearly every property
     * that matters, so the whole tail of the element is built here
     * rather than parameterised. */
    char h264_enc[320];
    if (g->switch264_nv12) {
        snprintf(h264_enc, sizeof(h264_enc),
                 "%s name=venc_switch_h264 bitrate=%d key-int-max=%d b-frames=0 ref-frames=1 "
                 "rate-control=cbr target-usage=6",
                 g->switch264_encoder, SWITCH_VIDEO_BITRATE_KBPS, keyframe_max_dist);
    } else {
        /* threads=1 and no sliced threading: frame-level threading holds
         * frames back to fill its pipeline, which is latency, and sliced
         * threading cuts each picture into several slices, which the
         * console's decoder handles far less predictably. */
        snprintf(h264_enc, sizeof(h264_enc),
                 "x264enc name=venc_switch_h264 tune=zerolatency speed-preset=veryfast "
                 "threads=1 sliced-threads=false bitrate=%d key-int-max=%d bframes=0",
                 SWITCH_VIDEO_BITRATE_KBPS, keyframe_max_dist);
    }
    fprintf(stderr, "gst_webrtc: native H.264 encoder: %s (%s)\n",
            g->switch264_encoder ? g->switch264_encoder : "none",
            g->switch264_nv12 ? "on the GPU" : "on the CPU");

    char desc[3072];
    snprintf(desc, sizeof(desc),
             /* max-buffers=1 + leaky matters more than it looks. appsrc
              * defaults to max-bytes=200000 with leaky-type=none, and a
              * 1080p I420 frame is 3.1 MB -- fifteen times that limit. The
              * limit only drives the need-data/enough-data signals, which
              * nothing here listens to, so with block=false every frame
              * was accepted anyway and the internal queue grew without
              * bound whenever the encoder fell behind. On a complex or
              * abruptly changing scene that is exactly when it does, and
              * the encoder then worked on older and older frames while
              * memory grew by 3.1 MB a time.
              *
              * For a live source the newest frame is the only one worth
              * having, so hold one and drop the stale one. */
             "appsrc name=vsrc_vp8 format=time is-live=true do-timestamp=true "
             "max-buffers=1 leaky-type=downstream "
             "caps=video/x-raw,format=I420,width=%d,height=%d,framerate=60/1 ! "
             "queue max-size-buffers=1 leaky=downstream ! "
             /* Resolution is changed by rewriting this capsfilter, not by
              * pushing differently-sized buffers into the appsrc: doing
              * the latter crashed inside libgstvpx, because the encoder
              * downstream had negotiated the old size and nothing told it
              * otherwise. videoscale exists to renegotiate, so it is what
              * does it. */
             "videoscale name=vscale ! capsfilter name=vscale_caps ! "
             "vp8enc name=venc_vp8 deadline=1 cpu-used=8 threads=4 end-usage=cbr target-bitrate=%d "
             "keyframe-max-dist=%d ! "
             "tee name=vtee_vp8 allow-not-linked=true "

             "appsrc name=asrc format=time is-live=true do-timestamp=true "
             "caps=audio/x-raw,format=S16LE,rate=%d,channels=%d,layout=interleaved ! "
             /* Audio latency budget, tightened deliberately:
              *  - queue capped at 2 buffers (~10 ms) instead of 8 (~40 ms):
              *    with leaky=downstream it drops stale audio rather than
              *    letting a backlog build up and never recover.
              *  - frame-size 5 ms instead of 10: half the encoder's
              *    packetisation delay, for a little more RTP overhead.
              *  - audio-type=restricted-lowdelay disables Opus's
              *    look-ahead/prediction, which is pure added delay for a
              *    live feed nobody rewinds. */
             "queue max-size-buffers=2 leaky=downstream ! "
             "audioconvert ! opusenc bitrate=64000 frame-size=5 audio-type=restricted-lowdelay ! "
             "tee name=atee allow-not-linked=true "

             /* The native (Switch) branch. Its own appsrc rather than a
              * tee off the 1080p one, because that is what makes it free
              * when no Switch is connected: nothing is pushed, so the
              * encoder never runs. A tee would have kept encoding a
              * second resolution for nobody.
              *
              * 720p because that is the handheld screen's native size and
              * the client decodes in software -- see
              * switch_homebrew/ARCHITECTURE.md. The scale itself costs
              * 0.87 ms/frame from YUYV, measured. */
             "appsrc name=vsrc_switch format=time is-live=true do-timestamp=true "
             "max-buffers=1 leaky-type=downstream "
             "caps=video/x-raw,format=I420,width=%d,height=%d,framerate=60/1 ! "
             "queue max-size-buffers=1 leaky=downstream ! "
             /* videorate as well as videoscale: a capsfilter alone
              * cannot change the rate, so asking for 30 fps quietly did
              * nothing and the client kept decoding 60 -- half the work
              * wasted on exactly the problem the request exists to
              * solve. */
             "videorate name=vrate_switch drop-only=true ! "
             "videoscale name=vscale_switch ! capsfilter name=vscale_switch_caps ! "
             "vp8enc name=venc_switch deadline=1 cpu-used=8 threads=2 end-usage=cbr "
             "target-bitrate=%d keyframe-max-dist=%d ! "
             /* async=false on all three native sinks.
              *
              * A sink normally holds the whole pipeline in PAUSED until
              * it has prerolled a first buffer, and only one of the two
              * video chains is ever fed -- so the idle one never
              * prerolled, the pipeline never reached PLAYING, and every
              * other sink sat blocked in wait_preroll. That is what
              * stopped the native stream dead and took the sound with
              * it. These are not clocks or speakers; nothing should wait
              * on them. */
             "appsink name=switchsink emit-signals=true sync=false async=false "
             "max-buffers=2 drop=true "

             /* The same picture in H.264, for the console's own decode
              * engine -- and a completely separate chain, from its own
              * appsrc down.
              *
              * This was one tee with a valve on each branch, which is the
              * obvious shape and the wrong one: the two branches then
              * share a negotiation, and the appsrc hung in an allocation
              * query that the closed branch never answered. The native
              * stream stopped dead, and with the pipeline half-stalled
              * the sound went with it. Two chains have nothing to agree
              * on. Only the chosen one is fed, so the other costs
              * nothing -- the same reason this branch is not itself a tee
              * off the browser's. */
             "appsrc name=vsrc_switch264 format=time is-live=true do-timestamp=true "
             "max-buffers=1 leaky-type=downstream "
             "caps=video/x-raw,format=%s,width=%d,height=%d,framerate=60/1 ! "
             "queue max-size-buffers=1 leaky=downstream ! "
             "videorate name=vrate_switch264 drop-only=true ! "
             "videoscale name=vscale_switch264 ! capsfilter name=vscale_switch264_caps ! "
             "%s ! "
             "video/x-h264,stream-format=byte-stream,alignment=au ! "
             /* config-interval=-1 repeats SPS/PPS ahead of every
              * keyframe. Without them a decoder does not know the
              * picture size or profile, so a client that arrives -- or
              * re-opens its decoder on a codec switch -- after the
              * stream started has nothing to start from and shows
              * black. */
             "h264parse config-interval=-1 ! "
             "appsink name=switchsink264 emit-signals=true sync=false async=false "
             "max-buffers=2 drop=true "

             /* Audio for the same client, tapped off the encoder the
              * browser already uses: one Opus stream serves both. */
             "atee. ! queue max-size-buffers=2 leaky=downstream ! "
             "appsink name=switchasink emit-signals=true sync=false async=false "
             "max-buffers=8 drop=true",
             width, height, g->video_bitrate_kbps * 1000, keyframe_max_dist, audio_rate, audio_channels,
             SWITCH_VIDEO_WIDTH, SWITCH_VIDEO_HEIGHT, SWITCH_VIDEO_BITRATE_KBPS * 1000,
             keyframe_max_dist,
             g->switch264_nv12 ? "NV12" : "I420", SWITCH_VIDEO_WIDTH, SWITCH_VIDEO_HEIGHT,
             h264_enc);

    GError *error = NULL;
    g->pipeline = gst_parse_launch(desc, &error);
    if (!g->pipeline) {
        fprintf(stderr, "gst_webrtc_stream_create: gst_parse_launch: %s\n", error ? error->message : "?");
        if (error) g_error_free(error);
        SDL_DestroyMutex(g->clients_mutex);
        free(g);
        return NULL;
    }

    g->vsrc_vp8 = gst_bin_get_by_name(GST_BIN(g->pipeline), "vsrc_vp8");
    g->venc_vp8 = gst_bin_get_by_name(GST_BIN(g->pipeline), "venc_vp8");
    g->vtee_vp8 = gst_bin_get_by_name(GST_BIN(g->pipeline), "vtee_vp8");
    g->vscale_caps = gst_bin_get_by_name(GST_BIN(g->pipeline), "vscale_caps");
    g->browser_width = width;
    g->browser_height = height;
    g->vsrc_switch = gst_bin_get_by_name(GST_BIN(g->pipeline), "vsrc_switch");
    g->venc_switch = gst_bin_get_by_name(GST_BIN(g->pipeline), "venc_switch");
    g->switchsink = gst_bin_get_by_name(GST_BIN(g->pipeline), "switchsink");
    g->vscale_switch_caps = gst_bin_get_by_name(GST_BIN(g->pipeline), "vscale_switch_caps");
    g->venc_switch_h264 = gst_bin_get_by_name(GST_BIN(g->pipeline), "venc_switch_h264");
    g->switchsink264 = gst_bin_get_by_name(GST_BIN(g->pipeline), "switchsink264");
    g->vsrc_switch264 = gst_bin_get_by_name(GST_BIN(g->pipeline), "vsrc_switch264");
    g->vscale_switch264_caps = gst_bin_get_by_name(GST_BIN(g->pipeline), "vscale_switch264_caps");
    g->switch_codec = C2S_CODEC_VP8;
    if (g->switchsink264) {
        g_signal_connect(g->switchsink264, "new-sample", G_CALLBACK(on_switch_video_sample), g);
    }
    g->switch_width = SWITCH_VIDEO_WIDTH;
    g->switch_height = SWITCH_VIDEO_HEIGHT;
    g->switch_fps = 60;
    g->switch_bitrate_kbps = SWITCH_VIDEO_BITRATE_KBPS;
    g->switchasink = gst_bin_get_by_name(GST_BIN(g->pipeline), "switchasink");
    if (g->switchsink) {
        g_signal_connect(g->switchsink, "new-sample", G_CALLBACK(on_switch_video_sample), g);
    }
    if (g->switchasink) {
        g_signal_connect(g->switchasink, "new-sample", G_CALLBACK(on_switch_audio_sample), g);
    }
    g->asrc = gst_bin_get_by_name(GST_BIN(g->pipeline), "asrc");
    g->atee = gst_bin_get_by_name(GST_BIN(g->pipeline), "atee");
    if (!g->vsrc_vp8 ||
        !g->venc_vp8 ||
        !g->vtee_vp8 || !g->asrc || !g->atee) {
        fprintf(stderr, "gst_webrtc_stream_create: element(s) not found in pipeline\n");
        gst_object_unref(g->pipeline);
        SDL_DestroyMutex(g->clients_mutex);
        free(g);
        return NULL;
    }

    g->ctx = g_main_context_new();

    g->thread = SDL_CreateThread(gst_thread_main, "gst-webrtc", g);

    gst_element_set_state(g->pipeline, GST_STATE_PLAYING);

    return g;
}

void gst_webrtc_stream_destroy(GstWebrtcStream *g) {
    if (!g) {
        return;
    }
    if (g->pipeline) {
        gst_element_set_state(g->pipeline, GST_STATE_NULL);
    }
    if (g->loop) {
        g_main_loop_quit(g->loop);
    }
    if (g->thread) {
        /* The bus watch and the loop run on the private context (g->ctx);
         * we wait for gst_thread_main() to return (right after
         * g_main_loop_quit above) before freeing that context, which is
         * also the one pushed as thread-default by handle_offer() for
         * each client webrtcbin. */
        SDL_WaitThread(g->thread, NULL);
    }
    if (g->ctx) {
        g_main_context_unref(g->ctx);
        g->ctx = NULL;
    }
    if (g->vsrc_vp8) gst_object_unref(g->vsrc_vp8);
    if (g->venc_vp8) gst_object_unref(g->venc_vp8);
    if (g->vtee_vp8) gst_object_unref(g->vtee_vp8);
    if (g->asrc) gst_object_unref(g->asrc);
    if (g->atee) gst_object_unref(g->atee);
    if (g->pipeline) gst_object_unref(g->pipeline);
    if (g->sws_i420) sws_freeContext(g->sws_i420);
    free(g->i420_buf);
    SDL_DestroyMutex(g->clients_mutex);
    free(g);
}

/* --- the native (Switch) branch ------------------------------------- */

/* Encoded frames leave the pipeline here and go straight onto the
 * socket. Called on a GStreamer thread, so it does nothing but hand the
 * bytes over -- switch_stream never blocks. */
static GstFlowReturn on_switch_video_sample(GstElement *sink, gpointer user_data) {
    GstWebrtcStream *g = user_data;
    GstSample *sample = NULL;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) {
        return GST_FLOW_OK;
    }

    /* Both encoders report here, and only the chosen one may reach the
     * client.
     *
     * On a codec switch the chain that was running still has a frame or
     * two inside it, and those come out after the client has been told
     * the codec changed and has rebuilt its decoder for the other one.
     * It then decoded VP8 as H.264 -- which does not fail cleanly, it
     * produces a picture, and the picture was bright pink. Nothing
     * downstream can tell those bytes apart from the real thing, so they
     * are stopped here, where which encoder produced them is known. */
    int is_h264 = (sink == g->switchsink264);
    if (is_h264 != (g->switch_codec == C2S_CODEC_H264)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        int keyframe = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
        switch_stream_send_video(g->switch_out, map.data, (uint32_t)map.size, keyframe);
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static GstFlowReturn on_switch_audio_sample(GstElement *sink, gpointer user_data) {
    GstWebrtcStream *g = user_data;
    GstSample *sample = NULL;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) {
        return GST_FLOW_OK;
    }
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        switch_stream_send_audio(g->switch_out, map.data, (uint32_t)map.size);
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* Changes the size the BROWSER stream is encoded at, while running. The
 * capture and the native branch are untouched. */
void gst_webrtc_stream_set_browser_resolution(GstWebrtcStream *g, int width, int height) {
    if (!g || !g->vscale_caps || width <= 0 || height <= 0) {
        return;
    }
    if (g->browser_width == width && g->browser_height == height) {
        return;
    }
    /* Rewriting the filter is the whole change: videoscale renegotiates
     * with the encoder, which is the supported way to do this while
     * running. */
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                        "width", G_TYPE_INT, width,
                                        "height", G_TYPE_INT, height, NULL);
    g_object_set(g->vscale_caps, "caps", caps, NULL);
    gst_caps_unref(caps);

    g->browser_width = width;
    g->browser_height = height;
    fprintf(stderr, "gst_webrtc: browser stream now %dx%d\n", width, height);
}

int gst_webrtc_stream_get_video_bitrate(GstWebrtcStream *g) {
    return g ? g->video_bitrate_kbps : 0;
}

void gst_webrtc_stream_get_browser_resolution(GstWebrtcStream *g, int *width, int *height) {
    if (width) *width = g ? g->browser_width : 0;
    if (height) *height = g ? g->browser_height : 0;
}

/* Forces the native encoder to emit a keyframe now. Called when a client
 * had to skip one: VP8 decodes every frame against the previous, so a
 * gap corrupts everything after it until the next keyframe -- five
 * seconds at the usual interval. */
static void on_switch_keyframe_request(void *ctx) {
    GstWebrtcStream *g = ctx;
    if (!g) {
        return;
    }
    /* Both branches, not just the VP8 one. Sending it only to the sink
     * that happened to be written first meant that in H.264 the request
     * went to an encoder receiving nothing, and the client waited out
     * the full keyframe interval -- several seconds of corruption, or a
     * black screen when the wait began at a codec switch. The closed
     * branch is idle, so the event costs nothing there. */
    if (g->switchsink) {
        gst_element_send_event(g->switchsink,
                               gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
    }
    if (g->switchsink264) {
        gst_element_send_event(g->switchsink264,
                               gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
    }
}

/* A client saying what it can actually decode. Applied to the native
 * branch only; the browser stream and the capture are untouched. */
/* One line, because it is called from four places and forgetting one of
 * them is how a client ends up showing a setting nobody else has. */
static void announce_shared(GstWebrtcStream *g) {
    if (!g || !g->switch_out) return;
    switch_stream_announce_shared(g->switch_out,
                                  (uint16_t)g->switch_width, (uint16_t)g->switch_height,
                                  (uint16_t)g->switch_fps,
                                  (uint16_t)g->switch_bitrate_kbps,
                                  (uint8_t)g->switch_codec,
                                  (uint8_t)(g->capture_mjpeg ? 1 : 0));
}

static void on_switch_profile_request(void *ctx, int w, int h, int fps, int bitrate_kbps) {
    GstWebrtcStream *g = ctx;
    if (!g || !g->vscale_switch_caps || w <= 0 || h <= 0) {
        return;
    }
    if (fps <= 0 || fps > 60) fps = 60;
    /* Even dimensions only: VP8 rejects odd ones and a half-pixel chroma
     * plane is not a thing. */
    w &= ~1;
    h &= ~1;

    GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                        "width", G_TYPE_INT, w,
                                        "height", G_TYPE_INT, h,
                                        "framerate", GST_TYPE_FRACTION, fps, 1, NULL);
    g_object_set(g->vscale_switch_caps, "caps", caps, NULL);
    /* Both chains, not just the one running: the idle one has to already
     * be the right size when the codec is switched to it, or the client
     * gets one announcement and a differently-sized picture. */
    if (g->vscale_switch264_caps) {
        g_object_set(g->vscale_switch264_caps, "caps", caps, NULL);
    }
    gst_caps_unref(caps);

    if (bitrate_kbps > 0) {
        if (bitrate_kbps < 500) bitrate_kbps = 500;
        if (bitrate_kbps > 20000) bitrate_kbps = 20000;
        if (g->venc_switch) {
            g_object_set(g->venc_switch, "target-bitrate", bitrate_kbps * 1000, NULL);
        }
        if (g->venc_switch_h264) {
            g_object_set(g->venc_switch_h264, "bitrate", bitrate_kbps, NULL);
        }
    }
    g->switch_width = w;
    g->switch_height = h;
    g->switch_fps = fps;
    if (bitrate_kbps > 0) g->switch_bitrate_kbps = bitrate_kbps;
    on_switch_keyframe_request(g);
    if (g->switch_out) {
        switch_stream_set_video_size(g->switch_out, (uint16_t)w, (uint16_t)h);
        switch_stream_announce_stream(g->switch_out, (uint16_t)w, (uint16_t)h,
                                      (uint8_t)g->switch_codec);
        announce_shared(g);
    }
    fprintf(stderr, "gst_webrtc: native stream now %dx%d@%d, %d kbps\n", w, h, fps,
            bitrate_kbps > 0 ? bitrate_kbps : SWITCH_VIDEO_BITRATE_KBPS);
}

/* Chooses which of the two native encoders is fed. Nothing is torn down:
 * switching back costs a keyframe, not a pipeline rebuild. */
static void on_switch_codec_request(void *ctx, int codec) {
    GstWebrtcStream *g = ctx;
    if (!g) {
        return;
    }
    if (codec != C2S_CODEC_VP8 && codec != C2S_CODEC_H264) {
        return;
    }
    if (g->switch_codec == codec) {
        return;
    }
    /* Nothing is switched over in the pipeline: which appsrc gets fed is
     * what decides, and that is read from here on the next captured
     * frame. The chain not being fed simply goes quiet. */
    g->switch_codec = codec;

    /* The newly fed chain has been encoding nothing, so its next picture
     * would be predicted from one the client never saw. */
    on_switch_keyframe_request(g);

    /* The client is told rather than left to assume: the change takes
     * effect some frames after the request, and a decoder re-initialised
     * at the wrong moment sees the tail of the old codec. */
    if (g->switch_out) {
        switch_stream_announce_stream(g->switch_out, (uint16_t)g->switch_width,
                                      (uint16_t)g->switch_height, (uint8_t)codec);
        announce_shared(g);
    }
    fprintf(stderr, "gst_webrtc: native stream now %s\n",
            codec == C2S_CODEC_H264 ? "H.264" : "VP8");
}

void gst_webrtc_stream_set_capture_mjpeg(GstWebrtcStream *g, int mjpeg) {
    if (!g) return;
    mjpeg = mjpeg ? 1 : 0;
    if (g->capture_mjpeg == mjpeg) return;
    g->capture_mjpeg = mjpeg;
    announce_shared(g);
}

void gst_webrtc_stream_set_switch_output(GstWebrtcStream *g, SwitchStream *out) {
    if (!g) {
        return;
    }
    g->switch_out = out;
    switch_stream_set_keyframe_request(out, on_switch_keyframe_request, g);
    switch_stream_set_profile_request(out, on_switch_profile_request, g);
    switch_stream_set_codec_request(out, on_switch_codec_request, g);
    announce_shared(g);
}

/* Scales the captured frame to the native client's size and pushes it.
 * Separate from push_i420() because the two differ in output size and in
 * which appsrc they feed, and sharing one function would mean a
 * converter rebuilt on every alternate call. */
void gst_webrtc_stream_push_video_switch(GstWebrtcStream *g, const uint8_t *const plane[3],
                                          const int stride[3], int av_pixel_format,
                                          int width, int height) {
    if (!g) {
        return;
    }
    /* Only the chain for the codec in use is fed; the other one sits at
     * zero. This is also what performs the switch -- there is no element
     * to toggle. */
    GstElement *dest = (g->switch_codec == C2S_CODEC_H264) ? g->vsrc_switch264 : g->vsrc_switch;
    if (!dest) {
        return;
    }
    enum AVPixelFormat format = (enum AVPixelFormat)av_pixel_format;
    const int dw = SWITCH_VIDEO_WIDTH, dh = SWITCH_VIDEO_HEIGHT;

    /* The GPU encoders take NV12 and nothing else; x264 and vp8 take
     * I420. Producing the right one here is free -- the conversion from
     * the capture format happens either way -- and saves a second pass
     * over every pixel inside the pipeline. */
    enum AVPixelFormat dst_format =
        (g->switch_codec == C2S_CODEC_H264 && g->switch264_nv12) ? AV_PIX_FMT_NV12
                                                                 : AV_PIX_FMT_YUV420P;

    if (g->sws_switch && (g->sws_switch_src_format != format || g->sws_switch_dst_format != dst_format)) {
        sws_freeContext(g->sws_switch);
        g->sws_switch = NULL;
    }
    if (!g->sws_switch) {
        g->sws_switch = sws_getContext(width, height, format, dw, dh, dst_format,
                                        SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!g->sws_switch) {
            return;
        }
        g->sws_switch_src_format = format;
        g->sws_switch_dst_format = dst_format;
        int hw = (dw + 1) / 2, hh = (dh + 1) / 2;
        g->switch_i420_size = (size_t)dw * dh + 2 * (size_t)hw * hh;
        free(g->switch_i420_buf);
        g->switch_i420_buf = malloc(g->switch_i420_size);
        if (!g->switch_i420_buf) {
            return;
        }
    }

    /* Both layouts start with a full-size Y plane; they differ in what
     * follows it -- two half-size planes for I420, one interleaved
     * half-height plane for NV12 -- and both come to the same total. */
    int hw = (dw + 1) / 2, hh = (dh + 1) / 2;
    uint8_t *dst[4] = {0};
    int dst_stride[4] = {0};
    dst[0] = g->switch_i420_buf;
    dst_stride[0] = dw;
    if (dst_format == AV_PIX_FMT_NV12) {
        dst[1] = dst[0] + (size_t)dw * dh;
        dst_stride[1] = dw;
    } else {
        dst[1] = dst[0] + (size_t)dw * dh;
        dst_stride[1] = hw;
        dst[2] = dst[1] + (size_t)hw * hh;
        dst_stride[2] = hw;
    }

    const uint8_t *src[4] = {plane[0], plane[1], plane[2], NULL};
    int src_strides[4] = {stride[0], stride[1], stride[2], 0};
    sws_scale(g->sws_switch, src, src_strides, 0, height, dst, dst_stride);

    GstClockTime pts = gst_util_uint64_scale(g->switch_frame, GST_SECOND, 60);
    g->switch_frame++;

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, g->switch_i420_size, NULL);
    gst_buffer_fill(buffer, 0, g->switch_i420_buf, g->switch_i420_size);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, 60);
    gst_app_src_push_buffer(GST_APP_SRC(dest), buffer);
}

static void push_i420(GstWebrtcStream *g, const uint8_t *const src_plane[3], const int src_stride[3],
                      enum AVPixelFormat src_format, int width, int height, GstClockTime pts,
                      GstClockTime duration) {
    const int out_w = width;
    const int out_h = height;

    if (g->sws_i420 && g->sws_src_format != src_format) {
        /* The source format changed under us (shouldn't happen, since
         * it is fixed at capture-open time, but a stale converter would
         * silently produce garbage). */
        sws_freeContext(g->sws_i420);
        g->sws_i420 = NULL;
    }
    if (!g->sws_i420) {
        /* vp8enc (software) expects I420: separate Y, U, V planes, each
         * chroma plane at half resolution. Coming from YUYV this is only
         * a chroma subsample -- no colour-space maths -- which is why it
         * costs a fraction of the RGB path (0.40 vs 5.17 ms/frame
         * measured at 1080p). */
        g->sws_i420 = sws_getContext(width, height, src_format, out_w, out_h, AV_PIX_FMT_YUV420P,
                                      SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!g->sws_i420) {
            return;
        }
        g->sws_src_format = src_format;
        int half_w = (out_w + 1) / 2;
        int half_h = (out_h + 1) / 2;
        g->i420_size = (size_t)out_w * out_h + 2 * (size_t)half_w * half_h;
        g->i420_buf = malloc(g->i420_size);
        if (!g->i420_buf) {
            return;
        }
    }

    int half_w = (out_w + 1) / 2;
    int half_h = (out_h + 1) / 2;
    uint8_t *dst[4] = {0};
    int dst_stride[4] = {0};
    dst[0] = g->i420_buf;
    dst_stride[0] = out_w;
    dst[1] = dst[0] + (size_t)out_w * out_h;
    dst_stride[1] = half_w;
    dst[2] = dst[1] + (size_t)half_w * half_h;
    dst_stride[2] = half_w;

    const uint8_t *src[4] = {src_plane[0], src_plane[1], src_plane[2], NULL};
    int src_strides[4] = {src_stride[0], src_stride[1], src_stride[2], 0};
    sws_scale(g->sws_i420, src, src_strides, 0, height, dst, dst_stride);

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, g->i420_size, NULL);
    gst_buffer_fill(buffer, 0, g->i420_buf, g->i420_size);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = duration;
    gst_app_src_push_buffer(GST_APP_SRC(g->vsrc_vp8), buffer);
}

void gst_webrtc_stream_push_video(GstWebrtcStream *g, const uint8_t *const plane[3],
                                  const int stride[3], int av_pixel_format, int width, int height) {
    if (!g) {
        return;
    }
    /* Nothing is converted or pushed until a client is actually
     * connected: with no viewer the encoder chain stays at 0% CPU. */
    GstClockTime pts = gst_util_uint64_scale(g->video_frame, GST_SECOND, 60);
    GstClockTime duration = gst_util_uint64_scale(1, GST_SECOND, 60);
    g->video_frame++;

    if (g->vp8_active) {
        push_i420(g, plane, stride, (enum AVPixelFormat)av_pixel_format, width, height, pts, duration);
    }
}

void gst_webrtc_stream_push_audio(GstWebrtcStream *g, const int16_t *pcm_interleaved, size_t frames) {
    if (!g) {
        return;
    }
    size_t bytes = frames * (size_t)g->audio_channels * sizeof(int16_t);
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, bytes, NULL);
    gst_buffer_fill(buffer, 0, pcm_interleaved, bytes);
    GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(g->audio_frames, GST_SECOND, g->audio_rate);
    GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(frames, GST_SECOND, g->audio_rate);
    g->audio_frames += frames;
    gst_app_src_push_buffer(GST_APP_SRC(g->asrc), buffer);
}

/* ---- per-client management ---- */

static void teardown_client(WebrtcClient *client);

/* Frees slots whose peer connection is already dead but whose teardown
 * hasn't fired yet.
 *
 * Normal teardown is driven by webrtcbin's connection-state reaching
 * FAILED, which only happens once ICE gives up -- several seconds after
 * the browser actually vanished. That lag is invisible in normal use,
 * but a burst of reconnects (a test loop, or someone reloading
 * repeatedly) can exhaust every slot while several are dead and
 * merely waiting to be noticed. So when we're out of slots, look at the
 * real state before refusing anyone.
 *
 * DISCONNECTED counts as reclaimable here, unlike in the passive path:
 * that state is often transient and worth waiting out normally, but when
 * the alternative is turning a live client away, a possibly-recovering
 * old connection is the better thing to sacrifice.
 *
 * Called from the HTTP connection's own thread (not a GStreamer signal
 * callback), which is the same thread that already builds and links
 * elements in handle_offer() -- so tearing down inline here is no more
 * risky than what that function already does. */
static int reclaim_dead_clients(GstWebrtcStream *g) {
    int reclaimed = 0;
    for (int i = 0; i < g->max_clients; i++) {
        WebrtcClient *client = &g->clients[i];

        SDL_LockMutex(g->clients_mutex);
        GstElement *bin = client->in_use ? client->webrtcbin : NULL;
        if (bin) gst_object_ref(bin);
        SDL_UnlockMutex(g->clients_mutex);
        if (!bin) {
            continue;
        }

        GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
        g_object_get(bin, "connection-state", &state, NULL);
        gst_object_unref(bin);

        if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
            state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED ||
            state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED) {
            fprintf(stderr, "gst_webrtc: reclaiming a dead client slot (state %d) to make room\n", (int)state);
            teardown_client(client);
            reclaimed++;
        }
    }
    return reclaimed;
}

static WebrtcClient *take_free_slot(GstWebrtcStream *g) {
    SDL_LockMutex(g->clients_mutex);
    WebrtcClient *slot = NULL;
    for (int i = 0; i < g->max_clients; i++) {
        if (!g->clients[i].in_use) {
            slot = &g->clients[i];
            memset(slot, 0, sizeof(*slot));
            slot->in_use = 1;
            slot->g = g;
            break;
        }
    }
    SDL_UnlockMutex(g->clients_mutex);
    return slot;
}

static WebrtcClient *acquire_client_slot(GstWebrtcStream *g) {
    WebrtcClient *slot = take_free_slot(g);
    if (slot) {
        return slot;
    }
    /* Full: some of those slots may belong to browsers that are already
     * gone. Check for real before turning this client away. */
    if (reclaim_dead_clients(g) > 0) {
        slot = take_free_slot(g);
    }
    return slot;
}

static void release_client_slot(WebrtcClient *client) {
    GstWebrtcStream *g = client->g;
    /* Whatever this client was holding goes with it. */
    gamepad_bridge_forget(GAMEPAD_SOURCE_BROWSER(client - g->clients));
    SDL_LockMutex(g->clients_mutex);
    client->in_use = 0;
    client->teardown_scheduled = 0;
    SDL_UnlockMutex(g->clients_mutex);
}

int gst_webrtc_stream_get_client_count(GstWebrtcStream *g, int *max_clients) {
    if (!g) {
        if (max_clients) *max_clients = 0;
        return 0;
    }
    if (max_clients) {
        *max_clients = g->max_clients;
    }
    SDL_LockMutex(g->clients_mutex);
    int count = 0;
    for (int i = 0; i < g->max_clients; i++) {
        if (g->clients[i].in_use) count++;
    }
    SDL_UnlockMutex(g->clients_mutex);
    return count;
}

/* Fully dismantles one client's branch and frees its slot.
 *
 * Without this, a slot stayed taken for the process's lifetime: after
 * max_clients connections (reached quickly, since logging in to play
 * deliberately reconnects, and every browser-test run costs one)
 * handle_offer() started refusing everyone until a restart.
 *
 * Runs on g->ctx's thread via an idle source, never directly from the
 * webrtcbin state-change callback that detects the disconnect: tearing
 * elements down from inside a GStreamer signal callback is a classic way
 * to deadlock against the streaming thread.
 *
 * Removing a branch from a *live* pipeline while other clients keep
 * streaming is the delicate part. Two things make it safe here: the
 * elements go to NULL before being unlinked (so nothing is mid-push when
 * the pads are released), and all three tees are created with
 * `allow-not-linked=true` (see the pipeline description), so a tee left
 * with zero branches -- the last client leaving -- keeps running instead
 * of erroring the whole pipeline out with NOT_LINKED. */
static void teardown_client(WebrtcClient *client) {
    GstWebrtcStream *g = client->g;

    /* Take a private copy of what we must dismantle, then mark the slot
     * free, so a new client can take it while we clean up the old
     * elements. */
    SDL_LockMutex(g->clients_mutex);
    if (!client->in_use) {
        /* Already gone (e.g. shutdown raced us). */
        client->teardown_scheduled = 0;
        SDL_UnlockMutex(g->clients_mutex);
        return;
    }
    GstElement *webrtcbin = client->webrtcbin;
    GstElement *vqueue = client->vqueue, *aqueue = client->aqueue;
    GstElement *vpay = client->vpay, *apay = client->apay;
    GstPad *vtee_pad = client->vtee_pad, *atee_pad = client->atee_pad;
    GstElement *vtee = client->vtee;
    client->in_use = 0;
    client->teardown_scheduled = 0;
    client->webrtcbin = NULL;
    client->vqueue = client->aqueue = client->vpay = client->apay = NULL;
    client->vtee_pad = client->atee_pad = NULL;
    client->vtee = NULL;
    SDL_UnlockMutex(g->clients_mutex);

    /* Stop everything first: downstream (webrtcbin) before upstream, so
     * no element is left pushing into one that has already gone away. */
    if (webrtcbin) gst_element_set_state(webrtcbin, GST_STATE_NULL);
    if (vpay) gst_element_set_state(vpay, GST_STATE_NULL);
    if (apay) gst_element_set_state(apay, GST_STATE_NULL);
    if (vqueue) gst_element_set_state(vqueue, GST_STATE_NULL);
    if (aqueue) gst_element_set_state(aqueue, GST_STATE_NULL);

    /* Give the tees their request pads back -- otherwise they'd leak one
     * dangling src pad per departed client. */
    if (vtee && vtee_pad) {
        gst_element_release_request_pad(vtee, vtee_pad);
        gst_object_unref(vtee_pad);
    }
    if (g->atee && atee_pad) {
        gst_element_release_request_pad(g->atee, atee_pad);
        gst_object_unref(atee_pad);
    }

    /* gst_bin_remove() drops the pipeline's reference, which is the last
     * one here, so this is what actually frees the elements. */
    if (vqueue) gst_bin_remove(GST_BIN(g->pipeline), vqueue);
    if (vpay) gst_bin_remove(GST_BIN(g->pipeline), vpay);
    if (aqueue) gst_bin_remove(GST_BIN(g->pipeline), aqueue);
    if (apay) gst_bin_remove(GST_BIN(g->pipeline), apay);
    if (webrtcbin) gst_bin_remove(GST_BIN(g->pipeline), webrtcbin);

    int remaining = gst_webrtc_stream_get_client_count(g, NULL);
    fprintf(stderr, "gst_webrtc: client disconnected, slot freed (%d/%d in use)\n", remaining, g->max_clients);
}

static gboolean teardown_client_idle(gpointer user_data) {
    teardown_client((WebrtcClient *)user_data);
    return G_SOURCE_REMOVE;
}

static void schedule_client_teardown(WebrtcClient *client) {
    GstWebrtcStream *g = client->g;

    SDL_LockMutex(g->clients_mutex);
    int schedule = client->in_use && !client->teardown_scheduled;
    if (schedule) {
        client->teardown_scheduled = 1;
    }
    SDL_UnlockMutex(g->clients_mutex);
    if (!schedule) {
        return;
    }

    GSource *source = g_idle_source_new();
    g_source_set_callback(source, teardown_client_idle, client, NULL);
    g_source_attach(source, g->ctx);
    g_source_unref(source);
}

/* Watches a client's peer-connection state to notice when the browser
 * has gone away.
 *
 * FAILED/CLOSED only, deliberately not DISCONNECTED: that one is
 * routinely transient (a brief network blip) and ICE often recovers from
 * it on its own, so tearing down there would kill sessions that were
 * about to come back. A closed tab reaches FAILED within seconds anyway,
 * once ICE gives up. */
static void on_connection_state_notify(GstElement *webrtcbin, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    WebrtcClient *client = user_data;
    GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
    g_object_get(webrtcbin, "connection-state", &state, NULL);
    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
        state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
        schedule_client_teardown(client);
    }
}

/* Links an encoding chain (tee, placed right after the raw encoder) into
 * webrtcbin for ONE client: tee -> queue -> RTP payloader DEDICATED to
 * this client -> webrtcbin. The payloader is created here, not shared,
 * which lets each client have its own negotiated payload number AND its
 * own RTP session (sequence/SSRC/base timestamp) -- two simultaneous
 * clients no longer step on each other, even if they negotiated
 * different PTs for the same codec. Only the encoding (CPU-expensive)
 * stays shared upstream of the tee. */

/* Link MTU of the interface this machine would use to reach `peer`, or 0
 * if it cannot be determined.
 *
 * Rather than parsing routing tables, this leans on the kernel: connect()
 * on a UDP socket sends nothing but does run the route lookup, so
 * getsockname() then reports the source address the kernel picked. That
 * address identifies the outgoing interface, whose MTU is one ioctl
 * away. */
static guint link_mtu_toward(const struct sockaddr *peer, socklen_t peer_len) {
    if (!peer) {
        return 0;
    }
    int s = socket(peer->sa_family, SOCK_DGRAM, 0);
    if (s < 0) {
        return 0;
    }
    struct sockaddr_storage local;
    socklen_t local_len = sizeof(local);
    if (connect(s, peer, peer_len) != 0 || getsockname(s, (struct sockaddr *)&local, &local_len) != 0) {
        close(s);
        return 0;
    }
    close(s);

    struct ifaddrs *ifas = NULL;
    if (getifaddrs(&ifas) != 0) {
        return 0;
    }
    guint mtu = 0;
    for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != local.ss_family) {
            continue;
        }
        int same = 0;
        if (local.ss_family == AF_INET) {
            same = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr ==
                   ((struct sockaddr_in *)&local)->sin_addr.s_addr;
        } else if (local.ss_family == AF_INET6) {
            same = memcmp(&((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr,
                          &((struct sockaddr_in6 *)&local)->sin6_addr, sizeof(struct in6_addr)) == 0;
        }
        if (!same) {
            continue;
        }
        int q = socket(AF_INET, SOCK_DGRAM, 0);
        if (q >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifa->ifa_name);
            if (ioctl(q, SIOCGIFMTU, &ifr) == 0) {
                mtu = (guint)ifr.ifr_mtu;
            }
            close(q);
        }
        break;
    }
    freeifaddrs(ifas);
    return mtu;
}

/* RTP packet size to use for one client.
 *
 * An explicit RTP_MTU in the .env always wins. Otherwise the size is
 * derived from the link that actually leads to this client, which is how
 * a viewer arriving over a 1280-byte tunnel gets smaller packets than one
 * on the LAN -- each client has its own payloader, so they can differ.
 *
 * Detection only ever lowers the value, never raises it above
 * DEFAULT_RTP_MTU: what we can measure is OUR first hop, not the whole
 * path. The far end may sit behind a link tighter than ours (PPPoE at
 * 1492, mobile, a VPN of their own), and 1200 is precisely the value
 * libwebrtc uses for the browser's own RTP because it survives those.
 * Sizing up to a locally-observed 1500 would re-create the exact failure
 * this exists to prevent. */
static guint rtp_mtu_for_peer(const struct sockaddr *peer, socklen_t peer_len) {
    char forced[32];
    if (config_get("RTP_MTU", forced, sizeof(forced)) && forced[0] &&
        g_ascii_strcasecmp(forced, "auto") != 0) {
        return (guint)config_get_int("RTP_MTU", DEFAULT_RTP_MTU, MIN_RTP_MTU, MAX_RTP_MTU);
    }

    guint link = link_mtu_toward(peer, peer_len);
    if (link == 0) {
        return DEFAULT_RTP_MTU;
    }
    guint mtu = link > RTP_HEADER_ALLOWANCE ? link - RTP_HEADER_ALLOWANCE : MIN_RTP_MTU;
    if (mtu > DEFAULT_RTP_MTU) {
        mtu = DEFAULT_RTP_MTU;
    }
    if (mtu < MIN_RTP_MTU) {
        mtu = MIN_RTP_MTU;
    }
    /* Always logged: when a remote viewer reports "no video", this line
     * is the first thing worth looking at. */
    fprintf(stderr, "gst_webrtc: peer reached over a %u-byte link, RTP packet size %u\n", link, mtu);
    return mtu;
}

/* Creates the webrtcbin on gst_thread_main()'s thread, where g->ctx is
 * genuinely the thread-default, so webrtcbin captures it.
 *
 * g_main_context_invoke_sync() runs the callback inline if this thread
 * already owns the context, so it is also correct (and deadlock-free)
 * when called from the gst thread itself. */
/* GLib has no synchronous invoke, so this is one: attach the work to
 * g->ctx and wait for it. The timeout matters -- if that context has no
 * loop running it (the gst thread starting up or shutting down), the
 * idle source would never fire and this would block a client's HTTP
 * request forever. */
#define MAKE_WEBRTCBIN_TIMEOUT_US (5 * G_USEC_PER_SEC)

struct make_webrtcbin_req {
    GstElement *result;
    int done;
    int abandoned; /* the waiter gave up; the callback owns the struct */
    GMutex lock;
    GCond cond;
};

static gboolean make_webrtcbin_cb(gpointer data) {
    struct make_webrtcbin_req *req = data;
    GstElement *e = gst_element_factory_make("webrtcbin", NULL);

    g_mutex_lock(&req->lock);
    if (req->abandoned) {
        g_mutex_unlock(&req->lock);
        if (e) {
            gst_object_unref(e);
        }
        g_mutex_clear(&req->lock);
        g_cond_clear(&req->cond);
        g_free(req);
        return G_SOURCE_REMOVE;
    }
    req->result = e;
    req->done = 1;
    g_cond_signal(&req->cond);
    g_mutex_unlock(&req->lock);
    return G_SOURCE_REMOVE;
}

static GstElement *make_webrtcbin_on_gst_thread(GstWebrtcStream *g) {
    struct make_webrtcbin_req *req = g_new0(struct make_webrtcbin_req, 1);
    g_mutex_init(&req->lock);
    g_cond_init(&req->cond);

    /* Runs inline when this thread already owns the context, so this is
     * also correct when called from the gst thread itself. */
    g_main_context_invoke(g->ctx, make_webrtcbin_cb, req);

    gint64 deadline = g_get_monotonic_time() + MAKE_WEBRTCBIN_TIMEOUT_US;
    g_mutex_lock(&req->lock);
    while (!req->done) {
        if (!g_cond_wait_until(&req->cond, &req->lock, deadline)) {
            break;
        }
    }
    if (req->done) {
        GstElement *out = req->result;
        g_mutex_unlock(&req->lock);
        g_mutex_clear(&req->lock);
        g_cond_clear(&req->cond);
        g_free(req);
        return out;
    }
    /* Hand the struct to the callback, which may still run later. */
    req->abandoned = 1;
    g_mutex_unlock(&req->lock);
    fprintf(stderr, "gst_webrtc: gst loop did not answer; webrtcbin built off its thread -- "
                    "gamepad latency may suffer\n");
    return gst_element_factory_make("webrtcbin", NULL);
}

static gboolean link_tee_into_webrtcbin(GstElement *pipeline, GstElement *tee, GstElement *webrtcbin,
                                         const char *payloader_factory, int pt, guint rtp_mtu,
                                         const char *rtp_caps_str, GstElement **queue_out,
                                         GstElement **payloader_out, GstPad **tee_pad_out) {
    GstElement *queue = gst_element_factory_make("queue", NULL);
    GstElement *payloader = gst_element_factory_make(payloader_factory, NULL);
    if (!queue || !payloader) {
        if (queue) gst_object_unref(queue);
        if (payloader) gst_object_unref(payloader);
        return FALSE;
    }
    g_object_set(payloader, "pt", pt, "mtu", rtp_mtu, NULL);
    gst_bin_add_many(GST_BIN(pipeline), queue, payloader, NULL);

    GstPad *tee_pad = gst_element_request_pad_simple(tee, "src_%u");
    GstPad *queue_sink = gst_element_get_static_pad(queue, "sink");
    gst_pad_link(tee_pad, queue_sink);
    gst_object_unref(queue_sink);

    if (!gst_element_link(queue, payloader)) {
        gst_object_unref(tee_pad);
        return FALSE;
    }

    /* We provide the RTP caps explicitly at pad-request time rather than
     * letting a caps event propagate asynchronously through the queue:
     * otherwise create-answer (called right after) can run before the
     * caps reach webrtcbin, which then marks the transceiver "inactive"
     * in its answer -- "connected" transport-side negotiation but no
     * media actually flowing. */
    GstCaps *caps = gst_caps_from_string(rtp_caps_str);
    GstPadTemplate *tmpl = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(webrtcbin), "sink_%u");
    GstPad *webrtc_sink = gst_element_request_pad(webrtcbin, tmpl, NULL, caps);
    gst_caps_unref(caps);
    if (!webrtc_sink) {
        gst_object_unref(tee_pad);
        return FALSE;
    }

    GstPad *pay_src = gst_element_get_static_pad(payloader, "src");
    gst_pad_link(pay_src, webrtc_sink);
    gst_object_unref(pay_src);
    gst_object_unref(webrtc_sink);

    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(payloader);

    *queue_out = queue;
    *payloader_out = payloader;
    *tee_pad_out = tee_pad;
    return TRUE;
}

/* Debug: prints the m=<media> section (up to the next m= line or the end)
 * of an SDP text to stderr, prefixed by `label`. Used to compare what
 * different browsers (Firefox/Chrome...) actually negotiate for audio, on
 * both the offer AND the answer. */
/* Fifty-odd lines of SDP per negotiation. Indispensable when a codec or
 * a candidate is being argued about, noise every other time. */
static void dump_media_section(const char *label, const char *sdp, const char *media) {
    if (!app_verbose()) {
        return;
    }
    char needle[16];
    snprintf(needle, sizeof(needle), "m=%s", media);
    const char *start = strstr(sdp, needle);
    if (!start) {
        fprintf(stderr, "%s: no m=%s section\n", label, media);
        return;
    }
    const char *next = strstr(start + 2, "\nm=");
    size_t len = next ? (size_t)(next - start) : strlen(start);
    fprintf(stderr, "%s:\n%.*s\n", label, (int)len, start);
}

static int parse_payload_type_for_codec(const char *sdp, const char *codec, int clock_rate) {
    const char *p = sdp;
    char needle[64];
    snprintf(needle, sizeof(needle), "%s/%d", codec, clock_rate);

    while ((p = strstr(p, "a=rtpmap:")) != NULL) {
        p += strlen("a=rtpmap:");

        char *end = NULL;
        long pt = strtol(p, &end, 10);
        if (end == p || pt < 0 || pt > 127 || *end != ' ') {
            continue;
        }

        const char *line_end = strchr(end, '\n');
        size_t line_len = line_end ? (size_t)(line_end - end) : strlen(end);
        char line[256];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, end, line_len);
        line[line_len] = '\0';

        if (strcasestr(line, needle)) {
            return (int)pt;
        }
    }

    return -1;
}

/* Receiving a binary message on the "gamepad" DataChannel: the browser
 * sends the current gamepad state there (Gamepad API), already formatted
 * on the JS side in the order expected by gamepad_bridge (one signed byte
 * per GAMEPAD_XB360_* entry). We just relay it as-is -- no transformation
 * on the server side. */
static void on_gamepad_message(GstWebRTCDataChannel *channel, GBytes *bytes, gpointer user_data) {
    (void)channel;
    const WebrtcClient *client = user_data;
    /* THE access control point: a viewer's messages are dropped right
     * here, before ever reaching the console. Browser-side UI gating is
     * only a convenience -- devtools can write to this DataChannel
     * directly, so this server-side check is what actually enforces it. */
    if (!client || !client->may_control) {
        return;
    }
    gsize size = 0;
    const uint8_t *data = g_bytes_get_data(bytes, &size);
    if (!data || size != GAMEPAD_BRIDGE_STATE_COUNT) {
        return;
    }
    int8_t state[GAMEPAD_BRIDGE_STATE_COUNT];
    memcpy(state, data, GAMEPAD_BRIDGE_STATE_COUNT);
    /* Per client, not as the one and only hand on the pad: the page
     * sends its state on every animation frame whether or not anything
     * changed, so a second player sitting on the page with nothing
     * plugged in used to wipe the first player's input sixty times a
     * second. */
    gamepad_bridge_update(GAMEPAD_SOURCE_BROWSER(client - client->g->clients), state);
}

/* The browser creates its own DataChannel ("gamepad") before its offer;
 * webrtcbin notifies us here once SCTP negotiation is complete. We do
 * nothing special with the channel reference beyond wiring up reception
 * -- webrtcbin keeps ownership of its lifecycle. The client slot is
 * passed along so on_gamepad_message() can check its may_control flag;
 * slots live for the whole process lifetime (a fixed array in
 * GstWebrtcStream, only ever marked free/in-use), so the pointer stays
 * valid for as long as the channel can fire. */
static void on_data_channel(GstElement *webrtcbin, GstWebRTCDataChannel *channel, gpointer user_data) {
    (void)webrtcbin;
    g_signal_connect(channel, "on-message-data", G_CALLBACK(on_gamepad_message), user_data);
}

void gst_webrtc_stream_set_video_bitrate(GstWebrtcStream *g, int bitrate_kbps) {
    if (!g) {
        return;
    }
    if (bitrate_kbps < 1000) {
        bitrate_kbps = 1000;
    } else if (bitrate_kbps > 50000) {
        bitrate_kbps = 50000;
    }

    g->video_bitrate_kbps = bitrate_kbps;
    if (g->venc_vp8) {
        g_object_set(g->venc_vp8, "target-bitrate", bitrate_kbps * 1000, NULL);
    }
    fprintf(stderr, "gst_webrtc: video bitrate: %d kbps\n", bitrate_kbps);
}

char *gst_webrtc_stream_handle_offer(GstWebrtcStream *g, const char *offer_sdp, int may_control,
                                     const struct sockaddr *peer, socklen_t peer_len) {
    if (!g || !offer_sdp) {
        return NULL;
    }

    WebrtcClient *client = acquire_client_slot(g);
    if (!client) {
        fprintf(stderr, "gst_webrtc_stream_handle_offer: too many connected clients\n");
        return NULL;
    }
    /* Set before the DataChannel can exist (it only appears once
     * negotiation below completes), so on_gamepad_message() always sees
     * the final value. */
    client->may_control = may_control ? 1 : 0;

    /* Push gst_thread_main()'s private, dedicated GLib context as
     * thread-default just for the time it takes to build webrtcbin:
     * that's when webrtcbin captures the context on which it will
     * schedule its future internal signals ("on-data-channel",
     * "on-message-data"...). Without this (HTTP thread different from
     * the gst_thread_main thread, so no thread-default pushed), webrtcbin
     * would use the global default context -- the same one the GTK loop
     * uses on its own thread -- and our gamepad DataChannel messages
     * would end up waiting their turn behind GTK rendering/docking
     * polling instead of being handled by gst_thread_main()'s dedicated
     * loop, which does nothing else. Once webrtcbin is built, its context
     * is fixed: no need to keep this one pushed any longer than its
     * creation.
     *
     * This used to be done by pushing g->ctx as thread-default right
     * here, from the HTTP thread -- which cannot work and never did.
     * g_main_context_push_thread_default() acquires the context, and
     * gst_thread_main() already owns g->ctx for as long as its
     * g_main_loop_run() lasts; a second thread's acquire fails. GLib
     * said so on every single connection:
     *
     *   g_main_context_push_thread_default: assertion 'acquired_context' failed
     *
     * The push was therefore a no-op, webrtcbin captured the GLOBAL
     * default context, and gamepad messages went back to queueing behind
     * GTK. Building it on the thread that owns the context is the way to
     * actually get it. */
    client->webrtcbin = make_webrtcbin_on_gst_thread(g);
    if (!client->webrtcbin) {
        release_client_slot(client);
        return NULL;
    }
    g_object_set(client->webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);
    g_signal_connect(client->webrtcbin, "on-data-channel", G_CALLBACK(on_data_channel), client);
    /* Frees this client's slot + pipeline branch once the browser goes
     * away -- without it a slot is taken for the process's lifetime. */
    g_signal_connect(client->webrtcbin, "notify::connection-state", G_CALLBACK(on_connection_state_notify), client);
    gst_bin_add(GST_BIN(g->pipeline), client->webrtcbin);
    gst_element_sync_state_with_parent(client->webrtcbin);

    GstSDPMessage *sdp_msg = NULL;
    gst_sdp_message_new(&sdp_msg);
    if (gst_sdp_message_parse_buffer((const guint8 *)offer_sdp, (guint)strlen(offer_sdp), sdp_msg) !=
        GST_SDP_OK) {
        fprintf(stderr, "gst_webrtc_stream_handle_offer: invalid SDP offer\n");
        gst_sdp_message_free(sdp_msg);
        release_client_slot(client);
        return NULL;
    }
    GstWebRTCSessionDescription *offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp_msg);

    GstPromise *set_remote_promise = gst_promise_new();
    g_signal_emit_by_name(client->webrtcbin, "set-remote-description", offer, set_remote_promise);
    gst_promise_wait(set_remote_promise);
    gst_promise_unref(set_remote_promise);
    gst_webrtc_session_description_free(offer);

    /* VP8 only. Every browser capable of WebRTC offers it, which is why
     * the H264 alternative that used to sit here never actually ran. */
    int video_pt = parse_payload_type_for_codec(offer_sdp, "VP8", 90000);
    int opus_pt = parse_payload_type_for_codec(offer_sdp, "opus", 48000);
    if (video_pt < 0 || opus_pt < 0) {
        fprintf(stderr, "gst_webrtc_stream_handle_offer: required codecs not found (VP8 pt=%d, opus pt=%d)\n",
                video_pt, opus_pt);
        release_client_slot(client);
        return NULL;
    }

    GstElement *video_tee = g->vtee_vp8;
    const char *video_codec_name = "VP8";
    const char *video_pay_factory = "rtpvp8pay";
    char video_caps_str[128];
    char audio_caps_str[128];
    snprintf(video_caps_str, sizeof(video_caps_str), "application/x-rtp,media=video,encoding-name=%s,payload=%d,clock-rate=90000",
             video_codec_name, video_pt);
    snprintf(audio_caps_str, sizeof(audio_caps_str), "application/x-rtp,media=audio,encoding-name=OPUS,payload=%d,clock-rate=48000",
             opus_pt);
    /* Each client gets its own RTP payloader (created further below in
     * link_tee_into_webrtcbin), configured with ITS OWN negotiated
     * payload number: no more possible conflict between clients that
     * would negotiate different PTs for the same codec, and each has its
     * own independent RTP session (sequence/SSRC). */
    fprintf(stderr, "gst_webrtc_stream_handle_offer: chosen video codec: %s pt=%d, opus pt=%d\n",
            video_codec_name, video_pt, opus_pt);
    dump_media_section("offer: audio", offer_sdp, "audio");

    /* create-answer decides each transceiver's codec/direction from its
     * "codec-preferences" -- not from the caps of the pad it will be
     * linked to (which may not yet have had time to propagate
     * asynchronously through the queue). Without this, create-answer
     * marks the transceivers "inactive": "connected" transport-side
     * negotiation, but no media actually flowing. */
    GArray *transceivers = NULL;
    g_signal_emit_by_name(client->webrtcbin, "get-transceivers", &transceivers);
    if (transceivers && transceivers->len >= 2) {
        GstWebRTCRTPTransceiver *vtrans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, 0);
        GstWebRTCRTPTransceiver *atrans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, 1);
        /* No "payload=" here: the payload number is chosen by the browser
         * in its offer (often different from our own pipeline's, e.g.
         * Opus at 109 rather than 97) -- forcing it would make the caps
         * intersection fail and reject the media even when the codec
         * itself matches. */
        char vcaps_str[96];
        snprintf(vcaps_str, sizeof(vcaps_str), "application/x-rtp,media=video,encoding-name=%s,clock-rate=90000",
                 video_codec_name);
        GstCaps *vcaps = gst_caps_from_string(vcaps_str);
        GstCaps *acaps = gst_caps_from_string("application/x-rtp,media=audio,encoding-name=OPUS,clock-rate=48000");
        g_object_set(vtrans, "codec-preferences", vcaps, NULL);
        g_object_set(atrans, "codec-preferences", acaps, NULL);
        gst_caps_unref(vcaps);
        gst_caps_unref(acaps);

        /* Without this, direction stays "inactive" in the answer even
         * with correct codec-preferences and a properly linked pad: the
         * browser never receives anything even though the transport
         * (ICE, DTLS) connects perfectly -- exactly the symptom observed
         * ("connecting" forever / black screen). The browser offers
         * "recvonly" (it only wants to receive); we answer "sendonly" (we
         * only send), which is the expected standard answer. */
        g_object_set(vtrans, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL);
        g_object_set(atrans, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL);
    }
    if (transceivers) {
        g_array_unref(transceivers);
    }

    g->vp8_active = 1;

    client->vtee = video_tee; /* remembered so teardown can release the pad */
    guint rtp_mtu = rtp_mtu_for_peer(peer, peer_len);
    if (!link_tee_into_webrtcbin(g->pipeline, video_tee, client->webrtcbin, video_pay_factory, video_pt, rtp_mtu,
                                  video_caps_str, &client->vqueue, &client->vpay,
                                  &client->vtee_pad) ||
        !link_tee_into_webrtcbin(g->pipeline, g->atee, client->webrtcbin, "rtpopuspay", opus_pt, rtp_mtu,
                                  audio_caps_str, &client->aqueue, &client->apay, &client->atee_pad)) {
        fprintf(stderr, "gst_webrtc_stream_handle_offer: failed to link tee->webrtcbin\n");
        gst_element_set_state(client->webrtcbin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(g->pipeline), client->webrtcbin);
        release_client_slot(client);
        return NULL;
    }

    GstPromise *answer_promise = gst_promise_new();
    g_signal_emit_by_name(client->webrtcbin, "create-answer", NULL, answer_promise);
    gst_promise_wait(answer_promise);
    const GstStructure *reply = gst_promise_get_reply(answer_promise);
    if (!reply) {
        fprintf(stderr, "gst_webrtc_stream_handle_offer: create-answer returned no reply\n");
        gst_promise_unref(answer_promise);
        release_client_slot(client);
        return NULL;
    }
    GstWebRTCSessionDescription *answer = NULL;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    gst_promise_unref(answer_promise);
    if (!answer) {
        fprintf(stderr, "gst_webrtc_stream_handle_offer: no SDP answer\n");
        release_client_slot(client);
        return NULL;
    }

    GstPromise *set_local_promise = gst_promise_new();
    g_signal_emit_by_name(client->webrtcbin, "set-local-description", answer, set_local_promise);
    gst_promise_wait(set_local_promise);
    gst_promise_unref(set_local_promise);
    gst_webrtc_session_description_free(answer);

    /* No trickle ICE: we simply wait for gathering to finish before
     * returning a complete SDP answer (candidates already included).
     * Simpler than a bidirectional signaling channel, at the cost of a
     * bit of latency on connection setup only (not on the stream once
     * connected). */
    GstWebRTCICEGatheringState gstate = GST_WEBRTC_ICE_GATHERING_STATE_NEW;
    for (int i = 0; i < 300; i++) {
        g_object_get(client->webrtcbin, "ice-gathering-state", &gstate, NULL);
        if (gstate == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) {
            break;
        }
        g_usleep(10000);
    }

    GstWebRTCSessionDescription *final_desc = NULL;
    g_object_get(client->webrtcbin, "local-description", &final_desc, NULL);
    if (!final_desc) {
        fprintf(stderr, "gst_webrtc_stream_handle_offer: no final local description\n");
        release_client_slot(client);
        return NULL;
    }

    gchar *sdp_text = gst_sdp_message_as_text(final_desc->sdp);
    if (sdp_text) {
        dump_media_section("answer: audio", sdp_text, "audio");
        dump_media_section("answer: video", sdp_text, "video");
    }
    char *result = sdp_text ? strdup(sdp_text) : NULL;
    g_free(sdp_text);
    gst_webrtc_session_description_free(final_desc);

    return result;
}
