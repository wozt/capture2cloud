# Capture2Cloud

Play your game console from a browser, on any device, over your network.

Capture2Cloud captures HDMI from a USB capture card, streams it to a
browser with WebRTC at low latency, and relays the browser's input back
to the console through a USB adapter — so a phone, tablet or laptop
becomes a working controller for real hardware.

> **Status:** working personal project, not a polished product. It runs
> daily on the setup it was built for. See
> [WORKINPROGRESS.md](WORKINPROGRESS.md) for the detailed state, known
> limitations and what's next.

---

## What it does

- **Low-latency video/audio** — V4L2 capture (raw YUYV by default, MJPEG
  for bandwidth-limited hosts), VP8 video and Opus audio over WebRTC.
  Multiple viewers share a single encode.
- **Play from the browser** — a real gamepad, an on-screen touch pad, or
  keyboard and mouse. Input travels over a WebRTC DataChannel to a
  ConsoleTuner adapter (Titan One and relatives) plugged into the
  console, which the console sees as an ordinary controller.
- **Watch or play** — anyone with the link can watch; controlling the
  console requires a password. Enforced server-side, not just hidden in
  the UI.
- **Native window too** — the capture also shows in a local SDL window
  with a small GTK control bar, which is handy for screen sharing over
  Discord and similar.
- **Wake the console** — optional: power-cycles a Home Assistant smart
  plug, since cutting and restoring power wakes some consoles from
  sleep.

### On-screen touch controls

A transparent, multitouch overlay in the style of RetroArch: two sticks,
a d-pad with real diagonals, face buttons, shoulders and triggers.
Xbox / PlayStation / Nintendo skins, symmetric or asymmetric stick
placement, adjustable colour and opacity, and a "move buttons" mode that
lets you drag any control where your thumbs actually are — saved per
browser.

### Keyboard, mouse and remapping

Keyboard and mouse can drive the virtual pad (mouse on the right stick
via Pointer Lock), with named, editable binding profiles. Real gamepads
get their own remapping panel — press a button, bind it — stored per
controller, because browsers report different controllers differently
and no single hardcoded mapping fits them all.

---

## How it fits together

```
 HDMI ──► USB capture card ──► Capture2Cloud ──► WebRTC ──► browser
                                    │                          │
                                    │       gamepad state       │
                                    │  ◄─── (DataChannel) ──────┘
                                    ▼
                            USB adapter (Titan One)
                                    │
                                    ▼
                                 console
```

The console is driven by a real USB adapter that emulates a controller,
so no modification or homebrew is needed on the console side.

---

## Requirements

**Hardware**

- A USB HDMI capture card (UVC/V4L2 compatible — developed against a
  MACROSILICON USB3 device)
- A ConsoleTuner adapter — Titan One, Cronus, CronusMAX — if you want to
  *control* the console. Without one, everything still works as a
  view-only stream.
- Linux with X11 (the local window uses SDL2 + GTK3 and an X-specific
  docking hint)

**Software** — on Debian/Ubuntu and derivatives, one script handles it:

```sh
./scripts/install_deps.sh
```

It installs the build tools, SDL2/GTK3/PulseAudio, the GStreamer stack
(including the `nice` plugin WebRTC needs for ICE), libusb and
`v4l-utils`; adds a udev rule so the gamepad adapter is reachable
without root; and creates `scripts/.env` from the template. Add `--dev`
for the browser-test tooling, or `--no-udev` to skip the USB rule.

On other distributions the script prints the list of equivalent
packages to install by hand.

---

## Installing

```sh
git clone <this-repo> capture2cloud
cd capture2cloud
./scripts/install_deps.sh
$EDITOR scripts/.env
```

At minimum set `VIDEO_DEVICE` and `AUDIO_SOURCE` for your capture card:

```sh
v4l2-ctl --list-devices     # find the video device
pactl list short sources    # find the audio source
```

Prefer a stable `/dev/v4l/by-id/...` path — plain `/dev/videoN` numbers
change across reboots.

Then build and run:

```sh
gcc -O2 -Wall -Wextra -o capture2cloud \
  capture2cloud.c video_capture.c audio_capture.c gtk_shell.c \
  web_stream.c gst_webrtc.c gamepad_bridge.c app_config.c \
  $(pkg-config --cflags --libs sdl2 libpulse-simple libjpeg gtk+-3.0 \
    x11 gstreamer-1.0 gstreamer-app-1.0 gstreamer-webrtc-1.0 \
    gstreamer-sdp-1.0 libswscale libusb-1.0) -lm

./capture2cloud                # local window + GTK control bar
./capture2cloud --headless     # no window; the web page is the interface
./capture2cloud --help
```

Two launchers sit one directory up. Both build if needed, share the same
build recipe and device checks, and toggle — run one again to stop it:

| | |
| --- | --- |
| `toggle_capture2cloud.sh` | Local window plus a GTK control bar. Convenient to bind to a keyboard shortcut. |
| `toggle_capture2cloud_headless.sh` | No window at all. For SSH, a systemd unit, or a machine with no desktop. |

They refuse to start while the other is running: only one instance can
hold the capture card and the port. Stopping works off the process table
rather than a pid file, so an instance whose pid file was lost is still
found and stopped.

Everything is resolved relative to the binary, so the directory can live
anywhere and be renamed freely.

### USB permissions

`install_deps.sh` sets this up. If you skipped it, the adapter's USB
device is root-only by default; grant access to the `plugdev` group:

```sh
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2508", MODE="0660", GROUP="plugdev"' | \
  sudo tee /etc/udev/rules.d/99-capture2cloud-adapter.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG plugdev "$USER"   # then log out and back in
```

### Adapter one-time setup

ConsoleTuner adapters need their output profile configured **once** with
the vendor's GTuner software (Windows, or a VM) before software
injection has any effect. Without it the USB writes succeed but the
console ignores them. The setting persists on the device afterwards.

---

## Using it

1. Start the app, then enable **Stream → Stream to browser** from the
   control bar (or set `WEB_AUTOSTART=1`). In `--headless` there is no
   control bar, so the stream always starts on its own.
2. Open `http://<this-machine>:5080` on any device on the network.
3. Click **start stream** to allow playback with sound.
4. To control the console, click **log in to play** and enter
   `PLAYER_PASSWORD`. Then pick your input from the *gamepad* dropdown:
   a detected controller, **Virtual buttons (touch)**, or
   **Keyboard/mouse**.

Without a password set, control is open to anyone who opens the page --
including a cross-origin POST from any site the user visits, which can
reach `/wake` and power-cycle the console. Set `PLAYER_PASSWORD`.

On the machine itself there is an icon in the notification area. Right
click gives *Settings*, *Send controller input* and *Quit*; the settings
window mirrors the page's controls, grouped the same way. Nothing there
asks for a password: the person at that keyboard is at the machine the
console is plugged into, and a login would guard a door they are standing
behind.

The video window is an ordinary window with its own decorations. It used
to be borderless with a GTK menu bar glued above it, following its moves,
mirroring its minimise and faking a fullscreen by moving both -- a great
deal of machinery to imitate one window out of two, fighting the window
manager the whole way. Closing it now hides it rather than quitting:
closing a monitor is not stopping a capture other people are watching.

The console's port is settable from the same window and defaults to
5081. It is its own port because the two streams are two servers -- the
browser's is HTTP, the console's a small binary protocol -- and moving
one has no reason to move the other. Changing it disconnects whatever is
connected, since a listening socket cannot be moved, so it has to be
changed on the console as well; the client has a field for it.

The tray icon is there in headless mode too: headless means no video
window, not no desk. The launcher passes a display through when there is
one, and over ssh or from a unit file the program says once that it has
no tray and carries on capturing.

A controller plugged into this machine can drive the console directly,
which until now needed a browser open on the machine the console is next
to. It is one more source into the same merge the browser and the console
client use, so several hands combine rather than fight. Off in headless
mode, where the local speakers are not opened either -- nobody is sitting
at a machine with no screen, and the stream is unaffected since that
output only ever fed a monitor.

The control bar is one line: mute, volume, and who you are. Everything
else lives in a group -- *stream*, *picture*, *controls*, *touch pad*,
*console* -- that opens over the video, one at a time. The volume slider
reads 0-100%, where 100% is four times the stream's own level; turning it
down is done by the video element itself, so zero is silent whether or
not the amplification graph is running.

The capture format defaults to **YUYV**, measured at 149% of a core
against 186% for MJPEG on the whole process at 1080p60 with a browser and
the console both watching. MJPEG costs a JPEG decode that YUYV does not;
what it buys is a fraction of the bytes over USB, which matters if the
USB3 path is shared. Switchable live from the page, and the choice is
remembered per browser.

MJPEG frames are decoded straight into the JPEG's own YUV planes rather
than into RGB. Everything downstream wants YUV, so decoding to RGB meant
converting it back -- a colour-space round trip over two million pixels,
sixty times a second, to undo what libjpeg had just done. Measured at
1080p60 with a browser and the console both connected, the whole process
went from 226% of a core to 155%.

H.264 for the Switch client is encoded on a GPU when one can do it --
both of this machine's can -- preferring the integrated one, which is
idle while the discrete card drives the display. It costs 47% of a core
at 720p60 against 243% for x264. `SWITCH_H264_ENCODER` in the .env forces
a particular one (`x264enc` to stay on the CPU). VP8 has no hardware
encoder on AMD and stays on the CPU.

Nothing is logged on a cycle. The adapter's report rate, the capture's
frame gaps, the SDP of every negotiation, the "this client's link is
behind" line -- all of them are behind `VERBOSE=1` in the .env. What is
left is errors, connections and state changes, which is a few dozen lines
for a whole session. The log and pid files live in `/dev/shm`, which is
RAM on every Linux system; `/tmp` only sometimes is.

The adapter's live report rate is readable at `GET /gamepad-rate` instead
-- one number, no authentication, and the way to see from outside that
input is actually reaching the console.

Five failed logins stop attempts for everyone for 30 seconds, so a weak
password is not brute-forceable at speed; it is still worth changing from
the `changeme` default. Traffic is plain HTTP, so passwords and session
tokens cross the network in clear unless a tunnel (Tailscale) or a TLS
reverse proxy carries them.

**restart server** in the *console* group stops and starts the capture
program, keeping its pid, its arguments and its log. Sound has been seen
to stop arriving with everything still claiming to work, and this is the
one thing that has always brought it back -- so it is a button rather
than a trip to the machine. The page waits for the server to answer
again and reloads itself; measured, the whole thing takes under two
seconds. Players only, refused server-side. The Switch client has the
same entry, and reconnects on its own.

Several players can be logged in at once, and their input is combined
rather than fought over: a button is pressed if anyone is pressing it,
and a stick takes the largest deflection anyone is giving it. Whoever
sent last used to win outright, which sounds like a hand-over and is not
one -- the page sends its state on every animation frame whether or not
anything changed, so a second person merely having it open wrote zeroes
over the first person's input sixty times a second. A player who
disconnects releases what they were holding.

A viewer who has not logged in gets the picture and the sound, and
nothing else: no gamepad controls, no video settings, and no controller
detection at all. Refused server-side, not merely hidden — gamepad input,
`/wake` and `/quality` all check the session. The bitrate in particular is
shared: one encoder feeds every client, so it is not a viewer's to change.

### Remote access

Only the page and the initial handshake go over HTTP; the actual media
and input travel peer-to-peer over UDP. Reaching it from outside your
network therefore needs more than a reverse proxy — see the "remote
access" goal in [WORKINPROGRESS.md](WORKINPROGRESS.md).

---

## Configuration

Everything lives in `scripts/.env` (git-ignored), read by both the
application and the shell scripts. See
[`scripts/.env.example`](scripts/.env.example) for the annotated list.

| Key | Purpose |
| --- | --- |
| `VIDEO_DEVICE` | V4L2 capture device |
| `AUDIO_SOURCE` | PulseAudio source for capture audio |
| `CAPTURE_FORMAT` | `mjpeg` (default; less USB bandwidth, ~50 points of a core more for the decode) or `yuyv` (raw, lowest CPU). Also switchable while running, from the page, and remembered per browser. |
| `GAMEPAD_DEDUP` | Only send a controller report when the state changes, plus a keepalive (default 1). `0` sends on every pass, as it used to. |
| `LOCAL_PLAYBACK` | Also play the captured audio on this machine (default 1) |
| `GAMEPAD_USB_VID` / `_PID` | Force a specific adapter (default: auto-detect) |
| `WEB_PORT` / `WEB_AUTOSTART` | Web server port, and whether it starts on launch |
| `MAX_CLIENTS` | Simultaneous browser clients (capped at 32) |
| `RTP_MTU` | `auto` sizes packets to the link toward each client; a number forces it |
| `KEYFRAME_MAX_DIST` | Frames between forced keyframes (300 = one every 5s at 60fps) |
| `PLAYER_PASSWORD` | Password to control the console; empty = open |
| `HA_URL` / `HA_TOKEN` / `HA_PLUG_ENTITY` | Home Assistant, for waking the console |

---

## Development

```sh
./tests/run_all.sh          # C unit tests + JavaScript suite
```

Browser tests need the app running, plus a one-time
`npm install && npx playwright install chromium`:

```sh
node tests/browser/smoke.js          # page loads, WebRTC connects, frames arrive
node tests/browser/ui_shots.js       # renders the UI and screenshots it
node tests/browser/watch_frames.js   # gaps between presented frames (hitches)
node tests/browser/measure_latency.js # browser-side latency budget
node tests/browser/ice_candidates.js  # what each side offers for connectivity
node tests/security/attack.js         # attack traffic against a running instance
```

The measurement scripts are how the hitch and latency questions in
WORKINPROGRESS.md were settled — worth reaching for before changing
anything that claims to affect smoothness or delay.

`page.html` and `app.js` are served straight from disk — edit and
refresh the browser, no rebuild.

Architecture, protocol notes and the reasoning behind the trickier parts
(latency, the GCAPI wire format, threading) are documented in
[WORKINPROGRESS.md](WORKINPROGRESS.md) and in comments next to the code
they explain.

---

## Acknowledgements

The GCAPI protocol handling was reverse-engineered with help from
[GIMX](https://github.com/matlo/GIMX)'s source and USB captures of the
vendor's own software.

## Licence

Not yet chosen — treat as all rights reserved for now.
