#!/bin/bash
# Runs the whole test suite: the C unit tests (tests/c/) and the
# JavaScript suite (tests/run_tests.js). Exits non-zero if anything
# fails, so it can gate a deploy.
#
# Usage: ./tests/run_all.sh   (from capture2cloud/, or anywhere)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Built once and kept, rather than into a fresh temporary directory every
# run. Each of these #includes a whole project source, so compiling all
# four is several seconds of gcc -- and gcc taking cores is exactly what
# makes the live stream stutter while the tests run. Rebuilt only when
# something they include has actually changed.
BUILD_DIR="$SCRIPT_DIR/.build"
mkdir -p "$BUILD_DIR"

# Nothing here is urgent, and something is usually watching the stream
# while it runs: the encoder must win every time they want the same core.
# `nice` alone is enough for that -- a niced process only gets a core the
# rest of the machine is not asking for. ionice is a bonus where the
# scheduler supports it, and harmless where it does not.
LOW="nice -n 19"
if command -v ionice >/dev/null 2>&1 && ionice -c3 true >/dev/null 2>&1; then
    LOW="ionice -c3 nice -n 19"
fi

failed=0

# How long each suite took, printed at the end. Worth having: "the tests
# take too long" is not actionable until you know which one is taking the
# time, and the answer is not what it looks like from the outside.
declare -a TIMINGS=()
timed() {
    local label="$1"; shift
    local start=$SECONDS
    "$@"
    TIMINGS+=("$(printf '%4d s  %s' "$((SECONDS - start))" "$label")")
}

# Whether `bin` has to be built again: missing, or older than any source
# it could have been built from. Cheap enough to ask every run, and far
# cheaper than the compile it usually avoids.
needs_build() {
    local bin="$1"
    [ -x "$bin" ] || return 0
    # The parentheses matter: without them the -newer binds only to the
    # second -name, so every .c matched unconditionally and printed
    # nothing, and the answer was always "no rebuild needed". A cache
    # that never invalidates is worse than no cache.
    if [ -n "$(find "$PROJECT_DIR" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) \
                    -newer "$bin" -print -quit 2>/dev/null)" ]; then
        return 0
    fi
    [ -n "$(find "$SCRIPT_DIR/c" -newer "$bin" -print -quit 2>/dev/null)" ]
}

run_c_test() {
    local name="$1"
    shift
    local pkgs="$1"
    shift

    echo "═══ C: $name ═══"
    if needs_build "$BUILD_DIR/$name"; then
        if ! $LOW gcc -O1 -Wall -Wextra -o "$BUILD_DIR/$name" "$SCRIPT_DIR/c/$name.c" \
            $(pkg-config --cflags --libs $pkgs) 2>&1; then
            echo "  COMPILE FAILED"
            failed=1
            return
        fi
    fi
    if ! $LOW "$BUILD_DIR/$name"; then
        failed=1
    fi
}

# The C tests #include the .c file under test, so they see its static
# functions; the pkg-config sets below are just that file's own deps.
timed "C: gamepad bridge"  run_c_test test_gamepad_bridge "libusb-1.0 sdl2"
timed "C: web stream auth"  run_c_test test_web_stream_auth "sdl2 gstreamer-1.0"
timed "C: change watch"     run_c_test test_change_watch "libjpeg sdl2 libswscale libavcodec libavutil"
timed "C: rtp mtu"          run_c_test test_rtp_mtu "sdl2 gstreamer-1.0 gstreamer-video-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 libavcodec libavutil libswscale"

# Before the security suite, and that order is not cosmetic: the security
# suite deliberately trips the failed-login lockout, which lasts thirty
# seconds. This suite needs a session token, so run after it, it does the
# only correct thing and waits the lockout out -- turning a 35-second
# suite into a 65-second one for no benefit at all.
echo
echo "═══ Native transport (needs the app running) ═══"
if curl -fsS -m 2 "http://127.0.0.1:${WEB_PORT:-5080}/clients" >/dev/null 2>&1; then
    timed "native transport" $LOW python3 "$SCRIPT_DIR/native/test_switch_stream.py" || failed=1
else
    echo "  skipped: nothing listening on 127.0.0.1:${WEB_PORT:-5080}"
fi
echo
echo "═══ Security (needs the app running) ═══"
if curl -fsS -m 2 "http://127.0.0.1:${WEB_PORT:-5080}/clients" >/dev/null 2>&1; then
    timed "security" $LOW node "$SCRIPT_DIR/security/attack.js" || failed=1
else
    echo "  skipped: nothing listening on 127.0.0.1:${WEB_PORT:-5080}"
fi


echo
echo "═══ JavaScript ═══"
if ! timed "javascript" $LOW node "$SCRIPT_DIR/run_tests.js"; then
    failed=1
fi

echo
echo "═══ where the time went ═══"
printf '%s\n' "${TIMINGS[@]}"

echo
if [ "$failed" -eq 0 ]; then
    echo "✅ all suites passed"
else
    echo "❌ some suites failed"
fi
exit "$failed"
