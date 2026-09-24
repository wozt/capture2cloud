# Capture2Cloud — development notes

This file keeps the implementation details that are easy to forget and expensive to rediscover.

For installation and normal usage, see [README.md](README.md).

---

## Current state

Capture2Cloud is functional on the hardware it was built around.

Working paths include:

* browser streaming through WebRTC or WebSocket/WebCodecs
* Android native client
* Nintendo Switch homebrew client
* Wii U console client
* direct Wii U GamePad streaming
* multiple simultaneous clients
* local Linux controller input
* Titan / ConsoleTuner controller output
* pcble2gamepad Bluetooth controller output
* script-based console wake
* Nintendo Switch 2 Bluetooth wake
* automatic controller recovery after wake

The server owns the implementation details. Clients send generic actions such as:

```text
WAKE
RESET_DONGLE
HOME
RESTART
```

They do not know which controller or wake backend is active.

---

## Main architecture

Important host-side pieces:

| File               | Role                                            |
| ------------------ | ----------------------------------------------- |
| `capture2cloud.c`  | application orchestration                       |
| `video_capture.c`  | V4L2 capture and signal-change detection        |
| `audio_capture.c`  | audio capture and local playback                |
| `gst_webrtc.c`     | browser/native encoder pipelines                |
| `web_stream.c`     | HTTP/WebSocket server                           |
| `switch_stream.c`  | native client protocol                          |
| `gamepad_bridge.c` | merges controller input and owns output backend |
| `output_titan.c`   | ConsoleTuner/Titan backend                      |
| `output_pcble.c`   | Bluetooth Nintendo controller backend           |
| `reset_method.c`   | console wake abstraction                        |
| `gtk_shell.c`      | host configuration UI                           |
| `app_config.c`     | `scripts/.env` configuration                    |

The main rule is separation of responsibilities:

```text
clients
   │
   ▼
generic protocol
   │
   ▼
Capture2Cloud host
   │
   ├── streaming backend
   ├── reset/wake backend
   └── controller-output backend
```

Do not push backend-specific behaviour into individual clients.

---

## Video paths

There are several independent audiences even when they use the same codec.

```text
Browser WebRTC
Browser WebSocket
Switch / Android
Wii U Console
Wii U GamePad
```

Routing must therefore be based on the **stream/audience**, not only the codec.

Several clients can use H.264 while requiring different resolution, frame rate or bitrate.

Encoder chains should only run while somebody needs them.

### Browser transports

WebRTC remains the preferred low-latency transport.

WebSocket/WebCodecs exists because it behaves like normal HTTP/WebSocket traffic and works through conventional reverse proxies.

The trade-off is fundamental:

```text
WebRTC / UDP
packet loss → damaged/lost frame

WebSocket / TCP
packet loss → later data waits
```

For interactive streaming, WebRTC is normally the better transport when network topology allows it.

---

## Controller architecture

Every input source is converted to the same controller state and merged by `gamepad_bridge.c`.

Sources include:

* browser
* Android
* Switch
* Wii U
* local Linux controller

The merged result is then sent through one output backend.

Currently:

```text
titan
pcble
```

JOCP/Pico remains a possible future backend.

### RESET_DONGLE

`RESET_DONGLE` no longer literally means "reset a USB dongle".

It means:

> recover the currently selected controller-output backend.

For example:

* Titan re-enumerates its USB connection.
* pcble reconnects the paired Nintendo console.

The same `gamepad_bridge_reset()` path is used by GTK, browser/native requests and automatic recovery.

---

## pcble2gamepad

The pcble backend emulates a Nintendo controller through Classic Bluetooth / BR/EDR.

The currently validated normal profile is Nintendo Pro Controller.

A separate privileged helper owns the Bluetooth stack while the virtual controller session is running. The main Capture2Cloud process remains unprivileged.

Persistent identity is always the physical Bluetooth adapter MAC, not `hci0`, `hci1`, etc.

Linux may renumber HCI controllers after rebooting or replugging them.

### Recovery

pcble recovery is intentionally bounded.

A failed reconnect currently gets at most **three real helper attempts**, then stops instead of occupying the Bluetooth adapter indefinitely.

The recovery operation is centralized in the backend.

Do not add separate reconnect loops in GTK, clients or wake code.

---

## Console wake

All wake requests end at:

```text
web_stream_wake_console()
        ↓
reset_method_wake()
```

This includes:

* GTK Overview
* GTK Maintenance
* browser `/wake`
* native `C2S_MSG_WAKE`
* Android
* Switch
* Wii U

### Script wake

The script backend typically power-cycles the console through Home Assistant.

A fixed delay was unreliable because the console takes several seconds to produce a useful picture.

Instead:

```text
run wake script
      ↓
arm video-change detector
      ↓
capture picture changes
      ↓
gamepad_bridge_reset()
```

The capture device generates its own no-signal image, so V4L2 status alone cannot tell whether the console is awake.

`video_capture.c` therefore compares samples from consecutive frames.

### Switch 2 Bluetooth wake

Switch 2 wake from sleep is now implemented.

A real wake advertisement is captured once from a paired controller and saved at:

```text
~/.local/share/capture2cloud/reset/switch2-beacon.bin
```

Permissions are `0600`.

The GTK **Reset method** page can:

* select the Bluetooth adapter
* test LE scan/advertising support
* capture a real Switch 2 wake advertisement
* replay/test the saved advertisement

The captured advertisement contains Nintendo manufacturer ID `0x0553` and the Switch 2 wake payload.

The working wake sequence is:

```text
suspend pcble recovery
        ↓
stop/release pcble Bluetooth helper
        ↓
wait for BlueZ cleanup
        ↓
transmit saved wake beacon
        ↓
wait 1 second
        ↓
gamepad_bridge_reset()
        ↓
pcble reconnects
```

This coordination matters. A pending pcble reconnect must not restart its helper while the wake code is trying to use the same Bluetooth controller.

The wake layer temporarily suspends pcble recovery, then the normal `gamepad_bridge_reset()` resumes it afterwards.

Do not implement a second Bluetooth reconnect mechanism inside `reset_method.c`.

---

## Wii U console client

`wiiu_console/` is working on real hardware.

The important path is:

```text
H.264
  ↓
H264DEC
  ↓
NV12
  ↓
custom GX2 renderer
```

The custom GX2 NV12 path is required for good performance. The SDL fallback was far slower.

The working hardware path reaches roughly **59–60 fps** under normal conditions.

Other validated pieces include:

* GamePad input
* configurable bindings
* password login
* saved sessions
* remote HOME
* 480p/720p profiles
* 30/60 fps modes
* menu overlay
* ProcUI HOME/resume handling
* stable audio playback through the native Wii U path

A geometry change requires rebuilding H264DEC before decoding the new stream.

Changing only frame rate does not.

Returning from the Wii U software keyboard also requires rebuilding the relevant SDL/GX2 state.

See [wiiu_console/README.md](wiiu_console/README.md) for client-specific details.

---

## Performance lessons worth keeping

### YUYV vs MJPEG

On the reference machine, raw YUYV is substantially cheaper to prepare for the encoder than MJPEG because no JPEG decode is required.

MJPEG remains useful when USB bandwidth is the limiting factor.

Do not assume the lowest-bandwidth capture format is also the lowest-latency or lowest-CPU option.

### Push before local presentation

The local SDL window used to call `SDL_RenderPresent()` before feeding the network encoder.

Presentation can block on monitor vsync.

The network path is now fed first.

The remote player matters more than keeping the local preview one frame ahead.

### Keep queues bounded

Interactive video must prefer recent frames over old frames.

An unbounded queue converts a short encoder slowdown into increasing latency.

The video appsrc path is therefore bounded/leaky rather than allowed to accumulate frames forever.

### Avoid excessive keyframes

A very short keyframe interval caused visible bitrate spikes and hitches.

The current longer cadence performed substantially better in testing.

### Do not flood controller output

Sending hundreds of identical controller reports per second can create queues inside downstream hardware.

Changed state should be sent immediately, but idle state can be deduplicated with periodic keepalives.

---

## Root causes worth remembering

### GCAPI packets are 64 bytes

The ConsoleTuner GCAPI packet on USB is:

```text
[type][length LE16][first][payload]
```

There is no extra leading report-ID byte on the wire.

A 65-byte packet can be accepted by USB while being silently ignored by the adapter firmware.

### Titan configuration persists in hardware

The adapter may require one-time configuration through GTuner Pro.

A software implementation can be correct while the console still ignores everything if the adapter itself is configured for the wrong output protocol.

### Protocol structs must not be decoded by guesswork

Several bugs came from reading fields at manually counted offsets.

One wrong offset can still produce perfectly valid-looking zeroes.

Keep protocol layout tests tied directly to `c2s_protocol.h`.

### Browser transport state must have one owner

An old WebSocket's delayed `onclose` once modified state belonging to its replacement, creating multiple live connections and multiple decoders drawing into the same canvas.

Callbacks must verify that they still belong to the current socket before changing shared state.

### Configuration UI is not security

Controls hidden from viewers are only presentation.

Authorization for input, wake, reset, restart and shared settings must be enforced server-side.

### Hardware disappears

Capture cards and controller adapters can disconnect while the process is running.

A recoverable device disappearance should not take the complete streaming server down.

---

## Wii U lifecycle rule

For the Wii U console client, foreground teardown order matters:

```text
network/audio
    ↓
H264DEC
    ↓
SDL/GX2
    ↓
ProcUIDrawDoneRelease
```

Resume rebuilds the required graphics, decoder and input state.

Changing this order without hardware testing is risky.

---

## Development rules

### Always run the full suite

```sh
./tests/run_all.sh
```

after meaningful changes.

### Keep secrets out of git

`scripts/.env` contains real configuration and credentials and must remain untracked.

### Runtime logs

High-volume runtime logs belong in `/dev/shm`, not persistent storage.

### Front-end script order matters

The browser front end currently uses multiple traditional scripts sharing one global scope.

Their load order is part of the application.

When adding/removing a front-end file, keep the HTML loader, server static-file list and test loader consistent.

### Stored setting formats

If the **shape** of a persisted browser setting changes, bump `SETTINGS_VERSION`.

Adding a new independently defaulted key does not necessarily require a bump.

---

## Security

Control actions require server-side authorization when `PLAYER_PASSWORD` is configured.

This includes:

* gamepad input
* `/wake`
* `/reset-dongle`
* `/restart`
* stream settings that affect everyone

Login tokens are generated from `/dev/urandom` and kept in server memory plus the browser tab's `sessionStorage`.

Failed logins use a server-wide lockout rather than a per-connection delay, so parallel guesses do not bypass throttling.

Known deployment limitations remain:

* HTTP itself is unencrypted.
* `Host` is not currently validated against an allowlist.
* running without `PLAYER_PASSWORD` intentionally leaves control endpoints open.

Use a trusted network, VPN or suitable reverse proxy when exposing the service beyond the local machine.

---

## Still open

### Networking

WebRTC still lacks a finished STUN/NAT traversal configuration.

WebSocket remains the practical remote/proxy-friendly transport.

### Browser latency

Audio remains slightly behind the lowest-latency video/input path.

Measure before changing capture or buffering APIs.

### Long sessions

Several causes of stalls have already been removed:

* unbounded queues
* excessive keyframes
* controller-report buildup

Long-session behaviour should still be monitored.

When a freeze happens, check whether the capture loop itself logged a frame gap. That separates capture-side stalls from downstream encoder/network problems.

### Windows

A Windows host/server implementation remains future work.

### Controller backends

Possible future work includes the JOCP/Pico backend and additional Bluetooth controller profiles.

---

## Things not to lose

* The Android signing key needs a backup.
* Development access tokens that have ever appeared in plaintext should be revoked/replaced.
* Hardware-specific behaviour should be validated on the actual console before replacing a working path because another implementation looks cleaner.

---

## Principle

The project has accumulated enough working hardware-specific behaviour that the safest rule is:

> **keep clients generic, keep hardware quirks inside their backend, and change one layer at a time.**

That is what allows browser, Android, Switch and Wii U clients to keep working while controller and wake implementations evolve independently.
