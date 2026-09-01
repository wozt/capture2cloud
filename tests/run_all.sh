#!/bin/bash
# Runs the whole test suite: the C unit tests (tests/c/) and the
# JavaScript suite (tests/run_tests.js). Exits non-zero if anything
# fails, so it can gate a deploy.
#
# Usage: ./tests/run_all.sh   (from capture2cloud/, or anywhere)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

failed=0

run_c_test() {
    local name="$1"
    shift
    local pkgs="$1"
    shift

    echo "═══ C: $name ═══"
    if ! gcc -O1 -Wall -Wextra -o "$BUILD_DIR/$name" "$SCRIPT_DIR/c/$name.c" \
        $(pkg-config --cflags --libs $pkgs) 2>&1; then
        echo "  COMPILE FAILED"
        failed=1
        return
    fi
    if ! "$BUILD_DIR/$name"; then
        failed=1
    fi
}

# The C tests #include the .c file under test, so they see its static
# functions; the pkg-config sets below are just that file's own deps.
run_c_test test_gamepad_bridge "libusb-1.0 sdl2"
run_c_test test_web_stream_auth "sdl2 gstreamer-1.0"
run_c_test test_change_watch "libjpeg sdl2 libswscale libavcodec libavutil"
run_c_test test_rtp_mtu "sdl2 gstreamer-1.0 gstreamer-video-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 libavcodec libavutil libswscale"

echo
echo "═══ Security (needs the app running) ═══"
if curl -fsS -m 2 "http://127.0.0.1:${WEB_PORT:-5080}/clients" >/dev/null 2>&1; then
    node "$SCRIPT_DIR/security/attack.js" || failed=1
else
    echo "  skipped: nothing listening on 127.0.0.1:${WEB_PORT:-5080}"
fi

echo
echo "═══ Native transport (needs the app running) ═══"
if curl -fsS -m 2 "http://127.0.0.1:${WEB_PORT:-5080}/clients" >/dev/null 2>&1; then
    python3 "$SCRIPT_DIR/native/test_switch_stream.py" || failed=1
else
    echo "  skipped: nothing listening on 127.0.0.1:${WEB_PORT:-5080}"
fi

echo
echo "═══ JavaScript ═══"
if ! node "$SCRIPT_DIR/run_tests.js"; then
    failed=1
fi

echo
if [ "$failed" -eq 0 ]; then
    echo "✅ all suites passed"
else
    echo "❌ some suites failed"
fi
exit "$failed"
