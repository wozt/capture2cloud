#!/bin/bash
# ap-pair.sh <8-digit PIN> -- brings the pairing AP up, arms WPS, then
# switches to the normal AP the moment M8 goes out.
#
# The window is narrow: the pad waits about 4 s after M8 before it looks
# for the normal AP, and a kill -9 leaves the interface in a transient
# state for around 5 s. Hence SIGTERM, and waiting for the process to
# actually exit rather than assuming it has.
#
# This needs the forked hostapd, which cannot be vendored here the way
# libdrc and drc-x264 are -- it is a whole hostapd tree. Point DRC_HOSTAP
# at your build of it. The conf/ directory beside this script holds the
# two templates those configs are made from, with the placeholders and
# the reasons; see ../docs/WIIU_GAMEPAD.md for the pairing account.
set +e
PIN=${1:-00005678}
: "${DRC_IF:=wlxe0ad474070d8}"
: "${DRC_AP_MAC:=34:af:2c:be:ef:01}"
: "${DRC_HOSTAP:=/home/wozt/rtw88_TSF/drc-hostap}"
: "${DRC_RUN:=$HOME/.cache/drc-lab}"
: "${DRC_UUID:=22210203-0405-0607-0809-1a1b1c1d1e1f}"
mkdir -p "$DRC_RUN"
HH=$DRC_HOSTAP/hostapd/hostapd
HC=$DRC_HOSTAP/hostapd/hostapd_cli
CONF=$DRC_HOSTAP/conf

ip link set "$DRC_IF" down; sleep 1
iw dev "$DRC_IF" set type managed 2>/dev/null
ip link set "$DRC_IF" address "$DRC_AP_MAC"
ip link set "$DRC_IF" up; sleep 2

echo "[pair] pairing AP (_STA1), PIN $PIN"
$HH -dd "$CONF/local_pair2.conf" > "$DRC_RUN/pair.log" 2>&1 &
HPID=$!
sleep 5
$HC -p /var/run/hostapd -i "$DRC_IF" wps_pin "$DRC_UUID" "$PIN" 300 >/dev/null
echo "[pair] PIN armed -- start the sync on the pad"

for i in $(seq 1 2000); do
  grep -qa "Building Message M8\|WPS-SUCCESS" "$DRC_RUN/pair.log" && break
  sleep 0.2
done
echo "[pair] M8 seen, switching to the normal AP"
kill -TERM $HPID 2>/dev/null
for i in $(seq 1 30); do kill -0 $HPID 2>/dev/null || break; sleep 0.1; done

ip link set "$DRC_IF" up 2>/dev/null
$HH -dd "$CONF/local_normal2.conf" > "$DRC_RUN/ap.log" 2>&1 &
for i in $(seq 1 100); do grep -qa "AP-ENABLED" "$DRC_RUN/ap.log" && break; sleep 0.05; done
ip addr flush dev "$DRC_IF"
ip addr add 192.168.1.10/24 dev "$DRC_IF"
ip link set mtu "${DRC_MTU:-1800}" dev "$DRC_IF"
[ -n "$DRC_DNSMASQ_CONF" ] && /usr/sbin/dnsmasq -d -C "$DRC_DNSMASQ_CONF" > "$DRC_RUN/dns.log" 2>&1 &
echo "[pair] normal AP up, waiting for DHCP..."
for i in $(seq 1 90); do
  grep -qa DHCPACK "$DRC_RUN/dns.log" 2>/dev/null && { echo "[pair] pad connected"; exit 0; }
  sleep 1
done
echo "[pair] no DHCP -- the pad did not come back"
