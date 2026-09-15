/* Unit tests for which H.264 encoder each chain is built with.
 *
 * There are three H.264 chains -- the console clients, the browsers and
 * the Wii U GamePad -- and a fourth is coming for the Wii U console.
 * They all used to be built with ONE element name chosen at startup, so
 * on a machine with two render nodes every encode landed on one video
 * engine while the other sat idle.
 *
 * What is checked here is the part that can be checked anywhere: that
 * every chain gets a name that exists, that a pinned choice wins, that a
 * bad one is survived rather than carried into the pipeline, and that
 * when there IS more than one engine the chains are actually spread over
 * them. What cannot be checked in a unit test is whether a busy engine
 * hands out another session -- see the note at the bottom.
 *
 * The .c file is #included directly so its `static` functions are
 * reachable; its own deps are supplied by run_all.sh.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>

#include "../../app_config.c"
#include "../../gst_webrtc.c"

#include "gst_webrtc_stubs.h"
#include "test_util.h"

/* Is this one of the names the host knows how to configure? Anything
 * else in the pipeline description would be built with properties that
 * do not exist on it. */
static int is_known_encoder(const char *name) {
    for (size_t i = 0; i < sizeof(H264_ENCODER_PREFERENCE) / sizeof(*H264_ENCODER_PREFERENCE); i++) {
        if (name && strcmp(name, H264_ENCODER_PREFERENCE[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int factory_exists(const char *name) {
    GstElementFactory *f = gst_element_factory_find(name);
    if (f) {
        gst_object_unref(f);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    const char *enc[GST_WEBRTC_H264_CHAINS];

    t_begin("the probe answers about opening, not about existing");
    t_eq_int("an element that does not exist does not open",
             h264_encoder_opens("no-such-encoder-12345"), 0);
    t_ok("x264enc is present on any build we support", factory_exists("x264enc"));
    t_eq_int("x264enc opens", h264_encoder_opens("x264enc"), 1);

    t_begin("every chain gets a usable encoder");
    memset(enc, 0, sizeof(enc));
    int engines = gst_webrtc_pick_h264_encoders(NULL, enc, GST_WEBRTC_H264_CHAINS);
    for (int i = 0; i < GST_WEBRTC_H264_CHAINS; i++) {
        t_ok("chain has a name at all", enc[i] != NULL);
        t_ok("the name is one the host can configure", is_known_encoder(enc[i]));
        t_ok("the element actually exists in this build", factory_exists(enc[i]));
    }
    t_ok("engine count is not negative", engines >= 0);

    t_begin("chains are spread over the engines there are");
    if (engines > 1) {
        /* The whole point: two engines, two different names, and the
         * third chain wrapping back round to the first. */
        t_ok("two chains do not share one engine", strcmp(enc[0], enc[1]) != 0);
        t_ok("round-robin wraps", strcmp(enc[2], enc[engines == 2 ? 0 : 2]) == 0);
        for (int i = 0; i < GST_WEBRTC_H264_CHAINS; i++) {
            t_ok("no chain fell back to the CPU while hardware was free",
                 strcmp(enc[i], "x264enc") != 0);
        }
    } else if (engines == 1) {
        t_ok("one engine: every chain uses it",
             strcmp(enc[0], enc[1]) == 0 && strcmp(enc[1], enc[2]) == 0);
        t_ok("and it is hardware, not the CPU", strcmp(enc[0], "x264enc") != 0);
    } else {
        /* No hardware that opens. Not a failure -- a machine without a
         * GPU has to work, it just works on the CPU. */
        t_ok("no hardware: every chain falls to the CPU together",
             strcmp(enc[0], "x264enc") == 0 && strcmp(enc[1], "x264enc") == 0 &&
                 strcmp(enc[2], "x264enc") == 0);
    }

    t_begin("a choice in the .env is a decision, not a hint");
    memset(enc, 0, sizeof(enc));
    t_eq_int("pinning reports no spreading",
             gst_webrtc_pick_h264_encoders("x264enc", enc, GST_WEBRTC_H264_CHAINS), 0);
    t_ok("every chain honours the pinned name",
         strcmp(enc[0], "x264enc") == 0 && strcmp(enc[1], "x264enc") == 0 &&
             strcmp(enc[2], "x264enc") == 0);

    t_begin("a bad choice in the .env is survived");
    /* This is the failure the fallback exists for: a name in the .env
     * that this build does not have used to be written straight into the
     * pipeline description, which then failed to parse -- the whole
     * stream lost over a preference. */
    memset(enc, 0, sizeof(enc));
    int after_missing = gst_webrtc_pick_h264_encoders("not-installed-anywhere", enc,
                                                      GST_WEBRTC_H264_CHAINS);
    for (int i = 0; i < GST_WEBRTC_H264_CHAINS; i++) {
        t_ok("a missing name still leaves a usable chain", factory_exists(enc[i]));
    }
    t_eq_int("and it chose exactly what it would have chosen anyway", after_missing, engines);

    /* An element that exists but is not an encoder: the .env is not
     * wrong about what is installed, it is wrong about what it is for.
     * Writing `bitrate=` on an identity element parses and then fails. */
    memset(enc, 0, sizeof(enc));
    t_ok("identity is present, so this tests the branch it means to",
         factory_exists("identity"));
    gst_webrtc_pick_h264_encoders("identity", enc, GST_WEBRTC_H264_CHAINS);
    for (int i = 0; i < GST_WEBRTC_H264_CHAINS; i++) {
        t_ok("an element that is not an encoder is refused",
             enc[i] && strcmp(enc[i], "identity") != 0);
        t_ok("and something configurable is used instead", is_known_encoder(enc[i]));
    }

    t_begin("asking for fewer chains than there are engines is fine");
    const char *one[1] = { NULL };
    gst_webrtc_pick_h264_encoders(NULL, one, 1);
    t_ok("a single chain gets the preferred engine", one[0] && is_known_encoder(one[0]));
    if (engines > 0) {
        t_ok("which is the first one that opened", strcmp(one[0], enc[0]) == 0 || engines == 0);
    }

    /* NOT tested here, deliberately: whether a video engine that is
     * already encoding will hand out another session. The probe above
     * opens an element to READY, which touches the device but does not
     * allocate an encode session -- that happens when caps are
     * negotiated. Measured on this machine: when a render node is
     * unreachable the VA plugin does not register its element at all, so
     * the factory is already absent and the probe never sees it. The
     * probe earns its place against a stale registry, not against a busy
     * engine. Spreading the chains is what actually answers "take
     * another one if this is busy", and that is what is checked above.
     */
    return t_report();
}
