/* Unit tests for the RTP packet sizing in gst_webrtc.c.
 *
 * This is the logic that got video working over Tailscale: GStreamer's
 * payloaders default to 1400-byte packets, which do not fit a tunnel
 * with a 1280-byte MTU. ICE, DTLS and the gamepad DataChannel all send
 * small packets and keep working, so the connection looks perfectly
 * healthy while every single video packet is dropped.
 *
 * The .c file is #included directly so its `static` functions are
 * reachable; gst_webrtc.c's own deps are supplied by run_all.sh.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>

#include "../../app_config.c"
#include "../../gst_webrtc.c"

#include "test_util.h"

/* gst_webrtc.c forwards gamepad DataChannel messages to the USB bridge.
 * None of that is exercised here, and pulling in gamepad_bridge.c would
 * drag libusb into a test about packet sizes, so a stub satisfies the
 * linker instead. */
void gamepad_bridge_update(unsigned source, const int8_t state[GAMEPAD_BRIDGE_STATE_COUNT]) {
    (void)source; (void)state;
}

void gamepad_bridge_forget(unsigned source) {
    (void)source;
}

/* gst_webrtc.c hands encoded frames to the native transport. None of
 * that is exercised here, and linking switch_stream.c would drag its
 * sockets and threads into a test about packet sizes. */
void switch_stream_send_video(SwitchStream *s, int codec, const uint8_t *d, uint32_t n, int k) {
    (void)s; (void)codec; (void)d; (void)n; (void)k;
}

void switch_stream_announce_stream(SwitchStream *s, uint8_t codec, uint16_t w, uint16_t h) {
    (void)s; (void)codec; (void)w; (void)h;
}

int switch_stream_codec_client_count(SwitchStream *s, int codec) {
    (void)s; (void)codec; return 0;
}

void switch_stream_set_demand_changed(SwitchStream *s, void (*cb)(void *ctx), void *ctx) {
    (void)s; (void)cb; (void)ctx;
}

void switch_stream_announce_shared(SwitchStream *s, uint16_t w, uint16_t h, uint16_t fps,
                                   uint16_t kbps, uint8_t codec, uint8_t mjpeg) {
    (void)s; (void)w; (void)h; (void)fps; (void)kbps; (void)codec; (void)mjpeg;
}

void switch_stream_send_audio(SwitchStream *s, const uint8_t *d, uint32_t n) {
    (void)s; (void)d; (void)n;
}
void switch_stream_set_video_size(SwitchStream *s, uint16_t w, uint16_t h) {
    (void)s; (void)w; (void)h;
}
void switch_stream_set_keyframe_request(SwitchStream *s, SwitchKeyframeRequest cb, void *ctx) {
    (void)s; (void)cb; (void)ctx;
}
void switch_stream_set_profile_request(SwitchStream *s,
                                       void (*cb)(void *ctx, int codec, int w, int h, int fps,
                                                  int kbps),
                                       void *ctx) {
    (void)s; (void)cb; (void)ctx;
}

/* Builds a sockaddr for an address string, so the tests can ask "what
 * would we send to a peer at this address?". */
static int make_addr(struct sockaddr_storage *ss, const char *ip) {
    memset(ss, 0, sizeof(*ss));
    struct sockaddr_in *v4 = (struct sockaddr_in *)ss;
    if (inet_pton(AF_INET, ip, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(9);
        return sizeof(struct sockaddr_in);
    }
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
    if (inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(9);
        return sizeof(struct sockaddr_in6);
    }
    return 0;
}

/* The arithmetic that decides the packet size, isolated from the route
 * lookup: given a link MTU, what do we send? Mirrors rtp_mtu_for_peer's
 * auto branch. Kept here so the "tunnel narrower than Tailscale's"
 * case can be checked on a machine that has no such tunnel. */
static guint size_for_link(guint link) {
    guint mtu = link > RTP_HEADER_ALLOWANCE ? link - RTP_HEADER_ALLOWANCE : MIN_RTP_MTU;
    if (mtu > DEFAULT_RTP_MTU) mtu = DEFAULT_RTP_MTU;
    if (mtu < MIN_RTP_MTU) mtu = MIN_RTP_MTU;
    return mtu;
}

int main(void) {
    t_begin("packet size fits the link it has to cross");
    /* The regression that started all this: a 1280-byte tunnel. The
     * packet plus IPv6+UDP+SRTP headers has to stay under it. */
    t_eq_int("Tailscale/WireGuard 1280 link", size_for_link(1280), 1200);
    t_ok("1280 link: packet + headers fit", size_for_link(1280) + RTP_HEADER_ALLOWANCE <= 1280);
    t_ok("GStreamer's 1400 default would NOT have fit", 1400 + RTP_HEADER_ALLOWANCE > 1280);

    /* A tunnel tighter than Tailscale's is exactly what auto-detection
     * is for -- the fixed 1200 default alone would not cover it. */
    t_eq_int("1100-byte tunnel drops below the default", size_for_link(1100), 1020);
    t_ok("1100 link: packet + headers fit", size_for_link(1100) + RTP_HEADER_ALLOWANCE <= 1100);
    t_eq_int("very small link is floored, not made absurd", size_for_link(300), MIN_RTP_MTU);

    t_begin("detection never sizes UP past the safe default");
    /* We can only measure our own first hop. The far end may sit behind
     * a tighter link, so a locally-observed 1500 must not push packets
     * back up to where they stop crossing other people's networks. */
    t_eq_int("ordinary 1500 LAN stays at the safe value", size_for_link(1500), DEFAULT_RTP_MTU);
    t_eq_int("loopback's enormous MTU stays at the safe value", size_for_link(65536), DEFAULT_RTP_MTU);

    t_begin("route lookup finds the interface toward a peer");
    struct sockaddr_storage ss;
    int len = make_addr(&ss, "127.0.0.1");
    t_ok("built a loopback address", len > 0);
    guint lo_mtu = link_mtu_toward((struct sockaddr *)&ss, (socklen_t)len);
    t_ok("loopback link MTU was detected", lo_mtu > 0);
    t_ok("loopback MTU is at least a normal Ethernet one", lo_mtu >= 1500);

    len = make_addr(&ss, "::1");
    t_ok("built an IPv6 loopback address", len > 0);
    t_ok("IPv6 route lookup also resolves", link_mtu_toward((struct sockaddr *)&ss, (socklen_t)len) > 0);

    t_eq_int("no peer address means no guess", (int)link_mtu_toward(NULL, 0), 0);

    t_begin("a peer we cannot reach falls back rather than failing");
    /* An address with no route must not produce a nonsense size: the
     * caller has to get something sendable either way. */
    len = make_addr(&ss, "203.0.113.7"); /* TEST-NET-3, guaranteed unrouted */
    guint mtu = rtp_mtu_for_peer((struct sockaddr *)&ss, (socklen_t)len);
    t_ok("unreachable peer still yields a usable size", mtu >= MIN_RTP_MTU && mtu <= DEFAULT_RTP_MTU);

    return t_report();
}
