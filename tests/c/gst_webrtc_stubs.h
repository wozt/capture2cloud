#ifndef GST_WEBRTC_STUBS_H
#define GST_WEBRTC_STUBS_H

/*
 * What gst_webrtc.c calls and a test does not want to link.
 *
 * The tests #include gst_webrtc.c directly so its `static` functions are
 * reachable, which means satisfying every symbol it references. Linking
 * the real ones would drag libusb, sockets, threads and drc-x264 into
 * tests that are about packet arithmetic and element names. This lived
 * inside test_rtp_mtu.c until a second test needed the same seventy
 * lines.
 *
 * Include it AFTER gst_webrtc.c, which is where the types come from.
 */

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
/* The Wii U encode, stubbed for the same reason as the rest: this test
 * is about packet sizes, and drc-x264 is a library that may not be
 * installed. test_drc_encoder.c is where that chain is actually
 * exercised. */
DrcEncoder *drc_encoder_open(const char *so, const char *preset, char *err, size_t n) {
    (void)so; (void)preset;
    if (err && n) snprintf(err, n, "stubbed out in this test");
    return NULL;
}
void drc_encoder_close(DrcEncoder *e) { (void)e; }
int drc_encoder_encode(DrcEncoder *e, const uint8_t *i420, int idr, DrcFrame *out) {
    (void)e; (void)i420; (void)idr; (void)out;
    return -1;
}
int drc_encoder_restart(DrcEncoder *e) { (void)e; return -1; }
const char *drc_encoder_library(const DrcEncoder *e) { (void)e; return ""; }

void switch_stream_send_video(SwitchStream *s, int slot, const uint8_t *d, uint32_t n, int k) {
    (void)s; (void)slot; (void)d; (void)n; (void)k;
}

void switch_stream_announce_stream(SwitchStream *s, int slot, uint16_t w, uint16_t h) {
    (void)s; (void)slot; (void)w; (void)h;
}

int switch_stream_stream_client_count(SwitchStream *s, int slot) {
    (void)s; (void)slot; return 0;
}

void switch_stream_set_demand_changed(SwitchStream *s, void (*cb)(void *ctx), void *ctx) {
    (void)s; (void)cb; (void)ctx;
}

void switch_stream_announce_shared(SwitchStream *s, int slot, uint16_t w, uint16_t h, uint16_t fps,
                                   uint16_t kbps, uint8_t mjpeg) {
    (void)s; (void)slot; (void)w; (void)h; (void)fps; (void)kbps; (void)mjpeg;
}

void switch_stream_set_drc_available(SwitchStream *s, int a) { (void)s; (void)a; }

int switch_stream_stream_codec(SwitchStream *s, int slot) { (void)s; (void)slot; return 0; }

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


#endif
