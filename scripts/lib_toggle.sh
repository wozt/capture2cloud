# Shared by toggle_capture2cloud.sh (windowed) and
# toggle_capture2cloud_headless.sh. Sourced, never executed.
#
# It exists so there is ONE build recipe: the source list and the
# pkg-config deps used to live in the launcher, and a second copy in the
# headless launcher would have gone stale the first time a .c file was
# added.

# Everything is derived from this file's own location, so the project can
# be moved or renamed without editing paths.
C2C_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

C2C_SOURCES=(
    "$C2C_DIR/capture2cloud.c"
    "$C2C_DIR/gtk_shell.c"
    "$C2C_DIR/web_stream.c"
    "$C2C_DIR/gst_webrtc.c"
    "$C2C_DIR/gamepad_bridge.c"
    "$C2C_DIR/app_config.c"
    "$C2C_DIR/video_capture.c"
    "$C2C_DIR/audio_capture.c"
    "$C2C_DIR/switch_stream.c"
    "$C2C_DIR/local_pad.c"
)
C2C_HEADERS=(
    "$C2C_DIR/gtk_shell.h"
    "$C2C_DIR/web_stream.h"
    "$C2C_DIR/gst_webrtc.h"
    "$C2C_DIR/gamepad_bridge.h"
    "$C2C_DIR/app_config.h"
    "$C2C_DIR/video_capture.h"
    "$C2C_DIR/audio_capture.h"
    "$C2C_DIR/switch_stream.h"
    "$C2C_DIR/c2s_protocol.h"
    "$C2C_DIR/app_settings.h"
    "$C2C_DIR/local_pad.h"
)
C2C_BIN="$C2C_DIR/capture2cloud"
C2C_PKGCONFIG_DEPS="sdl2 libpulse libpulse-simple libjpeg gtk+-3.0 x11 gstreamer-1.0 gstreamer-app-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-video-1.0 libswscale libusb-1.0"

# The launchers read the same .env the app itself does, so a different
# capture card is configured in one place.
c2c_load_env() {
    local env_file="$C2C_DIR/scripts/.env"
    if [ -f "$env_file" ]; then
        # shellcheck disable=SC1090
        source "$env_file"
    fi
    # Stable udev link by default (survives /dev/videoN renumbering on
    # reboot/replug).
    CAPTURE_VIDEO="${VIDEO_DEVICE:-/dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0}"
    CAPTURE_AUDIO_SOURCE="${AUDIO_SOURCE:-alsa_input.usb-MACROSILICON_USB3_Video_20210623-02.pro-input-0}"
}

c2c_check_devices() {
    if [ ! -e "$CAPTURE_VIDEO" ]; then
        echo "Error: video device not found: $CAPTURE_VIDEO"
        echo "Available devices:"
        v4l2-ctl --list-devices 2>/dev/null || ls -la /dev/video* 2>/dev/null
        return 1
    fi

    if ! pactl list short sources | awk '{print $2}' | grep -Fxq "$CAPTURE_AUDIO_SOURCE"; then
        echo "Error: audio source not found: $CAPTURE_AUDIO_SOURCE"
        echo "Available sources:"
        pactl list short sources
        return 1
    fi
}

# Rebuilds only when a source or header is newer than the binary.
c2c_build_if_needed() {
    local need=0
    if [ ! -x "$C2C_BIN" ]; then
        need=1
    else
        local f
        for f in "${C2C_SOURCES[@]}" "${C2C_HEADERS[@]}"; do
            if [ "$f" -nt "$C2C_BIN" ]; then
                need=1
                break
            fi
        done
    fi
    [ "$need" -eq 0 ] && return 0

    if ! command -v gcc >/dev/null 2>&1 || ! command -v pkg-config >/dev/null 2>&1; then
        echo "Error: gcc/pkg-config required to compile ${C2C_SOURCES[0]}"
        return 1
    fi
    if ! pkg-config --exists $C2C_PKGCONFIG_DEPS; then
        echo "Error: missing dependencies. Run: $C2C_DIR/install_deps.sh"
        return 1
    fi

    gcc -O2 -Wall -Wextra -o "$C2C_BIN" "${C2C_SOURCES[@]}" \
        $(pkg-config --cflags --libs $C2C_PKGCONFIG_DEPS) -lm
}

# --- finding a running instance -------------------------------------
#
# The pid file is a convenience, not the source of truth. It can be
# deleted, lost across a reboot, or left behind pointing at a pid the
# kernel has since handed to something else -- while the real process
# carries on holding the capture card and the web port. Every check below
# therefore confirms against /proc, and can find an instance with no pid
# file at all.

# True if $1 is a live process that is actually this project's binary.
c2c_pid_is_ours() {
    local pid="$1"
    [ -n "$pid" ] || return 1
    [ -r "/proc/$pid/cmdline" ] || return 1
    tr '\0' ' ' < "/proc/$pid/cmdline" | grep -Fq "$C2C_BIN"
}

# Prints the pid of a running instance of the given mode, if any.
# Modes: headless | windowed | any.
c2c_find_running() {
    local want="$1" pid args
    # Matched on the command line, not on the process name. The name is
    # not ours to rely on: a process that has re-executed itself can come
    # back under a different one, and this reported nothing running while
    # the capture card was still held.
    for pid in $(pgrep -f "$C2C_BIN" 2>/dev/null); do
        [ "$pid" = "$$" ] && continue
        c2c_pid_is_ours "$pid" || continue
        args="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)"
        case "$want" in
            headless)
                case "$args" in *--headless*) echo "$pid"; return 0 ;; esac ;;
            windowed)
                case "$args" in *--headless*) ;; *) echo "$pid"; return 0 ;; esac ;;
            any)
                echo "$pid"; return 0 ;;
        esac
    done
    return 1
}

# Stops the instance of the given mode, whether or not the pid file still
# names it -- so an orphan left by a deleted pid file is still stopped by
# running the launcher again, instead of lingering invisibly and making
# the next start fail with "device busy".
c2c_stop_mode() {
    local mode="$1" pid_file="$2" pid=""

    if [ -f "$pid_file" ]; then
        pid="$(cat "$pid_file" 2>/dev/null)"
        c2c_pid_is_ours "$pid" || pid=""
    fi
    [ -n "$pid" ] || pid="$(c2c_find_running "$mode")"
    rm -f "$pid_file"
    [ -n "$pid" ] || return 0

    # Negative pid targets the process group: the launcher starts it with
    # setsid, so the binary is its own group leader.
    kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null

    # A clean shutdown was measured at ~1.0 s (handing the adapter back,
    # then stopping the pipeline, sockets and audio threads). The old 2 s
    # limit meant SIGKILL landed right as the adapter was being released,
    # leaving it in capture mode across a relaunch -- which showed up as
    # input latency until it was physically unplugged. Give it room.
    for _ in $(seq 1 50); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done

    kill -9 -- "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true
}

# The capture card and the web port can only belong to one instance, so
# refuse to start the other mode rather than fail halfway with a
# confusing "device busy".
c2c_refuse_if_other_mode_running() {
    local mode="$1" name="$2" pid
    pid="$(c2c_find_running "$mode")" || return 0
    echo "Error: the $name instance is already running (pid $pid)."
    echo "It holds the capture device and the web port. Stop it first."
    return 1
}
