#!/bin/bash
# Build, send to the console, and watch its logs -- without touching the
# SD card.
#
# Two things make this possible, and both are already on the console:
# the wiiload plugin receives an .rpx over the network and launches it
# immediately, and WHBLogUdp sends printf output back here, which
# udplogserver prints.
#
#   ./tools/dev.sh            build and send the client
#   ./tools/dev.sh bringup    build and send the bring-up probe
#   ./tools/dev.sh logs       only watch the logs
#
# The console is found by looking for its FTP banner, so it does not
# matter that DHCP keeps moving it. Set WIIU_IP to skip the search.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WHAT="${1:-client}"

find_console() {
    seq 2 254 | sed 's|^|192.168.2.|' | xargs -P 64 -I{} sh -c '
        timeout 1 python3 -c "
import socket,sys
try:
    s=socket.create_connection((\"{}\",21),0.8); d=s.recv(64); s.close()
except Exception: sys.exit(1)
sys.exit(0 if b\"Hello\" in d else 1)" 2>/dev/null && echo {}' | head -1
}

CONSOLE="${WIIU_IP:-$(find_console)}"
if [ -z "$CONSOLE" ]; then
    echo "no console found (set WIIU_IP to skip the search)" >&2
    exit 1
fi
echo "console: $CONSOLE"

TARGET=""
case "$WHAT" in
    logs)    ;;
    bringup) make -C "$HERE/../bringup" || exit 1
             TARGET="$HERE/../bringup/bringup.rpx" ;;
    *)       make -C "$HERE/.." || exit 1
             TARGET="$HERE/../capture2wiiu.rpx" ;;
esac

# The log server first, so nothing said at startup is missed.
udplogserver &
LOGGER=$!
trap 'kill $LOGGER 2>/dev/null' EXIT
sleep 1

if [ -n "$TARGET" ]; then
    echo "sending $(basename "$TARGET")"
    WIILOAD="tcp:$CONSOLE" wiiload "$TARGET" || exit 1
fi

echo "--- logs (ctrl-c to stop) ---"
wait $LOGGER
