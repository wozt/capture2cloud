/* Unit tests for the "has the picture changed?" watch in
 * video_capture.c.
 *
 * This is what decides when to re-enumerate the adapter after waking the
 * console. Getting it wrong is expensive to discover on hardware: too
 * eager and the adapter re-handshakes while the console is still asleep
 * (which is the bug this replaces), too reluctant and the gamepad never
 * comes back at all. Neither is testable without power-cycling a real
 * console, so the logic is exercised here with synthetic frames.
 *
 * The .c file is #included directly so its statics are reachable.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "../../video_capture.c"

#include "test_util.h"

#define FRAME_BYTES (1920 * 2 * 1080)

/* A frame of one flat value: stands in for the card's own "no signal"
 * pattern, which is generated internally and therefore identical frame
 * to frame. */
static void fill_flat(uint8_t *buf, uint8_t value) {
    memset(buf, value, FRAME_BYTES);
}

/* Changes `fraction` of the samples the watch actually looks at, by
 * stepping through the buffer the same way sample_frame() does. */
static void change_fraction(uint8_t *buf, int samples_to_change, uint8_t to) {
    size_t step = FRAME_BYTES / CHANGE_SAMPLES;
    for (int i = 0; i < samples_to_change; i++) {
        buf[i * step] = to;
    }
}

int main(void) {
    uint8_t *frame = malloc(FRAME_BYTES);
    if (!frame) {
        printf("out of memory\n");
        return 1;
    }

    t_begin("a still picture is not a change");
    video_capture_watch_for_change();
    fill_flat(frame, 0x40);
    change_watch_feed(frame, FRAME_BYTES); /* becomes the reference */
    for (int i = 0; i < 120; i++) {
        change_watch_feed(frame, FRAME_BYTES);
    }
    t_ok("two seconds of the identical frame report nothing",
         !video_capture_take_change_detected());

    t_begin("a real change is caught, a one-frame glitch is not");
    video_capture_watch_for_change();
    fill_flat(frame, 0x40);
    change_watch_feed(frame, FRAME_BYTES);

    /* One odd frame: the card does emit corrupt ones, and a single
     * frame must not be enough to fire. */
    uint8_t *glitch = malloc(FRAME_BYTES);
    fill_flat(glitch, 0x40);
    change_fraction(glitch, CHANGE_SAMPLES, 0xF0);
    change_watch_feed(glitch, FRAME_BYTES);
    t_ok("one changed frame is not enough", !video_capture_take_change_detected());

    /* Sustained: the console has started drawing. */
    for (int i = 0; i < CHANGE_MIN_FRAMES; i++) {
        change_watch_feed(glitch, FRAME_BYTES);
    }
    t_ok("a sustained change fires", video_capture_take_change_detected());
    t_ok("and it is reported only once", !video_capture_take_change_detected());

    t_begin("the watch disarms itself once it has fired");
    /* Further frames must not re-fire: the reset is a one-shot per wake,
     * and re-enumerating the adapter mid-game would be worse than doing
     * nothing. */
    for (int i = 0; i < 60; i++) {
        change_watch_feed(glitch, FRAME_BYTES);
    }
    t_ok("no further reports", !video_capture_take_change_detected());

    t_begin("a change too small to matter is ignored");
    video_capture_watch_for_change();
    fill_flat(frame, 0x40);
    change_watch_feed(frame, FRAME_BYTES);
    uint8_t *tiny = malloc(FRAME_BYTES);
    fill_flat(tiny, 0x40);
    change_fraction(tiny, CHANGE_MIN_SAMPLES - 1, 0xF0);
    for (int i = 0; i < 30; i++) {
        change_watch_feed(tiny, FRAME_BYTES);
    }
    t_ok("below the sample threshold, nothing fires", !video_capture_take_change_detected());

    /* Brightness drifting by a hair is not the console waking up. */
    video_capture_watch_for_change();
    fill_flat(frame, 0x40);
    change_watch_feed(frame, FRAME_BYTES);
    uint8_t *dim = malloc(FRAME_BYTES);
    fill_flat(dim, 0x40 + CHANGE_SAMPLE_DELTA - 1);
    for (int i = 0; i < 30; i++) {
        change_watch_feed(dim, FRAME_BYTES);
    }
    t_ok("a change below the per-sample delta is noise", !video_capture_take_change_detected());

    t_begin("waiting forever is not an option");
    /* A wake whose picture never changes must still get its follow-up,
     * or the adapter is left un-reset with nothing to say so. */
    video_capture_watch_for_change();
    fill_flat(frame, 0x40);
    change_watch_feed(frame, FRAME_BYTES);
    for (int i = 0; i <= CHANGE_TIMEOUT_FRAMES; i++) {
        change_watch_feed(frame, FRAME_BYTES);
    }
    t_ok("the watch times out and reports anyway", video_capture_take_change_detected());

    t_begin("nothing is reported when no watch is armed");
    for (int i = 0; i < 30; i++) {
        change_watch_feed(glitch, FRAME_BYTES);
    }
    t_ok("an unarmed watch stays quiet", !video_capture_take_change_detected());

    free(frame);
    free(glitch);
    free(tiny);
    free(dim);
    return t_report();
}
