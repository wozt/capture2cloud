#!/usr/bin/env python3
"""Speaks the native protocol at a running host, the way the Switch
homebrew does.

Exists because the homebrew cannot be run from here: the console is the
only thing that can execute it, so every protocol decision would
otherwise be unverified until it was on hardware. This exercises the
handshake, the access check and the input path against the real server.

Never sends a HOME press with a valid token: that presses HOME on the
console for real. Its refusal path is what matters and needs no token.
"""
import socket, struct, sys, time, urllib.request

HOST = "127.0.0.1"
PORT = 5081
WEB = "http://127.0.0.1:5080"

MAGIC = 0x57533243
VERSION = 1
PAD_SLOTS = 21
MSG_VIDEO, MSG_AUDIO, MSG_INPUT, MSG_PING, MSG_HOME = 1, 2, 16, 17, 18

passed = failed = 0
def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1; print(f"  ok   {name}")
    else:
        failed += 1; print(f"  FAIL {name}" + (f"  <- {detail}" if detail else ""))
def group(n): print(f"\n{n}")

def login():
    """Waits out the failed-login lockout before asking.

    The security suite deliberately triggers that lockout and it lasts
    30 s -- longer than a run -- so this test running after it would get
    a 429, capture the empty body as its token, and then report the
    access checks as broken when they are working perfectly."""
    for _ in range(40):
        try:
            r = urllib.request.urlopen(urllib.request.Request(
                WEB + "/login", data=b"changeme", method="POST"), timeout=5)
            tok = r.read().decode().strip()
            if len(tok) == 64:
                return tok
        except urllib.error.HTTPError as e:
            if e.code != 429:
                return ""
        except Exception:
            return ""
        time.sleep(1)
    return ""

def connect(token=""):
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    tb = token.encode()
    s.sendall(struct.pack("<IBBH", MAGIC, VERSION, len(tb), 0) + tb)
    return s

def read_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError("host closed")
        buf += chunk
    return buf

# Mirrors C2sHelloAck exactly, including its trailing padding: the C
# side pins these sizes with _Static_assert, so a mismatch here means the
# test is wrong, not the protocol.
ACK_FORMAT = "<IBBBBHHBBHB3x"
ACK_SIZE = struct.calcsize(ACK_FORMAT)

def read_ack(s):
    raw = read_exact(s, ACK_SIZE)
    (magic, ver, accepted, may, _r, w, h, vc, ac, rate, ch) = struct.unpack(ACK_FORMAT, raw)
    return dict(magic=magic, version=ver, accepted=accepted, may_control=may,
                width=w, height=h, video_codec=vc, audio_codec=ac,
                audio_rate=rate, audio_channels=ch)

def frame(t, payload=b"", flags=0):
    return struct.pack("<BBHI", t, flags, 0, len(payload)) + payload

print(f"cible: {HOST}:{PORT}")

group("handshake")
tok = login()
check("obtained a session token from the web server", len(tok) == 64, f"{len(tok)} chars")

s = connect(tok)
ack = read_ack(s)
check("magic echoed back", ack["magic"] == MAGIC, hex(ack["magic"]))
check("version matches", ack["version"] == VERSION)
check("accepted", ack["accepted"] == 1)
check("a valid token grants control", ack["may_control"] == 1)
check("announces a video size", ack["width"] > 0 and ack["height"] > 0,
      f'{ack["width"]}x{ack["height"]}')
# The handshake reports what the host is encoding RIGHT NOW, not a
# constant: it answered VP8 unconditionally, so a client arriving while
# the stream was H.264 opened a VP8 decoder and saw nothing. Either
# answer is correct here; what must not happen is a third value.
check("announces the codec it is actually sending",
      ack["video_codec"] in (1, 3) and ack["audio_codec"] == 2,
      f'video={ack["video_codec"]} audio={ack["audio_codec"]}')
check("announces 48 kHz stereo", ack["audio_rate"] == 48000 and ack["audio_channels"] == 2,
      f'{ack["audio_rate"]}Hz x{ack["audio_channels"]}')

group("access control")
v = connect("")            # no token at all
ackv = read_ack(v)
check("no token -> accepted but as a viewer", ackv["accepted"] == 1 and ackv["may_control"] == 0)
b = connect("deadbeef" * 8)  # 64 chars, wrong
ackb = read_ack(b)
check("bogus token -> viewer", ackb["may_control"] == 0)

# A viewer's input must be dropped server-side; the only observable is
# that the server keeps the connection and does not act on it.
v.sendall(frame(MSG_INPUT, bytes([0] * PAD_SLOTS)))
v.sendall(frame(MSG_HOME))
time.sleep(0.3)
v.sendall(frame(MSG_PING))
check("a viewer sending input and HOME is not disconnected", True)

group("protocol errors are refused")
bad = socket.create_connection((HOST, PORT), timeout=5)
bad.sendall(struct.pack("<IBBH", 0xDEADBEEF, VERSION, 0, 0))
try:
    ackbad = read_ack(bad)
    check("wrong magic -> refused", ackbad["accepted"] == 0, str(ackbad["accepted"]))
except (EOFError, ConnectionResetError):
    # Refused and closed. Whether the refusal itself arrives before the
    # close is a race in the kernel, not a difference in behaviour.
    check("wrong magic -> refused", True)
bad.close()

wrongver = socket.create_connection((HOST, PORT), timeout=5)
wrongver.sendall(struct.pack("<IBBH", MAGIC, 99, 0, 0))
try:
    ackwv = read_ack(wrongver)
    check("wrong version -> refused", ackwv["accepted"] == 0, str(ackwv["accepted"]))
except (EOFError, ConnectionResetError):
    check("wrong version -> refused", True)
wrongver.close()

group("input reaches the host")
# All zeroes, then a button, then zeroes again: the host log shows the
# report rate rise when something changes, which is the observable that
# the input path is live.
for _ in range(30):
    s.sendall(frame(MSG_INPUT, bytes([0] * PAD_SLOTS)))
    time.sleep(0.01)
pressed = bytearray(PAD_SLOTS); pressed[19] = 100   # A
for _ in range(30):
    s.sendall(frame(MSG_INPUT, bytes(pressed)))
    time.sleep(0.01)
for _ in range(10):
    s.sendall(frame(MSG_INPUT, bytes([0] * PAD_SLOTS)))
    time.sleep(0.01)
check("70 input messages sent without the host closing", True)

# Everything opened so far is finished with, and the host only has four
# slots. Holding them all meant the groups below could not get one --
# and it meant this suite could not be run while the console itself was
# connected, which is exactly when someone would want to run it.
s.close(); v.close(); b.close()
time.sleep(0.5)

group("a viewer's input really is dropped, not just ignored client-side")
# The observable is the host's report rate to the adapter: with
# de-duplication on, a CHANGING state raises it, an unchanging one does
# not. So driving a changing pattern and watching that rate is what
# distinguishes "the input arrived" from "the input was refused".
def peak_rate(sock, seconds, drive):
    """Host reports/s over `seconds`, optionally driving the pad.

    Measured as a BASELINE-then-DRIVEN pair rather than an absolute
    number, because this is not the only thing that can move the pad: a
    browser may be connected with a real controller plugged into it, and
    on this machine one usually is. An absolute threshold reads that as
    the viewer's input getting through.
    """
    # Read from GET /gamepad-rate rather than scraped out of a log file.
    # It used to count lines the adapter printed every few seconds, which
    # meant a diagnostic had to keep being written for this test to work
    # -- the test was the reason the log existed.
    def rate():
        with urllib.request.urlopen(WEB + "/gamepad-rate", timeout=3) as r:
            return float(r.read().decode().strip())

    # A ping every couple of seconds throughout, driving or not. The
    # host drops a client that has said nothing for ten seconds -- which
    # a real client never does, because it pings -- and the idle half of
    # this measurement is a good deal longer than that.
    def keepalive_sleep(total):
        end = time.time() + total
        while time.time() < end:
            time.sleep(min(1.0, max(0.0, end - time.time())))
            sock.sendall(frame(MSG_PING))

    # Let the adapter's own averaging window settle on the state before
    # this call, or it describes the previous group rather than this one.
    #
    # The host recomputes its rate once a second, so that -- not a round
    # number -- is what these durations have to clear. A second and a
    # half of settling and three of measuring puts at least two whole
    # windows inside the driving period whichever way the boundaries
    # fall, and two samples afterwards catch the one that closes just
    # after it.
    #
    # They used to be 5.5, 11 and 6, because the host only recomputed the
    # figure every five seconds. That was a pace chosen for a log line,
    # and this suite was paying for it: it alone took 106 of the 113
    # seconds a full run cost. The window is now its own setting.
    keepalive_sleep(1.5)
    before = rate()

    t_end = time.time() + seconds
    i = 0
    while time.time() < t_end:
        if drive:
            st = bytearray(PAD_SLOTS)
            st[19] = 100 if (i // 5) % 2 else 0
            st[11] = (i * 7) % 100
            sock.sendall(frame(MSG_INPUT, bytes(st)))
        elif i % 60 == 0:
            sock.sendall(frame(MSG_PING))
        i += 1
        time.sleep(0.016)

    # The highest the rate reached while driving. Sampled after the fact
    # as well as during, since the adapter averages over several seconds
    # and the window covering this call may only close afterwards.
    peak = before
    for _ in range(2):
        peak = max(peak, rate())
        keepalive_sleep(1.0)
    return peak

viewer = connect("")
read_ack(viewer)
# The baseline is taken on BOTH sides of the driving window and the
# higher one used. Anything else on this machine may be driving the pad
# too -- a browser with a real controller plugged into it, which is the
# normal state here -- and a baseline measured only before would be read
# as the viewer's input the moment that other source got busier.
base_before = peak_rate(viewer, 3.0, drive=False)
driven = peak_rate(viewer, 3.0, drive=True)
base_after = peak_rate(viewer, 3.0, drive=False)
base = max(base_before, base_after)
check(f"a viewer driving the pad adds nothing (idle {base_before}/{base_after}, driving {driven})",
      driven <= base + 20,
      f"{driven}/s against a {base}/s baseline -- viewer input reached the adapter")
viewer.close()

player = connect(tok)
ackp = read_ack(player)
check("re-connected as a player", ackp["may_control"] == 1)
pbase = peak_rate(player, 3.0, drive=False)
pdriven = peak_rate(player, 3.0, drive=True)
check(f"a player driving the pad does raise it (baseline {pbase}/s, driving {pdriven}/s)",
      pdriven > pbase + 10, f"{pdriven}/s vs {pbase}/s -- player input never arrived")
player.close()

group("the host counts us")
try:
    n = urllib.request.urlopen(WEB + "/clients", timeout=3).read().decode()
    check("web /clients still answers", "/" in n, n)
    # A console client counts as a viewer. It used to be invisible from
    # the page, which said nobody was connected while somebody plainly
    # was -- and the denominator is both servers' limits summed, since a
    # count that could read 12/8 is worse than one whose bottom half
    # moved.
    here, cap = n.split("/")
    watcher = connect("")
    read_ack(watcher)
    time.sleep(0.5)
    after = urllib.request.urlopen(WEB + "/clients", timeout=3).read().decode()
    check("a console client is counted as a viewer",
          int(after.split("/")[0]) == int(here) + 1, f"{n} -> {after}")
    check("the limit covers both servers", int(cap) > 8, cap)
    watcher.close()
except Exception as e:
    check("web /clients still answers", False, str(e))

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
