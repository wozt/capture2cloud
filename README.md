<img src="assets/icon-256.png" width="96" align="left" alt="">

# Capture2Cloud

**Play your game console from a browser, phone, Nintendo Switch or Wii U — anywhere on your network.**

<br clear="left">

Capture2Cloud takes the HDMI output of a console through a USB capture card,
streams it to one or more clients, and sends controller input back to the
console.

Controller input from browsers, native clients and a controller connected to
the host is merged into one common state, then sent through a selectable
controller-output backend.

> **Status:** a working personal project, not a product. It runs daily on
> the setup it was built for. [WORKINPROGRESS.md](WORKINPROGRESS.md) contains
> development notes, measurements and known limitations.

---

## Features

* Low-latency video and audio streaming
* Browser client with WebRTC or WebSocket/WebCodecs
* Native Android client
* Nintendo Switch homebrew client
* Native Wii U console client
* Direct Wii U GamePad streaming
* Multiple clients at the same time
* Password-protected controller access
* Touch controls, keyboard/mouse and physical controllers
* Local controller input on the Linux host
* Pluggable controller-output backends
* Live input and output stick shaping
* Optional Home Assistant console wake-up

---

## Architecture

Video paths:

* HDMI capture card -> Capture2Cloud
* Browser -> WebRTC or WebSocket
* Android / Nintendo Switch -> native H.264 transport
* Wii U console -> dedicated H.264 path
* Wii U GamePad -> dedicated DRC path

Controller inputs from all clients and the local Linux controller are merged by
the host.

The resulting controller state is sent through one selected output backend:

* **Titan / ConsoleTuner USB**
* **pcble2gamepad Bluetooth**
* **JOCP / Raspberry Pi Pico 2 W** — planned

Clients do not need to know which output backend is active.

The historical `RESET_DONGLE` command therefore means **recover the currently
selected controller-output backend**: Titan can recover its USB connection,
while pcble2gamepad reconnects the paired console.

---

## What you need

### Required

* **Linux PC**
* **USB HDMI capture card** supported by V4L2/UVC
* a console with HDMI output
* another device to use as a client

Capture2Cloud was mainly developed with a MACROSILICON USB3 capture device.

### Controller output

To actually control the console, you currently have two choices.

#### Titan / ConsoleTuner

A compatible USB controller adapter such as:

* Titan One
* Cronus / CronusMAX
* another supported ConsoleTuner-compatible adapter

#### pcble2gamepad

A **Linux-compatible Bluetooth dongle supporting Classic Bluetooth / BR/EDR**.

One Bluetooth adapter is enough for Pro Controller emulation.

Using a dedicated USB Bluetooth dongle is recommended because pcble2gamepad
temporarily takes control of the Bluetooth stack while the virtual controller
session is running.

The experimental Joy-Con pair mode requires two Bluetooth adapters.

Without an output backend, Capture2Cloud still works as a view-only streaming
server.

---

## Getting started

```sh
git clone https://github.com/wozt/capture2cloud
cd capture2cloud

./scripts/install_deps.sh
$EDITOR scripts/.env
```

At minimum, configure your capture devices:

```sh
v4l2-ctl --list-devices
pactl list short sources
```

Example:

```ini
VIDEO_DEVICE=/dev/video0
AUDIO_SOURCE=alsa_input.usb-MACROSILICON...
PLAYER_PASSWORD=something
```

Then start Capture2Cloud:

```sh
./toggle_capture2cloud.sh
```

or without the local window:

```sh
./toggle_capture2cloud_headless.sh
```

Open:

```text
http://<host>:5080
```

Run the launcher again to stop the server.

---

## pcble2gamepad Bluetooth backend

Capture2Cloud includes an integrated version of **pcble2gamepad**.

Instead of sending controller state to an external USB adapter, the Linux host
itself appears to the console as a Nintendo controller over Classic Bluetooth.

The currently validated profile is **Nintendo Pro Controller**.

Install the privileged helper once:

```sh
sudo ./scripts/install_pcble_backend.sh
```

The main Capture2Cloud process remains unprivileged. Only the separate Bluetooth
helper temporarily takes control of BlueZ.

The **Controller output** page provides:

* Bluetooth adapter discovery
* adapter selection
* controller profile selection
* **Pair / Sync new Switch**
* **Reconnect paired Switch**
* saved pairing information
* connection status
* controller colour configuration

Capture2Cloud remembers both:

* the paired console Bluetooth address
* the physical Bluetooth adapter MAC address

The adapter is remembered by MAC rather than by `hci0`, `hci1`, etc., because
Linux may assign a different HCI number after rebooting or replugging USB
devices.

The same pcble recovery operation is used for:

* automatic reconnect at startup
* **Reconnect paired Switch**
* the Maintenance recovery button
* browser reset requests
* native-client `RESET_DONGLE` requests

The recovery logic belongs to the backend rather than GTK, so it also works with
a headless Capture2Cloud server.

---

## Controller input and shaping

### Local controller

A controller connected directly to the Linux host can be selected from the
**Local controller input** page.

The selected controller is remembered by SDL GUID rather than by its temporary
device index, so normal replugging or rebooting should not require selecting it
again.

### Input shaping

Input shaping affects only the local physical controller.

Available controls include:

* dead zone
* stick range
* diagonal range
* trigger threshold
* Y-axis inversion

### Output shaping

Output shaping happens **after all controller sources have been merged**.

It therefore applies equally to input coming from:

* browser
* Android
* Nintendo Switch
* Wii U
* local Linux controller

Changes are applied live.

The GTK interface also draws the resulting left and right stick shapes so dead
zones, cardinal saturation and diagonal saturation can be seen directly.

### Nintendo stick calibration

The pcble backend sends raw 12-bit Nintendo stick values and advertises matching
factory calibration data to the console.

Keeping those two sides consistent fixes the case where full cardinal movement
could stop short while diagonal movement reached the edge of the Nintendo
calibration screen.

---

## Playing

Click **start stream**, then **log in to play** and enter the configured
password.

Depending on the client, input can come from:

* physical controller
* virtual touch controls
* keyboard and mouse
* the controller connected locally to the Capture2Cloud host

All controller sources eventually become the same common controller state on
the server.

The browser touch interface provides sticks, d-pad, face buttons, shoulders and
triggers, and allows the controls to be repositioned.

Physical browser controllers have their own remapping configuration.

Keyboard and mouse use the same virtual controller path, with pointer lock
available for right-stick control.

---

## Browser transports

The browser supports two video transports.

### WebRTC

The default low-latency path.

Media travels directly over UDP, making packet loss preferable to blocking
later frames.

### WebSocket / WebCodecs

Carries the native Capture2Cloud stream over ordinary TCP/WebSocket traffic.

This is useful behind conventional HTTP proxies such as Nginx, Cloudflare or
Authelia, although TCP packet loss can stall later data and therefore behaves
worse than WebRTC on lossy links.

Each encode path runs only while at least one client needs it.

---

## Native clients

### Nintendo Switch

`switch_homebrew/`

Native H.264 client with controller and touch input.

### Android

`android/`

Hardware H.264 decoding with Bluetooth/USB controllers, touch controls and
automatic bitrate handling.

### Wii U console

`wiiu_console/`

A native Wii U client using:

* **H264DEC** hardware video decoding
* custom **GX2 NV12 zero-copy rendering**
* GamePad controller input
* remappable bindings
* 480p / 720p modes
* 30 / 60 FPS profiles
* password login through the Wii U software keyboard
* saved session tokens
* diagnostics
* display-target settings
* remote HOME

Normal Wii U HOME-menu suspend/resume/exit behaviour has been validated on
hardware.

See [`wiiu_console/README.md`](wiiu_console/README.md).

### Wii U GamePad

`wiiu_gamepad/`

Capture2Cloud can also stream directly to a real Wii U GamePad through the
libdrc-based path, with controller input returned to the same host-side bridge.

---

## Shared settings

There is one capture device and one controller output, so some settings are
shared between clients.

A newly connected client receives the current server state instead of
overwriting everybody else's configuration with whatever it had saved locally.

Client-only settings such as UI layout, touch controls and local presentation
remain local.

See [SHARED_SETTINGS.md](SHARED_SETTINGS.md) for the complete split.

---

## Configuration

Configuration lives in:

```text
scripts/.env
```

The annotated template is:

```text
scripts/.env.example
```

Important settings include:

| Setting                                | Purpose                                     |
| -------------------------------------- | ------------------------------------------- |
| `VIDEO_DEVICE`                         | V4L2 capture device                         |
| `AUDIO_SOURCE`                         | PulseAudio capture source                   |
| `PLAYER_PASSWORD`                      | Password required to control the console    |
| `CAPTURE_FORMAT`                       | Capture format such as `yuyv` or `mjpeg`    |
| `WEB_TRANSPORT`                        | Default browser transport: `webrtc` or `ws` |
| `WEB_PORT`                             | Browser/server port                         |
| `SWITCH_PORT`                          | Native client port                          |
| `GAMEPAD_OUTPUT_BACKEND`               | `titan` or `pcble`                          |
| `TITAN_OUTPUT_PROTOCOL`                | Titan emulation protocol                    |
| `PCBLE_CONTROLLER`                     | pcble Nintendo controller profile           |
| `PCBLE_PRIMARY_ADAPTER`                | Preferred Bluetooth adapter                 |
| `PCBLE_PAIRED_ADAPTER`                 | Adapter MAC that owns the stored pairing    |
| `PCBLE_SWITCH_ADDRESS`                 | Stored paired console Bluetooth address     |
| `LOCAL_SINK`                           | Optional local audio output                 |
| `HA_URL`, `HA_TOKEN`, `HA_PLUG_ENTITY` | Optional Home Assistant wake support        |

Input and output shaping also have corresponding `INPUT_*` and `OUTPUT_*`
settings in `.env`.

---

## Development

Run the complete test suite with:

```sh
./tests/run_all.sh
```

The normal launch scripts rebuild the Linux host and pcble helper when their
sources change.

The repository contains separate code for the native clients because each
platform has very different video, audio and UI requirements, while the common
controller protocol and server-side bridge remain shared.

Additional design notes and measurements are available in
[WORKINPROGRESS.md](WORKINPROGRESS.md).

---

## Acknowledgements

The GCAPI protocol handling was reverse-engineered with help from
[GIMX](https://github.com/matlo/GIMX)'s source and USB captures of the vendor
software.

The Bluetooth controller backend incorporates work developed as
**pcble2gamepad** and the upstream projects credited by its vendored source and
licence files.

---

## Licence

Not yet chosen — treat as all rights reserved for now.
