#!/bin/bash

set -u

# Capture2Cloud without a local window: no SDL display, no GTK control
# bar. Capture, encode and serve, nothing drawn. For running over SSH,
# from a systemd unit, or on a machine with no desktop session -- the web
# page is the only interface, so the stream starts on its own.
#
# Toggles: run it again to stop. Its own pid/log files, separate from the
# windowed launcher's, so the two never trample each other's state.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
PID_FILE="/dev/shm/capture2cloud_headless.pid"
LOG_FILE="/dev/shm/capture2cloud_headless.log"

if c2c_find_running headless >/dev/null; then
    echo "Stopping Capture2Cloud (headless)..."
    c2c_stop_mode headless "$PID_FILE"
    echo "Capture2Cloud OFF"
    exit 0
fi
rm -f "$PID_FILE"

c2c_refuse_if_other_mode_running windowed "windowed" || exit 1

c2c_load_env
c2c_check_devices || exit 1
c2c_build_if_needed || exit 1

# DISPLAY is passed through when there is one, and simply absent when
# there is not.
#
# It used to be dropped on purpose -- the point of this mode being that
# it needs no display -- which is true of the CAPTURE and false of the
# tray icon. Headless means no video window, not no desk: someone running
# it this way still wants somewhere to change the bitrate and something
# to quit with. Over ssh or from a unit file there is no DISPLAY to pass,
# the program says so once and carries on without a tray.
setsid "$C2C_BIN" --headless "$CAPTURE_VIDEO" >"$LOG_FILE" 2>&1 &

PID=$!
echo "$PID" > "$PID_FILE"
disown "$PID"

# Give it a moment to bind the port (or fail), so the message below
# reflects what actually happened rather than what was attempted.
sleep 2
if ! kill -0 "$PID" 2>/dev/null; then
    echo "Capture2Cloud (headless) failed to start. Last lines of $LOG_FILE:"
    tail -n 15 "$LOG_FILE"
    rm -f "$PID_FILE"
    exit 1
fi

PORT="$(grep -oP 'listening on port \K[0-9]+' "$LOG_FILE" | tail -1)"
echo "Capture2Cloud ON (headless, no window)."
if [ -n "$PORT" ]; then
    echo "Stream: http://$(hostname -I 2>/dev/null | awk '{print $1}'):$PORT/"
else
    echo "Warning: the web stream did not report a port -- check $LOG_FILE"
fi
echo "Log: $LOG_FILE"
echo "Run this script again to stop."
