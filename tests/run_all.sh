#!/bin/bash
# Runs the whole test suite: the C unit tests (tests/c/) and the
# JavaScript suite (tests/run_tests.js). Exits non-zero if anything
# fails, so it can gate a deploy.
#
# Usage: ./tests/run_all.sh   (from capture2cloud/, or anywhere)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# C2C_0835_SMART_TEST_SELECTION
# The smart selector calls this script with --only, never --changed.
if [ "${1:-}" = "--changed" ]; then
    shift
    exec python3 "$PROJECT_DIR/tools/smart_tests.py" "$@"
fi

VALID_SUITES=" test_gamepad_bridge test_controller_shaping test_wup_touch test_output_pcble test_pcble_protocol test_jc2_stick_routing test_reset_method test_web_stream_auth test_change_watch test_ws_frame test_rtp_mtu test_h264_encoders test_gtk_layout test_drc_encoder wozt-assets wozt-tooltips javascript native security smart "
if [ "${1:-}" = "--list" ]; then
    printf 'Available suites: %s\n' "$VALID_SUITES"
    printf 'Aliases: gtk (test_gtk_layout), js (javascript)\n'
    exit 0
fi

ONLY_SUITE=""
if [ "${1:-}" = "--only" ]; then
    shift
    if [ "$#" -ne 1 ] || [ -z "$1" ]; then
        echo 'Usage: ./tests/run_all.sh --only test_gtk_layout[,javascript,...]' >&2
        exit 2
    fi
    IFS=',' read -r -a selected <<< "$1"
    shift
    for choice in "${selected[@]}"; do
        case "$choice" in
            gtk) choice=test_gtk_layout ;;
            js) choice=javascript ;;
        esac
        if [ -z "$choice" ] || [[ "$VALID_SUITES" != *" $choice "* ]]; then
            printf 'Unknown suite: %s (see --list)\n' "$choice" >&2
            exit 2
        fi
        ONLY_SUITE="${ONLY_SUITE:+$ONLY_SUITE,}$choice"
    done
fi
if [ "$#" -ne 0 ]; then
    echo 'Usage: ./tests/run_all.sh [--changed [--dry-run|--base REV]] [--only SUITES] [--list]' >&2
    exit 2
fi

suite_selected() {
    [ -z "$ONLY_SUITE" ] && return 0
    case ",$ONLY_SUITE," in
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

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

# Only run these validators when selected (default full mode selects all).
if suite_selected wozt-tooltips; then
    if ! python3 "$PROJECT_DIR/tools/check-wozt-tooltips.py"; then failed=1; fi
fi
if suite_selected wozt-assets; then
    if ! python3 "$PROJECT_DIR/tools/check-wozt-assets.py"; then failed=1; fi
fi

# How long each suite took, printed at the end. Worth having: "the tests
# take too long" is not actionable until you know which one is taking the
# time, and the answer is not what it looks like from the outside.
declare -a TIMINGS=()
timed() {
    local label="$1"; shift
    local start=$SECONDS
    local exit_code=0
    "$@" || exit_code=$?
    TIMINGS+=("$(printf '%4d s  %s' "$((SECONDS - start))" "$label")")
    return "$exit_code"
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
    if [ -n "$(find "$PROJECT_DIR/server" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) \
                    -newer "$bin" -print -quit 2>/dev/null)" ]; then
        return 0
    fi
    if [ -n "$(find "$PROJECT_DIR/shared" -maxdepth 1 -name '*.h' \
                    -newer "$bin" -print -quit 2>/dev/null)" ]; then
        return 0
    fi
    [ -n "$(find "$SCRIPT_DIR/c" -newer "$bin" -print -quit 2>/dev/null)" ]
}

# Same as run_c_test, with a virtual display for a test that opens a
# window. Xvfb, not the real X server: a test must not steal focus or
# pop windows over whatever somebody is doing.
run_c_test_x11() {
    local name="$1"
    shift
    local pkgs="$1"
    shift
    local extra="${1:-}"

    echo "═══ C: $name ═══"
    if needs_build "$BUILD_DIR/$name"; then
        if ! $LOW gcc -O1 -Wall -Wextra -I"$PROJECT_DIR" -I"$PROJECT_DIR/shared" -I"$PROJECT_DIR/server" \
            -o "$BUILD_DIR/$name" "$SCRIPT_DIR/c/$name.c" \
            $(if [ -n "$pkgs" ]; then pkg-config --cflags --libs $pkgs; fi) $extra 2>&1; then
            echo "  COMPILE FAILED"
            failed=1
            return
        fi
    fi
    local gtk_exit=0
    $LOW env C2C_RESOURCE_ROOT="$PROJECT_DIR" NO_AT_BRIDGE=1 xvfb-run -a "$BUILD_DIR/$name" || gtk_exit=$?
    if [ "$gtk_exit" -ne 0 ]; then
        failed=1
        # A failed assertion exits with code 1 and already prints its details.
        # Rerun under GDB only for signal exits (e.g. SIGSEGV=139, SIGABRT=134).
        if [ "$gtk_exit" -ge 128 ] && command -v gdb >/dev/null 2>&1; then
            echo "  GTK crashed (exit $gtk_exit); collecting optional GDB backtrace"
            $LOW env C2C_RESOURCE_ROOT="$PROJECT_DIR" NO_AT_BRIDGE=1 xvfb-run -a gdb -q -batch -ex run -ex "thread apply all bt 12" --args "$BUILD_DIR/$name" || true
        fi
    fi
}

run_c_test() {
    local name="$1"
    shift
    local pkgs="$1"
    shift
    # Anything pkg-config cannot describe: a vendored header's include
    # path, a library opened rather than linked. Optional.
    local extra="${1:-}"

    echo "═══ C: $name ═══"
    if needs_build "$BUILD_DIR/$name"; then
        if ! $LOW gcc -O1 -Wall -Wextra -I"$PROJECT_DIR" -I"$PROJECT_DIR/shared" -I"$PROJECT_DIR/server" \
            -o "$BUILD_DIR/$name" "$SCRIPT_DIR/c/$name.c" \
            $(if [ -n "$pkgs" ]; then pkg-config --cflags --libs $pkgs; fi) $extra 2>&1; then
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
if suite_selected test_gamepad_bridge; then
    timed "C: gamepad bridge"  run_c_test test_gamepad_bridge "libusb-1.0 sdl2"
fi
if suite_selected test_controller_shaping; then
    timed "C: controller shaping" run_c_test test_controller_shaping "" "-lm"
fi
if suite_selected test_wup_touch; then
    timed "C: WUP touch"          run_c_test test_wup_touch ""
fi
if suite_selected test_output_pcble; then
    timed "C: pcble output"     run_c_test test_output_pcble "sdl2 gio-2.0"
fi
if suite_selected test_pcble_protocol; then
    timed "C: pcble protocol"   run_c_test test_pcble_protocol ""
fi
# Both sticks must remain independent in Joy-Con 2 pair/mixed upright mode.
# This is a pure mapping test; no Bluetooth adapter or paired Switch needed.
if suite_selected test_jc2_stick_routing; then
    timed "C: Joy-Con 2 stick routing" run_c_test test_jc2_stick_routing "gio-2.0"
fi
if suite_selected test_reset_method; then
    timed "C: reset method"     run_c_test test_reset_method "sdl2 gio-2.0"
fi
# glib because server/web_stream.c now answers a WebSocket upgrade, whose
# handshake is a SHA-1 and a base64 -- both GLib's.
if suite_selected test_web_stream_auth; then
    timed "C: web stream auth"  run_c_test test_web_stream_auth "sdl2 gstreamer-1.0 glib-2.0"
fi
if suite_selected test_change_watch; then
    timed "C: change watch"     run_c_test test_change_watch "libjpeg sdl2 libswscale libavcodec libavutil"
fi
if suite_selected test_ws_frame; then
    timed "C: websocket frame" run_c_test test_ws_frame "glib-2.0"
fi
if suite_selected test_rtp_mtu; then
    timed "C: rtp mtu"          run_c_test test_rtp_mtu "sdl2 gstreamer-1.0 gstreamer-video-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 libavcodec libavutil libswscale"
fi
if suite_selected test_h264_encoders; then
    timed "C: h264 encoders"   run_c_test test_h264_encoders "sdl2 gstreamer-1.0 gstreamer-video-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 libavcodec libavutil libswscale"
fi
# The settings window, opened for real on a virtual display: GTK reports
# a broken layout at run time and never at compile time. Skipped rather
# than failed where Xvfb is not installed.
if suite_selected test_gtk_layout; then
    if command -v xvfb-run >/dev/null 2>&1; then
        timed "C: gtk layout" run_c_test_x11 test_gtk_layout "sdl2 gtk+-3.0 x11 glib-2.0 json-glib-1.0" "-lm"
    else
        echo "═══ C: gtk layout ═══"; echo "  xvfb-run not installed; skipped"
        # A targeted GTK-only invocation cannot claim success without GTK.
        if [ -n "$ONLY_SUITE" ]; then failed=1; fi
    fi
fi

# The fourth encode. Linked against libavcodec on purpose: that is what
# brings the distribution's x264 into the process, and the whole point of
# this test is that drc-x264 lives beside it without either answering for
# the other. Skips itself where drc-x264 is not installed.
if suite_selected test_drc_encoder; then
    timed "C: wii u encode"     run_c_test test_drc_encoder "libavcodec libavutil" "-I$PROJECT_DIR/wiiu_gamepad/include -ldl"
fi

if suite_selected smart; then
    echo "═══ Python: smart test selector ═══"
    if ! timed "python: smart test selector" $LOW python3 "$PROJECT_DIR/tests/test_smart_tests.py"; then
        failed=1
    fi
fi

# Before the security suite, and that order is not cosmetic: the security
# suite deliberately trips the failed-login lockout, which lasts thirty
# seconds. This suite needs a session token, so run after it, it does the
# only correct thing and waits the lockout out -- turning a 35-second
# suite into a 65-second one for no benefit at all.
echo
if suite_selected native; then
    echo "═══ Native transport (needs the app running) ═══"
    if curl -fsS -m 2 "http://127.0.0.1:${WEB_PORT:-5080}/clients" >/dev/null 2>&1; then
        timed "native transport" $LOW python3 "$SCRIPT_DIR/native/test_switch_stream.py" || failed=1
    else
        echo "  skipped: nothing listening on 127.0.0.1:${WEB_PORT:-5080}"
    fi
fi
echo
if suite_selected security; then
    echo "═══ Security (needs the app running) ═══"
    if curl -fsS -m 2 "http://127.0.0.1:${WEB_PORT:-5080}/clients" >/dev/null 2>&1; then
        timed "security" $LOW node "$SCRIPT_DIR/security/attack.js" || failed=1
    else
        echo "  skipped: nothing listening on 127.0.0.1:${WEB_PORT:-5080}"
    fi
fi


echo
if suite_selected javascript; then
    echo "═══ JavaScript ═══"
    if ! timed "javascript" $LOW node "$SCRIPT_DIR/run_tests.js"; then
        failed=1
    fi
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
