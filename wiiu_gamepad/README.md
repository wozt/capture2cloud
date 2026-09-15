# Capture2Cloud on a real Wii U GamePad

A native client like the Switch homebrew: it connects to the console
port, decodes the same H.264, and sends the same twenty-one bytes of
controller state back. What is different is where the picture goes —
over the air to a GamePad, through
[libdrc](https://github.com/wozt/libdrc) and a Realtek adapter
pretending to be a Wii U.

**Status: written, builds, not yet run against a pad.** Everything it
does was measured in the sibling project; nothing in *this* copy has
been in front of the hardware. Treat the first run as a test.

---

## Turning it on

The toggle is in the app's settings window — **serve to wii u gamepad**,
under the console port it connects to. `WIIU_PAD_AUTOSTART=1` in the
`.env` starts it with the host.

The host starts and stops this program; it is not a thread in there.
Three reasons, in `../wiiu_pad.h`, and the first decides it on its own:
the host builds from one `gcc` line over a list of C files, and this
needs C++ and two vendored libraries.

It can also be run by hand, which is what to do the first time:

```sh
./wiiu_pad --port 5081 --no-pad     # decode only: is the host talking?
./wiiu_pad --port 5081              # the real thing
./wiiu_pad --port 5081 --stats      # with libdrc's own counters
```

`--no-pad` exists so that a fault in the half that can be tested is not
blamed on the half that cannot. Use it first, every time.

---

## Building

Not part of the host's build, on purpose: a machine with no Realtek
adapter has no use for it.

```sh
make
```

The two libraries are in `vendor/` beside this file, copied from the
sibling project. They are **not in git** — binaries built on one machine
are nobody else's answer — so a fresh checkout has to fetch them again:

```sh
cp -a ../../bottom_screen_server/gamepad/vendor .   # or:
make VENDOR=../../bottom_screen_server/gamepad/vendor
```

`cp -a`, not `-r`: `libx264.so` is a symlink to `libx264.so.140`, and a
dereferenced copy still links but misleads the next person to read the
directory. The Makefile prints both lines if it cannot find them.

| | Why it cannot be the system one |
|---|---|
| `libdrc` | the protocol itself; there is no packaged version |
| `drc-x264` | x264 with DRH slicing and no slice header, which the GamePad requires and upstream x264 has no notion of |

A machine with ffmpeg also has a system x264, and both end up loaded in
one process. That is safe and checked rather than assumed: x264 stamps
its build number into every exported symbol, so libdrc asks for
`x264_encoder_open_140` and only the vendored library defines it. The
linker warns; ignore it.

---

## Before it can work at all

The radio stack has to be up: the driver loaded with `disable_ips=1`,
hostapd running on 5 GHz with a Nintendo OUI, MTU 1800, and the pad
paired. All of it, measured, is in [docs/WIIU_GAMEPAD.md](docs/WIIU_GAMEPAD.md)
— a copy; the sibling project is upstream for it.

What is here to run it with:

| | |
|---|---|
| `docs/WIIU_GAMEPAD.md` | the measured account: driver, AP, pairing, encoder, transport |
| `docs/rtw88_TSF.md` | the driver fork and its TSF counter |
| `conf/wiiu_pair.conf`, `conf/wiiu_normal.conf` | hostapd templates, placeholders not credentials |
| `tools/ap-pair.sh` | brings the pairing AP up, arms WPS, switches to the normal AP |
| `tools/reload_rtw88.sh` | unloads and reloads the driver |

`ap-pair.sh` still needs the forked hostapd, which is a whole tree and
cannot be vendored here. Point `DRC_HOSTAP` at your build of it.

Two failures that look like something else:

- **"libdrc would not start"** often means another copy is running.
  It holds three UDP ports and only one process may have them:
  `ss -lunp | grep -E ':5001[0-9]|:5002[0-9]'`.
- **Buttons ignored, picture fine** means no token. The host says
  `may_control=0` and this prints `watching only`. Set `C2S_TOKEN` or
  pass `--token`.

---

## What it does that is not obvious

**It does not ask the host for a size.** `C2sShared` says the
resolution, the frame rate and the bitrate belong to every native client
at once, so a pad asking for its own 848×480 would drag the browser and
the Switch down with it. It takes what the stream is and scales locally
— one extra resample, nobody else affected.

**It letterboxes.** The panel is 864×480 and 16:9 lands as 848×480 with
eight pixels of black each side.

**It does not negate the sticks.** libdrc already reports them the way
this protocol wants — up is positive, which is what `gamepad_bridge.h`
says about `GAMEPAD_XB360_LY`. This was got wrong three times in the
sibling project by reasoning from the screen's Y axis.

**It deauthenticates the pad three frames in.** A pad that is already
associated will not accept a new video transport. The streamer is
started *before* that cycle: starting it afterwards leaves audio silent.

**It escalates when the picture stops** rather than blocking for ever:
a keyframe at 2 s, the codec asked again at 4 s, the connection dropped
at 30 s.

---

## What is missing

- **Touch goes nowhere.** The pad has a touch screen; a console driven
  through a ConsoleTuner adapter has no touch input. The three honest
  options are in section 5 of `../WIIU_GAMEPAD_HANDOVER.md`; none is
  implemented, and touch is currently read and discarded.
- **No on-screen menu.** The sibling project draws one into the video —
  stick deadzones, resolution, filter — opened by touching a corner.
  Worth having, and it is where touch would first earn its keep.
- **No stick deadzone.** The same project needed 12% on the right stick
  because that pad drifts. If yours does, that is why.
- **Two lossy passes.** The host encodes, this decodes, libdrc encodes
  again at a quantiser pinned to 32. Section 9 of the handover describes
  how to remove the first one — a fourth encoder chain, `SS_STREAM_DRC`,
  fed by drc-x264 only while a pad is connected. It needs a small change
  in libdrc first.
