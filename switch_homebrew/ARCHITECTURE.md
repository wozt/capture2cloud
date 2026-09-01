# capture2switch — a Switch 1 client for capture2cloud

Receives the picture and sound from the capture2cloud host, and sends the
Switch 1's buttons back so they drive the Switch 2 through the Titan One
adapter.

---

## The one decision that shapes everything: not WebRTC

The browser client talks WebRTC. Doing the same here is not realistic:
it needs ICE, DTLS-SRTP and SCTP data channels, none of which exist as a
devkitPro portlib. Porting `libdatachannel` and its dependencies would be
weeks of work before a single frame appeared on screen.

So the host gains a **second, native path** alongside WebRTC, for clients
that are not browsers:

    browser  ──HTTP /offer──▶ WebRTC (ICE, DTLS-SRTP, VP8+Opus)   unchanged
    Switch   ──TCP──────────▶ length-prefixed VP8 + Opus frames

Same encoders, same `tee` in the GStreamer pipeline — only the transport
differs. The browser path is untouched.

Why this is cheap on the host: the pipeline already fans out through
`tee`, so a native branch is another consumer of frames that are being
encoded anyway.

## Decoding: NVDEC, not the CPU

**An earlier version of this file said the Tegra X1's video engine was
unreachable from homebrew and that software decoding was the only
option. That was wrong**, and it cost real performance: the client could
not hold 30 fps at 360p while three CPU cores did work the hardware does
for free.

devkitPro's ffmpeg carries averne's `nvtegra` backend, the same one
Moonlight-Switch uses, and it ships with `vp8_nvtegra`, `h264_nvtegra`,
`hevc_nvtegra` and `vp9_nvtegra` already built. VP8 being among them is
what makes this free: the host stream did not have to change codec.

These are **hwaccels, not decoders in their own right**. There is no
`vp8_nvtegra` to look up by name; asking for one returns nothing and the
fallback then runs in software while looking like it succeeded. The
ordinary decoder is opened with `avcodec_find_decoder()`, an
`AV_HWDEVICE_TYPE_NVTEGRA` device is attached, and a `get_format`
callback returns `AV_PIX_FMT_NVTEGRA` -- that callback is what actually
engages the engine.

The host can also encode H.264 (`C2S_MSG_CODEC`, and the Codec entry in
the menu), which is the engine's native case. Two things are needed for
it to show a picture rather than a black screen: SPS/PPS repeated ahead
of every keyframe (`h264parse config-interval=-1` on the host), and a
keyframe forced the moment the codec changes -- otherwise the reopened
decoder starts on a picture predicted from frames it never saw. The same
applies to a client that has just connected, so the host forces one at
the end of every handshake.

The texture follows what the decoder produces rather than what the host
announced. Frames whose size did not match were dropped, and the two
disagree routinely: the announcement arrives before the encoder has
finished changing over.

Frames come back in `AV_PIX_FMT_NVTEGRA` and are moved to memory the CPU
can read with `av_hwframe_transfer_data`, arriving as NV12 which SDL
uploads directly. On unified memory that is a copy rather than a bus
transfer.

libvpx is kept as a fallback: a client that shows nothing is worse than
one that shows a slow picture, and there is no way to tell from here
whether every firmware exposes the engine. The menu says which decoder
actually opened, because the difference between them is the difference
between a smooth picture and a slideshow.

## Where the frames are encoded

The host encodes H.264 for this client on a GPU (`vah264enc` /
`varenderD129h264enc`), preferring the integrated one: not because it is
faster, but because the discrete card is driving the display and whatever
is being played on it, while the integrated one sits idle. Measured on
this machine, 720p60:

| encoder | host CPU |
| --- | --- |
| VP8, software | 85-89% |
| H.264, x264 on the CPU | 243% |
| H.264, integrated GPU | 47-48% |

That is why 720p60 to the console only worked when the browser stream was
turned down: x264 at one thread cost 6.4 ms of CPU per frame, and it was
competing with the MJPEG decode, the 1080p VP8 encode for the browser and
two colour conversions for the same cores.

It is also the answer to "how does Moonlight manage this". Moonlight on
the Switch is only a client; the encoding is done by Sunshine or GeForce
Experience on the host's GPU, and the stream it sends is H.264, which the
Tegra decodes on NVDEC. Both halves were hardware and both of ours were
not.

**VP8 stays on the CPU and always will**: AMD's video engine has never
supported VP8 encoding, on either of these GPUs. The browser stream is
VP8 because that is what WebRTC negotiates by default; moving it to the
GPU means offering H.264 over WebRTC instead, which is a separate change.

The GPU encoders take NV12 and x264 takes I420, so the scaler that feeds
the branch produces whichever the chosen encoder wants -- the conversion
from the capture format happens either way, so this costs nothing.

## Picture adjustments

The same four the browser page offers, and they cost very different
things -- so they are done in two different places.

**Brightness and contrast are free.** They are per-channel arithmetic
(`out = in * gain + offset`), which the renderer expresses directly: a
colour modulation, plus a second pass adding the picture to itself when
the gain is above one, plus a quad added or subtracted for the offset. At
most three passes over a full-screen quad, which is nothing on this
hardware.

**Saturation and hue are not per-channel** -- they mix colour channels
into each other, and no blend mode can express that. But the picture
arrives as YUV, where both are one 2x2 matrix on the two chroma bytes and
nothing else: a quarter of the frame's data, and the two settings
collapse into a single matrix so using both costs exactly what using one
costs. Measured:

| resolution | chroma pass | share of a core |
| --- | --- | --- |
| 720p60 | 2.5 ms | ~15% |
| 480p60 | 1.0 ms | ~6% |
| 360p30 | 0.6 ms | ~2% |

Worth having, not worth paying for when it is not wanted, so the pass is
skipped entirely while both sit at their defaults -- and the menu says
which of the four are free and which are not.

Doing brightness and contrast the same way would have meant a pass over
the luma plane too, twice the data, for something the renderer does for
nothing.

## Nothing queues up

Late frames have no value here, so they are thrown away rather than shown
late. Three things enforce that, and all three are needed:

- The client takes everything that has arrived in one pass each frame and
  decodes only the newest, dropping the rest. Decoding all of them
  collapses -- one frame behind means two to decode next time, then
  three.
- The host skips a frame for any client whose socket still holds unsent
  video (`SS_MAX_INFLIGHT_BYTES`). `send()` succeeding is not delivery:
  the kernel's buffer is megabytes, and a link that cannot keep up fills
  it with seconds of video the app believes it has sent.
- The client asks for a small receive buffer *before* connecting. Without
  it nothing ever backs up on a fast link -- the frames simply pour into
  this end and wait here instead, which looks like a perfect picture that
  is minutes behind.

Dropping a frame breaks the ones after it, since both codecs predict from
what came before, so a keyframe is requested whenever anything was
dropped and the frame kept was not itself one. A freshly built decoder
likewise ignores everything until the first keyframe.

## Why a smaller stream for this client

The handheld screen is 1280x720, so sending 1080p would be decoded and
then scaled back down. The client asks the host for what it can sustain
(`C2S_MSG_PROFILE`), and steps down on its own when it cannot keep up --
falling behind makes the host skip frames, and skipping a predictive
codec is far worse than receiving fewer.

## Buttons

Checked against libnx's `hidsys.h` rather than assumed:

| what | how |
| --- | --- |
| ordinary buttons, sticks | libnx `padUpdate` / `padGetStickPos`, on a dedicated 250 Hz thread |
| touch (menu gesture) | SDL2 touch events |
| **capture** (screenshot) | the API exists, but it did not work in practice — dropped |
| **HOME** | `hidsysAcquireHomeButtonEventHandle` — exists, but the system acts on it too |

The HOME event handle is real, but libnx marks both as "not intended for
general use; AM-sysmodule uses it internally", and the system suspends
the applet on HOME regardless of what we do with the event. So HOME is
attempted, not relied on.

Resulting mapping, with the fallbacks the user asked for:

## The menu, and the glass

The menu is two levels: a short list of categories -- connection, stream,
touch pad, controls, sound, console, system -- and a short list inside
each. It was one flat list of twenty-odd entries that had to be scrolled
through to reach anything, with the things reached most often somewhere
in the middle of it. Grouped the same way the browser page groups them,
so the two are learned once. Rows are shorter and the text smaller than
they were, because this is drawn in front of something you are watching.

**Log in as player is pinned at the top of the root list, in green.** It
is the one entry that has to be found without looking for it: everything
about the controller stops at it -- the sticks and the on-screen pad
alike, since both leave by the same door -- and buried three rows inside
a category it was simply forgotten, which reads as the controls being
broken. It goes plain once you are a player: the colour is there to be
noticed when it matters, not to stay loud afterwards.

The menu is drawn over the running stream and only veils it. What the
console is doing while the menu is open is exactly what most of these
settings are being changed for.

It is also usable by finger: tapping a row selects it and acts on it in
one go, since a touchscreen has no separate "confirm", and there is a
back button beside the title. The console is a tablet; reaching for the
glass is the obvious thing to do long before reaching for a stick.

The colour behind everything when there is no picture is a mid grey, not
black. On a console, black is what a screen that has gone to sleep looks
like, and there are several moments -- connecting, waiting for the first
frame, the console asleep -- where the only question worth answering is
whether this thing is still on.

## The on-screen pad

For playing with the console flat on a table and nothing plugged into it.
The same idea and the same layout as the browser page's touch overlay:
everything sized in one unit so the whole thing scales together, the
d-pad a single round zone rather than four buttons so that up and right
are two separate answers instead of one nearest button, and every control
its own touch target so several fingers do several things at once.

It does not replace the physical pad -- both are read and combined, a
button pressed if either says so and a stick taking whichever deflection
is larger -- so a Joy-Con in one hand and a thumb on the glass is a valid
way to play.

The overlay is lit from that combined state rather than from which
fingers are down, so it shows what is actually happening: press a
shoulder button on a Joy-Con and the one on screen lights up, move the
stick and the knob follows. The browser page's overlay does the same, and
it is the difference between an overlay and a picture of one.

**The screen is read, not waited on.** A finger used to be bound when a
press event arrived and released when a release event arrived, and a
release that never arrives leaves a button held for good -- which is what
"start stays pressed forever" was. A lifted finger can vanish from the
hardware without the event layer noticing, and no timeout makes "still
pressed" true again afterwards. The touchscreen's own state has no such
gap: a finger that is gone is simply not in the list, so anything bound
to an id that is no longer there is released on the next pass, four
milliseconds later. Only the menu is still driven by events, where a tap
is an instant and a missed release cannot leave anything stuck.

Reading the screen is not quite enough on its own, because **the
touchscreen reuses finger ids**. Two fingers down, one lifts, and the one
still on the glass can come back reported under the id the lifted one
had -- so a binding keyed on the id alone survives, still pressed,
attached to a finger that is somewhere else entirely. That is a button
that never releases while the stick is being used, and it is random
because it depends on which id the hardware happens to reuse. A bound
finger that appears to have jumped more than 150 pixels in one pass has
not moved: at 250 Hz a real finger moves a few pixels, so that is a
different finger, and the binding is dropped and made again.

A thumb that slides off a button also releases it, and is free to land on
whatever it slid onto in the same pass. The sticks and the d-pad are
exempt, since aiming them means travelling well outside their circle.

Reading and merging happen on the controller thread, not in the frame
loop, which is what gives touch input the same 250 Hz as the sticks
rather than the drawing's rate. The finger array is written by the frame loop and read
there, so it is behind a lock held for a handful of instructions.

Positions are kept normalised and saved with the rest of the settings, so
they survive a change of resolution and read sensibly in the config file.
**Move the buttons** drags each one where a thumb reaches, and there is a
reset back to the default layout.

## Sticks

Three numbers per stick, and the two outer ones are the half that was
missing.

A stick does not reach its electrical maximum, least of all on a
diagonal, so pushing it all the way sent 70 or 80 rather than 100 and the
remote console read a gentle push. **Range** says what counts as fully
pushed along an axis; **diagonals** says the same for the corners, where
a stick reaches least far and by an amount that differs from one stick to
the next. The two blend by how diagonal the direction is, so there is no
seam anywhere in the circle. **Deadzone** is the movement around the
centre that counts as none at all. Both outer limits go down to 45%.

The pair is judged on the LENGTH of the vector, not on each axis, because
a stick is round -- judged per axis, a corner reads as three quarters of
a push whatever the limits are. And the result is scaled so the LARGER of
the two components reaches 100, so a full corner comes out as 100/100
rather than 71/71: "as far as it goes" should mean the same thing in
every direction for something being handed to a game.

Per stick, because the two wear at different rates -- the left one takes
most of the movement in most games and goes first. Choosing either sends
you into the controller test, which is where the effect is visible; the
alternative is tuning them blind. The browser page has the same four
settings, and the same reasoning applies there.

Volume runs 0 to 100 and reaches four times the stream's own level, with
that level at 25 -- the browser page's scheme. It used to run 0 to 200
and mean it literally, so the top of the range was twice the source and
barely louder than it: a stream that arrives quiet stayed quiet. Stored
positions from the old scale are converted rather than misread, which is
what the config file's `version` is for.

Everything chosen in the menu -- profile, bitrate, codec, volume, mute --
-- and the stick limits, whether the diagnostics are shown, and the host
password -- is written back to `sdmc:/switch/capture2switch.cfg` as it is
changed and on the way out.

The password is stored in the clear, which is what a file on an SD card
is: anyone holding the console can read it. A password the host refuses
is discarded rather than retried forever, so a changed one can be typed
again.

**Connecting is as a viewer.** Becoming a player is a thing you press,
because connecting to a stream and taking control of someone else's
console are two different decisions and a remembered password should save
the typing rather than make the second one. That is surprising the first
time it happens -- the picture and the sound arrive, the controller does
nothing -- so the stream says so in as many words, over the picture, with
the gesture that opens the menu. **Log in on connect** in that menu turns
it into one decision for anyone who would rather it were.

Hiding the diagnostics stops the measuring as well, not just the drawing:
a panel nobody is looking at should not cost anything to fill in. The
menu takes the whole screen while it is hidden. There is no keyboard here and no shell; re-picking all of
it on every launch is a chore nobody should be asked to repeat.

The menu also carries **Wake the console** and **Reset the adapter**, the
two buttons the page has; both are players-only and refused server-side
for anyone else. There is no browser on this console, and needing one to
wake the console the client exists to show would be a poor joke.

| gesture | |
| --- | --- |
| L3+R3 | open the local menu (or tap the top of the screen) |
| Start+Select ~1 s | HOME, forwarded to the remote console |
| Start+Select ~5 s | quit |

HOME fires on release, so holding on toward quit does not send one on the
way past. The menu is a press rather than a hold: the way back out of the
stream should not itself need a knack.

Every player number is read, not just the first, and every style. It was
one player and the default pad, which covers the handheld console and
whatever happens to be player 1. Detach the Joy-Cons and they come back
over Bluetooth as a controller that may not be player 1 at all -- if
something else already held that slot, the reconnected pair lands on
player 2 and nothing ever saw it again. No unplugging is needed for that
to happen: the console reassigns players on its own as controllers come
and go. All of them are read and folded together, so it does not matter
which one is picked up.

**Menus act on counted presses, not on comparing one frame with the
next.** That comparison only ever sees what a button was doing at the two
instants the frame loop happened to look, so a press and release between
two frames is invisible -- and the frame loop slows down whenever there
is more to decode. The menu stopped answering exactly when the picture
got busy, which is what "the confirm button no longer works" was. The
sampling thread counts presses at 250 Hz and the frame loop drains the
count, so every press is acted on once whatever the drawing is doing.

The pad is read on its own thread at 250 Hz, not by the frame loop.
Sampling it per drawn picture tied the hand to the decoder -- at 30 fps
the stick moved 30 times a second, a short press could land between two
samples and be missed entirely, and one sampled twice read as a hold that
outlived it. The thread also does the sending, on change plus a 100 ms
heartbeat, so the socket is shared with the frame loop and every message
is written under a lock.

## Layout

    switch-homebrew/
      src/           the homebrew itself
      resources/     font, icon, anything packed into the .nro
      ARCHITECTURE.md

## Toolchain

devkitA64 16.1.0 + libnx, via devkitPro's apt repository. Portlibs used:
`switch-sdl2`, `switch-libvpx` (VP8), `switch-libopus` (audio),
`switch-sdl2_ttf` (menu text). ffmpeg is installed but only needed if the
stream ever moves to H.264.

    source /etc/profile.d/devkit-env.sh
    make            # produces capture2switch.nro
