# WUP Stream transport for Capture2Cloud

Third independent Wii U GamePad route: a GamePad running **WUP Stream** from
`wozt/wup-stream` / `wup-homebrew`, connected over the LAN. The other two Wii U
routes (`wiiu_gamepad/` direct Nintendo radio and `wiiu_console/` RPX) remain
unchanged. Capture2Cloud supplies HDMI video and filtered HDMI audio; it never
captures the host's X11 desktop for this transport.

## Build (once, or after updating this component)

```sh
./scripts/build_wupstream.sh
```

The script pins upstream `wozt/wup-stream` commit
`8609916410a2c7b0ef7f052e00d788538e2ca43e` and builds its patched x264
in a local, Git-ignored `wiiu_wupstream/build/`. `wup_stream_helper` links
against that static library in a **separate process**: mixing its special
x264 ABI with GStreamer and libdrc in Capture2Cloud would be unsafe.

Enable **WUP Stream via Wi-Fi** on the **Client · Wii U GamePad** page, or set:

```
WUP_STREAM_AUTOSTART=1
WUP_STREAM_PAD_IP=
WUP_TOUCH_MODE=relative
```

An empty IP means automatic discovery. The direct Nintendo AP path and WUP
Stream cannot both be enabled from the GTK UI. The application on the GamePad
must already be installed and waiting for a stream. No microphone input is
forwarded in this version.

## Input routing

`gamepad_bridge_update(GAMEPAD_SOURCE_WUPSTREAM, ...)` handles physical
buttons and sticks; controls go to the same Titan/PCBLE backend as every other
client. When PCBLE selects a Joy-Con 2 profile, touchscreen gestures also go
through `output_pcble_mouse()`:

* **Relative**: touchpad movement, tap for left click, move/drag with left
  button, long press for right button.
* **Absolute (virtual)**: finger X/Y (0..853/0..479) is converted into the
  relative delta needed to reach the desired point in a **software pointer
  estimate**. The Joy-Con 2 HID mouse has no absolute-position API, so the
  Switch's real cursor may drift away from this virtual position. A true
  absolute cursor requires feedback or a calibrated target-side API.

Touch is ignored for PCBLE profiles without a Joy-Con 2. Inputs release on
pad disconnect. `/dev/uinput` and host-X11 mouse injection are not used.

## Design

* `server/wup_stream.c/.h`: supervisor, shared-memory ring, scaling, inputs
* `server/wup_touch.c/.h`: standalone gesture/position state machine
* `wiiu_wupstream/helper.c`: isolated `libwupstream` network/codec loop
* `wiiu_wupstream/shared.h`: fixed-layout process-to-process transport
* `scripts/build_wupstream.sh`: pinned optional dependency build

Transport uses Linux memfd (about 5 MB for three 854x480 BGRA slots and a
small PCM ring), never files under `/tmp`. A slow GamePad only drops frames
in this transport; it does not block Capture2Cloud's V4L2 capture loop.
