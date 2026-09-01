#!/bin/bash

set -u

# Low-latency HDMI/USB capture, designed for Discord window sharing.
# Launches a native V4L2 + SDL2 + PulseAudio player: audio and the window
# belong to the same process, with less buffering than an ffmpeg|ffplay pipe.

WINDOW_TITLE="Capture2Cloud"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The build recipe and device checks are shared with the headless
# launcher, so there is only ever one of each to keep up to date.
#
# The library is found rather than assumed, because this launcher is
# useful from more than one place: beside the project, at its root, or in
# its scripts directory. It used to hardcode the first of those, so a
# copy anywhere else failed on its first line with "N'est pas un
# dossier". Everything else -- the source list, the binary, the .env --
# is derived from where the library itself turns out to be.
C2C_LIB=""
for candidate in \
    "$SCRIPT_DIR/lib_toggle.sh" \
    "$SCRIPT_DIR/scripts/lib_toggle.sh" \
    "$SCRIPT_DIR/capture2cloud/scripts/lib_toggle.sh"; do
    if [ -f "$candidate" ]; then
        C2C_LIB="$candidate"
        break
    fi
done
if [ -z "$C2C_LIB" ]; then
    echo "Error: cannot find lib_toggle.sh from $SCRIPT_DIR" >&2
    exit 1
fi
# shellcheck source=capture2cloud/scripts/lib_toggle.sh
source "$C2C_LIB"

# In /dev/shm, which is RAM on every Linux system. /tmp is only
# sometimes -- it happens to be tmpfs here, but a log that is written
# for as long as this runs must not depend on that being true.
PID_FILE="/dev/shm/capture2cloud.pid"
LOG_FILE="/dev/shm/capture2cloud.log"

# Useful when the script is launched from SSH.
export DISPLAY="${DISPLAY:-:0}"

if c2c_find_running windowed >/dev/null; then
    echo "Stopping Capture2Cloud..."
    c2c_stop_mode windowed "$PID_FILE"
    echo "Capture2Cloud OFF"
    exit 0
fi
rm -f "$PID_FILE"

c2c_refuse_if_other_mode_running headless "headless" || exit 1

c2c_load_env

c2c_check_devices || exit 1
c2c_build_if_needed || exit 1

setsid "$C2C_BIN" "$CAPTURE_VIDEO" >"$LOG_FILE" 2>&1 &

PID=$!
echo "$PID" > "$PID_FILE"
disown "$PID"

echo "Capture2Cloud ON."
echo "Window: $WINDOW_TITLE"
echo "Log: $LOG_FILE"
echo "Run this script again to stop."
