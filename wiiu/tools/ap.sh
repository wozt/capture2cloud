#!/bin/bash
# ap.sh start|stop -- the access point a Wii U GamePad associates to.
#
# Separate from the host, and from ap-pair.sh beside it: pairing is a
# one-time ceremony with a PIN, while this is the everyday "is the radio
# up". Both need the forked hostapd, which cannot be vendored -- it is a
# whole hostapd tree. Point DRC_HOSTAP at your build of it.
#
# hostapd binds a network interface, so it needs root. This asks through
# sudo, which is what a terminal would do; a machine with no password
# prompt available will simply report that.
set -u
: "${DRC_IF:=wlxe0ad474070d8}"
: "${DRC_AP_MAC:=34:af:2c:be:ef:01}"
: "${DRC_HOSTAP:=/home/wozt/rtw88_TSF/drc-hostap}"
: "${DRC_RUN:=$HOME/.cache/drc-lab}"
: "${DRC_MTU:=1800}"
HH=$DRC_HOSTAP/hostapd/hostapd
CONF=$DRC_HOSTAP/conf/local_normal2.conf
mkdir -p "$DRC_RUN"

case "${1:-}" in
start)
    if pgrep -f "hostapd .*$(basename "$CONF")" >/dev/null 2>&1; then
        echo "ap: already up"
        exit 0
    fi
    [ -x "$HH" ] || { echo "ap: no hostapd at $HH"; exit 1; }
    sudo ip link set "$DRC_IF" down 2>/dev/null
    sudo ip link set "$DRC_IF" address "$DRC_AP_MAC" 2>/dev/null
    sudo ip link set "$DRC_IF" up 2>/dev/null
    sudo "$HH" -B -dd "$CONF" > "$DRC_RUN/ap.log" 2>&1 || {
        echo "ap: hostapd would not start; see $DRC_RUN/ap.log"; exit 1; }
    # The address libdrc hardcodes, and the MTU audio needs: 1572-byte
    # packets fragment at 1500, and fragmentation is what was left of
    # the freeze.
    sudo ip addr flush dev "$DRC_IF"
    sudo ip addr add 192.168.1.10/24 dev "$DRC_IF"
    sudo ip link set mtu "$DRC_MTU" dev "$DRC_IF"
    echo "ap: up on $DRC_IF"
    ;;
stop)
    sudo pkill -TERM -f "hostapd .*$(basename "$CONF")" 2>/dev/null
    echo "ap: stopped"
    ;;
deauth)
    # Asks whatever pad is associated to come back. No root: the control
    # socket belongs to the sudo group, and this is the gesture that
    # restarts a session without touching the access point.
    HC=$DRC_HOSTAP/hostapd/hostapd_cli
    [ -x "$HC" ] || HC=hostapd_cli
    IFACE=$(ls /var/run/hostapd 2>/dev/null | head -1)
    [ -n "$IFACE" ] || { echo "deauth: no access point is running"; exit 1; }
    MAC=$("$HC" -p /var/run/hostapd -i "$IFACE" list_sta 2>/dev/null | head -1)
    [ -n "$MAC" ] || { echo "deauth: no pad is associated"; exit 1; }
    "$HC" -p /var/run/hostapd -i "$IFACE" deauthenticate "$MAC" >/dev/null
    echo "deauth: asked $MAC to come back"
    ;;
*)
    echo "usage: ap.sh start|stop|deauth"
    exit 2
    ;;
esac
