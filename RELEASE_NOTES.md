# Capture2Cloud 1.4.0.1

This release adds a complete **native Wii U console client**, expands Capture2Cloud's multi-client architecture, and reorganises the Linux host into a clearer server/control interface.

Capture2Cloud can now serve the browser, Android, Nintendo Switch, Wii U console and a real Wii U GamePad from the same host, while keeping the stream paths appropriate for each target.

## Native Wii U console client

The biggest addition is `wiiu_console/`, a full Capture2Cloud client running directly on Wii U hardware.

The validated video/audio path uses:

* **H264DEC** for hardware H.264 decoding
* a custom **GX2 zero-copy renderer** for decoded NV12 surfaces
* **AX** for direct 48 kHz stereo PCM playback

The Wii U client also includes:

* GamePad controller input
* remappable controls
* 480p and 720p stream profiles
* 30 and 60 FPS modes
* configurable bitrate
* stick deadzone, range and Y-axis settings
* password login through the Wii U software keyboard
* saved session tokens
* diagnostics
* selectable display targets
* remote HOME support
* normal Wii U HOME-menu suspend, resume and exit behaviour

Both `.rpx` and `.wuhb` builds are included in the release.

## Multi-client streaming

Capture2Cloud no longer assumes every client should receive the exact same video path.

The host now supports several client families with transports and encode paths suited to their hardware:

* Browser — WebRTC or WebSocket
* Android — native H.264 transport
* Nintendo Switch — native H.264 transport
* Wii U console — dedicated H.264 + PCM path
* Wii U GamePad — dedicated DRC path

Unused paths are not fed, while clients that share the same path can share its encoder.

The development setup has been tested with **four different client families connected simultaneously**, with all four remaining usable at the same time.

## Wii U video results

On the developer's Wii U setup, the native Capture2Cloud client showed **fewer visible video artefacts than Moonlight-WiiU during comparable use**.

This is an observation from the tested hardware and network configuration, not a claim that Capture2Cloud will outperform Moonlight on every system.

## New Linux server interface

The GTK application has been reorganised into a proper Capture2Cloud control panel, with dedicated sections for:

* server overview
* connected clients
* controller output
* capture
* local monitor
* maintenance

Server and client state is now surfaced more clearly in the UI.

## Pluggable controller output

Controller input from browsers, native clients and local controllers is merged into a common controller state before reaching a selectable output backend.

Current and planned backends are:

* **Titan / ConsoleTuner USB** — current implementation
* **Joy-Con 2 Bluetooth / pcble2joycon2** — planned
* **JOCP / Raspberry Pi Pico 2 W** — planned

This separates client input from the hardware used to present that input to the captured console.

## Wii U GamePad cleanup

The Wii U GamePad client also received maintenance work for this release, including removal of the obsolete `--no-deauth` option after the connection/recovery logic made it ineffective.

The GamePad build continues to use the modified x264 ABI required by libdrc alongside the system FFmpeg/x264 libraries.

## Downloads

This release includes:

* Linux x86_64 host
* Android APK
* Nintendo Switch homebrew client
* Wii U console client
* Wii U GamePad client
* SHA-256 checksums

## Full changelog

All changes since 1.3.0.1:

https://github.com/wozt/capture2cloud/compare/v1.3.0.1...v1.4.0.1
