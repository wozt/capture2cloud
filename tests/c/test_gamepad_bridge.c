/* Unit tests for gamepad_bridge.c's pure logic: the GCAPI wire format
 * and value clamping.
 *
 * The .c file is #included directly so its `static` functions are
 * reachable -- the usual way to unit test internals in C. Nothing here
 * opens a device: libusb/SDL are linked but never asked to do anything
 * (no gamepad_bridge_init(), so g_handle stays NULL and no transfer is
 * ever attempted).
 *
 * Why this file exists: the wire format below is exactly where the
 * original "everything is accepted by USB but the console never reacts"
 * bug lived (a 65-byte packet where the firmware wants 64, shifting
 * every field by one). That was expensive to find; these tests pin the
 * layout down so it can't silently regress. */
#include "../../app_config.c"
#include "../../gamepad_bridge.c"

#include "test_util.h"

/* Two people connected at once.
 *
 * There was a single state and whoever wrote last won. The browser sends
 * its state on every animation frame whether or not anything changed, so
 * a second person merely having the page open -- no controller selected,
 * all zeroes -- overwrote the first person's input sixty times a second.
 * Neither of them was playing; both were stuttering. */
static void test_two_players(void) {
    t_begin("two players at once");

    g_state_mutex = SDL_CreateMutex();
    g_connected = 1;
    memset(g_sources, 0, sizeof(g_sources));
    memset(g_latest_state, 0, sizeof(g_latest_state));

    int8_t playing[GAMEPAD_BRIDGE_STATE_COUNT] = {0};
    playing[GAMEPAD_XB360_A] = 100;
    playing[GAMEPAD_XB360_LX] = -80;

    int8_t idle[GAMEPAD_BRIDGE_STATE_COUNT] = {0};

    gamepad_bridge_update(GAMEPAD_SOURCE_NATIVE(0), playing);
    gamepad_bridge_update(GAMEPAD_SOURCE_BROWSER(0), idle);

    t_eq_int("an idle second player does not release A",
             g_latest_state[GAMEPAD_XB360_A], 100);
    t_eq_int("an idle second player does not recentre the stick",
             g_latest_state[GAMEPAD_XB360_LX], -80);

    /* The same press from both is one press, not two: a button is held
     * or it is not, and an axis has one position. */
    gamepad_bridge_update(GAMEPAD_SOURCE_BROWSER(0), playing);
    t_eq_int("the same button from both is still one press",
             g_latest_state[GAMEPAD_XB360_A], 100);
    t_eq_int("the same stick from both is still one deflection",
             g_latest_state[GAMEPAD_XB360_LX], -80);

    /* Both pushing opposite ways is a tie nobody can win; the larger
     * deflection is the least surprising answer, and it keeps the value
     * inside the range a real stick produces. */
    int8_t other_way[GAMEPAD_BRIDGE_STATE_COUNT] = {0};
    other_way[GAMEPAD_XB360_LX] = 40;
    gamepad_bridge_update(GAMEPAD_SOURCE_BROWSER(0), other_way);
    t_eq_int("opposing sticks: the larger deflection wins",
             g_latest_state[GAMEPAD_XB360_LX], -80);

    /* Leaving must release what you were holding, or the console is left
     * with a button pressed by someone who is no longer there. */
    gamepad_bridge_forget(GAMEPAD_SOURCE_NATIVE(0));
    t_eq_int("a departing player releases their button",
             g_latest_state[GAMEPAD_XB360_A], 0);
    t_eq_int("a departing player releases their stick",
             g_latest_state[GAMEPAD_XB360_LX], 40);

    /* Cut off mid-press rather than disconnected cleanly: nothing
     * announces the departure, so the press has to expire. */
    gamepad_bridge_update(GAMEPAD_SOURCE_NATIVE(1), playing);
    t_eq_int("a fresh source is applied", g_latest_state[GAMEPAD_XB360_A], 100);
    for (int i = 0; i < GAMEPAD_MAX_SOURCES; i++) {
        if (g_sources[i].in_use && g_sources[i].key == GAMEPAD_SOURCE_NATIVE(1)) {
            g_sources[i].last_ms -= GAMEPAD_SOURCE_TIMEOUT_MS + 1;
        }
    }
    gamepad_bridge_update(GAMEPAD_SOURCE_BROWSER(0), idle);
    t_eq_int("a source gone silent stops holding the button down",
             g_latest_state[GAMEPAD_XB360_A], 0);

    /* More clients than slots must not corrupt anything: the extras are
     * ignored, and the ones already playing keep working. */
    for (int i = 0; i < GAMEPAD_MAX_SOURCES + 4; i++) {
        gamepad_bridge_update(GAMEPAD_SOURCE_BROWSER(i), idle);
    }
    gamepad_bridge_update(GAMEPAD_SOURCE_BROWSER(0), playing);
    t_eq_int("a full source table still tracks the players in it",
             g_latest_state[GAMEPAD_XB360_A], 100);

    g_connected = 0;
    SDL_DestroyMutex(g_state_mutex);
    g_state_mutex = NULL;
}

static void test_report_layout(void) {
    t_begin("GCAPI report layout");

    unsigned char buf[REPORT_SIZE];
    uint8_t data[GCAPI_OUTPUT_TOTAL] = {0};
    data[0] = 11;
    data[1] = 22;
    data[GCAPI_OUTPUT_TOTAL - 1] = 33;

    build_report(buf, GPPKG_OUTPUT_REPORT, data, GCAPI_OUTPUT_TOTAL);

    /* 64 bytes on the wire, NOT 65: no leading report-id byte. This is
     * the whole point -- see the comment above send_report(). */
    t_eq_int("report size is 64", (long long)sizeof(buf), 64);

    t_eq_int("byte 0 = packet type", buf[0], GPPKG_OUTPUT_REPORT);
    t_eq_int("byte 1 = length low", buf[1], GCAPI_OUTPUT_TOTAL & 0xff);
    t_eq_int("byte 2 = length high", buf[2], (GCAPI_OUTPUT_TOTAL >> 8) & 0xff);
    t_eq_int("byte 3 = 'first' flag", buf[3], 1);

    /* Payload starts at offset 4, immediately after the header. */
    t_eq_int("payload[0] at offset 4", buf[4], 11);
    t_eq_int("payload[1] at offset 5", buf[5], 22);
    t_eq_int("last payload byte", buf[4 + GCAPI_OUTPUT_TOTAL - 1], 33);

    /* Everything past the payload must be zero, not stale memory. */
    int tail_all_zero = 1;
    for (size_t i = 4 + GCAPI_OUTPUT_TOTAL; i < REPORT_SIZE; i++) {
        if (buf[i] != 0) tail_all_zero = 0;
    }
    t_ok("bytes after the payload are zeroed", tail_all_zero);
}

static void test_report_length_encoding(void) {
    t_begin("report length encoding");

    unsigned char buf[REPORT_SIZE];

    /* A length that spans both bytes, to prove the LE16 split is right
     * (a big-endian mistake would swap these two). */
    build_report(buf, GPPKG_OUTPUT_REPORT, NULL, 0x0102);
    t_eq_int("0x0102 low byte", buf[1], 0x02);
    t_eq_int("0x0102 high byte", buf[2], 0x01);

    /* Commands with no payload (ENTER/LEAVE_CAPTURE) still carry a
     * well-formed header. */
    build_report(buf, GPPKG_ENTER_CAPTURE, NULL, 0);
    t_eq_int("empty command type", buf[0], GPPKG_ENTER_CAPTURE);
    t_eq_int("empty command length low", buf[1], 0);
    t_eq_int("empty command length high", buf[2], 0);
    t_eq_int("empty command still sets 'first'", buf[3], 1);
}

static void test_report_is_clean_between_calls(void) {
    t_begin("report buffer reuse");

    unsigned char buf[REPORT_SIZE];
    uint8_t big[GCAPI_OUTPUT_TOTAL];
    memset(big, 0x7f, sizeof(big));

    build_report(buf, GPPKG_OUTPUT_REPORT, big, GCAPI_OUTPUT_TOTAL);
    t_eq_int("first build wrote the payload", buf[4], 0x7f);

    /* Reusing the same buffer for a payload-less command must not leave
     * the previous payload behind -- that would send phantom button
     * presses, which is precisely the class of bug that made "Select"
     * appear permanently held earlier in this project. */
    build_report(buf, GPPKG_LEAVE_CAPTURE, NULL, 0);
    int payload_cleared = 1;
    for (size_t i = 4; i < REPORT_SIZE; i++) {
        if (buf[i] != 0) payload_cleared = 0;
    }
    t_ok("previous payload fully cleared", payload_cleared);
}

static void test_report_payload_is_capped(void) {
    t_begin("oversized payload is capped");

    unsigned char buf[REPORT_SIZE];
    uint8_t huge[200];
    memset(huge, 0xAB, sizeof(huge));

    /* A length larger than the packet can hold must be truncated, not
     * copied past the end of the buffer. */
    build_report(buf, GPPKG_OUTPUT_REPORT, huge, sizeof(huge));
    t_eq_int("payload starts as given", buf[4], 0xAB);
    t_eq_int("last writable byte still in range", buf[REPORT_SIZE - 1], 0xAB);
}

static void test_clamp(void) {
    t_begin("clamp_i8");

    t_eq_int("zero", clamp_i8(0), 0);
    t_eq_int("in range positive", clamp_i8(42), 42);
    t_eq_int("in range negative", clamp_i8(-42), -42);
    t_eq_int("upper bound", clamp_i8(100), 100);
    t_eq_int("lower bound", clamp_i8(-100), -100);
    t_eq_int("above range saturates", clamp_i8(1000), 100);
    t_eq_int("below range saturates", clamp_i8(-1000), -100);
    /* Values that would wrap if cast to int8_t without clamping first
     * (200 -> -56), which is how an out-of-range stick value could turn
     * into movement in the opposite direction. */
    t_eq_int("200 saturates rather than wrapping", clamp_i8(200), 100);
    t_eq_int("-200 saturates rather than wrapping", clamp_i8(-200), -100);
}

int main(void) {
    test_report_layout();
    test_report_length_encoding();
    test_report_is_clean_between_calls();
    test_report_payload_is_capped();
    test_clamp();
    t_begin("adapter unplug/replug");
    /* The send thread runs with no sleep in it, so it has to tell "the
     * adapter is gone, go reconnect" apart from "that transfer failed",
     * or it would either spin logging millions of lines or give up on a
     * recoverable hiccup. */
    t_ok("NO_DEVICE means gone", usb_error_is_disconnect(LIBUSB_ERROR_NO_DEVICE));
    t_ok("NOT_FOUND means gone", usb_error_is_disconnect(LIBUSB_ERROR_NOT_FOUND));
    t_ok("IO means gone", usb_error_is_disconnect(LIBUSB_ERROR_IO));
    t_ok("PIPE means gone", usb_error_is_disconnect(LIBUSB_ERROR_PIPE));
    t_ok("success is not a disconnect", !usb_error_is_disconnect(0));
    t_ok("a timeout is not a disconnect", !usb_error_is_disconnect(LIBUSB_ERROR_TIMEOUT));
    t_ok("a busy device is not a disconnect", !usb_error_is_disconnect(LIBUSB_ERROR_BUSY));

    /* Sending with no handle must report "gone" rather than dereference
     * it -- that is the state the loop is in the instant after an
     * unplug. */
    g_handle = NULL;
    t_eq_int("send with no handle reports NO_DEVICE",
             send_report(GPPKG_OUTPUT_REPORT, NULL, 0), LIBUSB_ERROR_NO_DEVICE);

    /* Closing an already-closed device is what shutdown does after an
     * unplug; it must not crash. */
    usb_close_device();
    t_ok("closing twice is harmless", g_handle == NULL);

    t_begin("reset request");
    /* The reset is performed by the send thread, never by the caller:
     * that thread is using the same handle continuously. */
    g_connected = 0;
    g_reset_requested = 0;
    gamepad_bridge_reset();
    t_ok("no adapter: reset is ignored", g_reset_requested == 0);

    g_connected = 1;
    gamepad_bridge_reset();
    t_ok("adapter present: reset is queued for the send thread", g_reset_requested == 1);
    g_connected = 0;

    test_two_players();

    return t_report();
}
