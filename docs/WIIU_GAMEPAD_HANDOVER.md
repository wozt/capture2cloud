# Streaming to a real Wii U GamePad — what it took, so it takes less next time

Notes for whoever builds a GamePad client for Capture2Cloud. Everything
here was learned building one for
[bottom_screen_server](../bottom_screen_server), where it works: a game
reaches a real GamePad at 60 frames a second with zero keyframe requests
sustained. That client is `gamepad/bs_gamepad.cpp` in that repo — about
1100 lines — and it is worth reading before writing a new one.

What follows is not a tutorial. It is the list of things that were
wrong, and what each one cost, because none of them announce themselves:
a GamePad that is not working looks the same whichever of these is the
reason.

**Measured unless marked otherwise.** Where something is a guess it says
so.

---

## 1. What already exists, and what is actually new work

| | Where | Reusable here? |
|---|---|---|
| The radio stack: driver, AP, pairing | `~/rtw88_TSF`, and `gamepad/docs/WIIU_GAMEPAD.md` in bottom_screen_server | **Yes, as-is.** Nothing about it is specific to one project |
| `libdrc` — the protocol itself | vendored in `bottom_screen_server/gamepad/vendor/libdrc` | Yes, copy the build |
| `drc-x264` — x264 with DRH slicing | vendored beside it | Yes, copy the build |
| The bridge: connect, decode, scale, push, poll | `gamepad/bs_gamepad.cpp` | The *shape*, yes. The protocol half is bottom_screen's and becomes C2S here |

So the new work is genuinely small: swap one wire protocol for another
inside a program whose hard parts are already solved. Budget your time
for section 4 instead, which is where it actually goes.

### Confusing name

Capture2Cloud already has a `gamepad_bridge.c`, and it is something
else entirely — the ConsoleTuner USB adapter that drives the console.
Call the new one something that cannot be mistaken for it:
`wiiu_pad.c`, `drc_client.c`. Two files named for the same word, meaning
opposite directions of travel, is a bug waiting for a tired evening.

---

## 2. The stack has to be up before any of this matters

Driver loaded with **`disable_ips=1`**, hostapd running on 5 GHz channel
48 with a Nintendo OUI, MTU **1800**, PC at `192.168.1.10/24` (libdrc
hardcodes it), pad at `.11`. All of it, with the reasons, is in
`gamepad/docs/WIIU_GAMEPAD.md`. Read that first; it is the measured
account and none of it is guesswork.

Two that bite immediately:

- **`disable_ips=1`** or the MAC powers down when idle and every TSF
  read returns `0xEAEAEAEA`.
- **MTU 1800**, not 1500. Audio packets are 1572 bytes of IP. At 1500
  they fragment, and fragmentation was the last cause of the freeze.

---

## 3. The panel is not any shape you have

864 × 480 is what libdrc takes. The screen itself is **854 × 480**.
Capture2Cloud's source is 16:9 from a capture card — 1280 × 720 or
1920 × 1080 — so unlike the multi-console case there is only one shape
to handle, which makes this the easy version. Even so:

**Ask the server for the shape you will draw, not for the panel.**
The mistake that cost the most time was asking for 864 × 480 whatever
the source was. The *server* then does the stretching, and no amount of
letterboxing in the client can undo it — the circles are ovals by the
time the client sees them. Compute the largest rectangle of the source's
shape that fits the panel, ask for exactly that, pad the sides with
black.

For 16:9 into 864 × 480 that is 848 × 480 with 8 pixels of black each
side. Round requests to **whole macroblocks** (multiples of 16), so the
encoder is not cropping.

**Letterbox to the source's true shape, not to the size that arrives.**
Those differ, because the request was rounded: ask for three quarters of
a 4:3 box and 480 × 368 arrives rather than 480 × 360 — 1.30 where the
picture is 1.33. Fitting to what arrives carries that 2% into the
picture. Fit to the shape you know; the eight-pixel stretch is
invisible.

**Map touch through the letterbox.** The pad reports 0..1 across the
whole 864. If the picture occupies 848 at x=8, reading a tap across the
panel puts it off by the bars and worse towards the edges — and it reads
as a calibration fault when it is arithmetic. A tap in the black is not
on the picture; drop it.

---

## 4. The things that cost days

### The preset is `fast`, and libdrc's own note is wrong for games

`resync` is the number that decides everything: it counts the keyframe
requests the pad sends when it cannot decode. Zero means it is decoding
cleanly; anything near the frame rate means the picture is about to
freeze.

Measured against a running game, twenty seconds each:

| `DRC_PRESET` | resync/s | packets an image |
|---|---|---|
| `medium` | **60** | up to 12 |
| `fast` | 0 | 5 |
| `veryfast` | 0 | up to 8 |
| `ultrafast` | 0 | 5 |

libdrc's own note says medium or below, and that was measured on a flat
test pattern. A game is heavier. Set `fast` as the default —
`setenv("DRC_PRESET", "fast", 0)`, with the 0 so anything exported by
hand still wins.

The GamePad's DRH slicing wants **exactly 5 chunks per frame**. Watch
that number; it drifting upward is the first sign of trouble.

QP is pinned at 32 by the protocol and cannot be moved. That is why the
picture handed to libdrc should be as clean as the upstream encoder can
make it — there is a second lossy pass after yours that you do not
control.

### The deauth/reauth cycle, and the order that makes audio work

A GamePad that is already associated will not accept a new video
transport. It has to be deauthenticated and allowed to come back:

1. Start the streamer. **First.**
2. Deauthenticate the pad through hostapd's control socket
   (`hostapd_cli -p /var/run/hostapd -i <if> deauthenticate <mac>`).
3. Wait for it to reassociate and reauthorize.

**Starting the streamer after reauthorization leaves audio silent.**
That was measured twice on this bench and is not negotiable.

Detecting the return: watch **`connected_time` reset to 0** in
`hostapd_cli sta <mac>`, or the `[AUTHORIZED]` flag. Watching for the
station to *disappear* does not work — it reassociates in under half a
second and you will usually miss the gap. Typical reauthorization here:
**0.8 s**.

In bottom_screen this runs on its own thread once 3 frames have decoded,
so a decode fault is never blamed on the radio.

### Only one process may hold libdrc's ports

`50010`, `50022`, `50023`. A second bridge fails with "libdrc would not
start — is the AP up and the pad paired?", which is a misleading message
when the real answer is "another copy of you is running". Check the
ports before believing it:

```sh
ss -lunp | grep -E ':5001[0-9]|:5002[0-9]'
```

This matters more than it sounds because of the next one.

### A bridge that ignores SIGTERM is the worst failure mode

libdrc starts its own threads, and they inherit the signal mask of the
thread that starts them. A bridge that had been streaming for a while
**stopped answering SIGTERM entirely**: alive, holding the UDP ports,
pushing nothing. On the pad that looks like the picture simply stopping
and never coming back, with no replacement able to start. It needed
SIGKILL.

Install the handlers **after** `streamer.Start()`, and unblock the
signals in the main thread:

```c
sigset_t unblock;
sigemptyset(&unblock);
sigaddset(&unblock, SIGTERM);
sigaddset(&unblock, SIGINT);
pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
signal(SIGTERM, on_term);
signal(SIGINT,  on_term);
```

### Never block forever on the network read

Blocking indefinitely is right only while the single way to stop
receiving is the server going away, which closes the socket. It is wrong
for a stream that merely *stops*: a pump left asleep, a paused emulator,
a source detached. The socket stays open and empty and the picture stays
frozen with nothing anywhere saying why. That exact thing happened, and
the server-side cause took a `gdb` backtrace to find.

The fix, and the trap inside the fix: **a timeout that can fire in the
middle of a message desynchronises the stream by one header and every
message after it is garbage** — worse than the freeze. So arm the
receive timeout before a message begins and clear it once the first byte
has arrived. In bottom_screen that is `bs_conn_set_idle_timeout()` in
`bs_net.c`, with `tests/idle_timeout.c` holding it down: a sender that
dribbles a message out a byte at a time, three times slower than the
timeout, must still be read whole.

Then escalate rather than give up at once:

| silence | action |
|---|---|
| 2 s | ask for a keyframe — one packet, covers a decoder that lost its reference |
| 4 s | re-send what this client wants (size, codec, screen) — wakes a server that stopped sending |
| 30 s | drop the connection |

### Do not negate the sticks

libdrc already reports a stick the way a sane protocol wants it: **up is
positive**. This was got wrong three separate times, in three different
clients, by people reasoning from the screen's Y axis growing downward.
That is a rule about screens, not about sticks.

Capture2Cloud's 21 slots are −100..100, so the mapping is a scale, not a
flip.

### Pace the audio

Generating an unbounded chunk on every drain fills libdrc's queue with
old silence. Pace it at 48 000 samples/s and push bounded amounts. Mono
sources are upmixed by repeating the sample — *untested*, nothing here
sends mono.

### Stick drift is real and belongs in the client

The right stick on this pad drifts. A **12% central radial deadzone**
with smooth rescaling outside it (so full travel is retained) fixes it.
The left stick did not need one and defaults to 0 — applying 12% to a
stick nobody complained about is a silent change to how a game plays.
Optional centre calibration: average 1.2 s after a 0.3 s settling delay,
reject the sample set if the stick moved.

---

## 5. What is different for Capture2Cloud

| | bottom_screen_server | Capture2Cloud |
|---|---|---|
| Source | an emulator's framebuffer, 2 screens, 3 console shapes | one capture card, 16:9, one screen |
| Asking for a size | `BS_MSG_SET_SIZE`, per client | `C2S_MSG_PROFILE` — but it is **shared**, see below |
| Codec | always H.264 | several — the bridge must pin H.264 with `C2S_MSG_CODEC` |
| Input out | discrete button/axis events | `C2S_MSG_INPUT`, 21 slots of −100..100 |
| Touch | goes to the console's digitiser | there is none — see below |
| Keyframe | server-issued only | `C2S_MSG_KEYFRAME` exists; use it for the watchdog |

**Simpler in three ways.** One shape means no per-console arithmetic and
no top/bottom switching. One codec decision, made once at connect. And
`C2sHelloAck` already hands you `width`, `height`, `audio_rate` and
`audio_channels` — everything the decoder needs before the first frame.

**Harder in one you will not see coming.** In bottom_screen a client
asks for its own size and the server obliges. Here `C2sShared` says the
size, the frame rate, the bitrate and the codec are **shared between all
native clients**. A GamePad bridge that asks for 848 × 480 because that
is what its panel holds will drag the Switch client and every browser
down to 848 × 480 with it.

So decide, deliberately, before writing the request:

- **Ask, and accept that the pad sets the room's quality.** Right if the
  pad is usually the only thing watching.
- **Do not ask at all**, take whatever the stream already is, and scale
  it to the panel locally. One extra resample, nobody else affected.
  This is the safer default.
- **Ask only when nothing else is connected**, which needs a viewer
  count the protocol does not currently carry.

Whichever you pick, say it in a comment. Someone will otherwise "fix"
it in the direction you rejected.

Also: `C2sHelloAck.may_control` can come back 0, in which case the
stream plays and every button is silently dropped. Say that on the pad's
screen rather than letting it look like broken input.

**And one question with no clean answer.** The GamePad has a **touch
screen and nowhere obvious to send it**. A console being driven through a ConsoleTuner adapter has
no touch input. Three honest options, none of them implemented anywhere
yet:

1. Drop it, and use touch only for the client's own on-screen menu.
2. Map it to a stick — a touch position becomes a right-stick
   deflection, which is how a lot of console UIs are navigable.
3. Map it to whatever pointer the target console has, if it has one.

Pick one deliberately and say so in the code. A touch that silently does
nothing is a bug report every time somebody new tries it.

**The pad's own menu is worth having.** bottom_screen draws a full-screen
menu into the video it pushes, opened by touching an 8 × 8 pixel corner
marker. It carries the stick deadzones, the requested resolution, the
scaling filter and sharpness. It costs a Cairo dependency and about 250
lines, and it is the difference between settings you can change from the
sofa and settings you cannot.

---

## 6. The numbers to watch

Print them behind a flag, not by default — a line a second for as long
as it runs is a wall, and it buries the two lines that matter, the
freeze and the recovery.

| | good | bad |
|---|---|---|
| `resync` | 0 | anything near the frame rate: the picture is about to freeze |
| packets an image | 5 | climbing: the preset is too heavy |
| frames/s pushed | 60 | below: you are the bottleneck, not the radio |
| reauthorization | ~0.8 s | timing out: the AP or the pairing |

---

## 7. A concrete order of work

This builds the **two-pass** client: the host sends ordinary H.264, the
bridge decodes it and libdrc encodes it again. Build it anyway, even
though section 9 describes a better one — it is what the better one
falls back to when the vendored library is missing, and it is what you
measure the better one against. It is also the only version that can be
finished without touching libdrc.

1. Get the radio stack up and the pad paired, following
   `gamepad/docs/WIIU_GAMEPAD.md`. Nothing else can be tested until
   this works.
2. Copy `vendor/libdrc` and `vendor/x264` and their rpath-based build.
   Do not fight the system x264: it is a different build number and both
   can be loaded at once (see section 8).
3. Write the bridge with **`--no-pad`** from the first line: connect,
   decode, scale, and send nothing to a pad. It is how you tell a fault
   in your code from a fault in the radio, and it earned that on the
   first run here — the pad could not decode a game while the same
   bridge fed the built-in pattern perfectly, which moved the fault to
   the encoder settings in one step.
4. Then the pad: `Start`, the deauth cycle, `PushVidFrame`,
   `PushAudSamples`, `PollInput`.
5. Then input back into `C2S_MSG_INPUT`.
6. Then the watchdog and the signal handling, before you need them.

---

## 8. One thing that looks alarming and is fine

A machine with ffmpeg has a system x264, and libdrc needs the modified
one. Both end up loaded in the same process and the linker warns about
it. That is safe, and checked rather than assumed: x264 stamps its build
number into every exported symbol, so libdrc asks for
`x264_encoder_open_140` and only the vendored library defines it. The
system's `164` cannot answer.

---

## 9. The better plan: encode once, in the pad's own format

Everything above assumes the bridge decodes what the host sends and
libdrc encodes it again. That is **two lossy passes**, and the second one
is pinned at QP 32 by the protocol.

Capture2Cloud can skip the first. It already has the exact architecture
for it — and this is not a coincidence, it is the same idea it already
applies to browsers and the Switch:

```c
#define SS_STREAM_VP8  0
#define SS_STREAM_H264 1
#define SS_STREAM_WEB  2
```

> Three, not two codecs: the browsers' H.264 and the console's are the
> same codec at different sizes, so they cannot share one. The routing
> key had to stop being the codec.
>
> — `switch_stream.h`

A GamePad is one more of those: **`SS_STREAM_DRC 3`**, fed by
**drc-x264** instead of x264enc or a VA element. `switch_wanted[]`
already means "a chain with no audience is not fed, so it encodes
nothing and costs nothing", which is exactly the "only when a Wii U
client is connected" part — no new mechanism, just a fourth entry.

### What this fixes, beyond the picture

**It removes the shared-settings conflict from section 5 entirely.**
That was the awkward one: a GamePad asking for 848 × 480 would drag
every other native client down with it. This chain does not ask for
anything. Its size, its QP and its slicing are the protocol, not
preferences:

| | value | negotiable? |
|---|---|---|
| Size | 864 × 480, YUV420P | no — `kScreenWidth`/`kScreenHeight` in `drc/screen.h` |
| QP | 32 | no — pinned by the protocol |
| Chunks per frame | exactly 5 | no — `kH264ChunksPerFrame` |
| Slice header | absent | no — this is what drc-x264 exists for |
| Planar prediction | off | no |

So `C2sShared` never hears about it, and the browser and the Switch keep
whatever they had. That alone is worth the change.

### The one thing that blocks it

**libdrc has no way in for an already-encoded frame.** Both entry points
take pixels:

```c
void PushVidFrame(std::vector<byte>* frame, u16 w, u16 h, PixelFormat, ...);
void PushNativeVidFrame(std::vector<u8>* frame);  // YUV420P at 864x480
```

`PushNativeVidFrame`'s "native format" is the native *pixel* format, not
a bitstream. The video streamer owns an `H264Encoder` and calls
`Encode()` on it.

So step one is a change in [wozt/libdrc](https://github.com/wozt/libdrc):
an entry point that takes an `H264ChunkArray` plus the IDR flag and goes
straight to the vstrm packetiser, bypassing `H264Encoder`. It is a small
change — the packetiser is already separate — but nothing else can be
tested until it exists.

The settings the host's encoder must reproduce are not guesswork either:
`internal/h264-encoder.h` and its implementation **are** the
specification. Copy them exactly, including the NAL callback that
produces the chunks, and including `Restart()` — with intra refresh on,
x264 answers a forced IDR by restarting its refresh wave and never emits
`NAL_SLICE_IDR` again, which leaves a decoder that lost the sequence
frozen for good while audio keeps playing. A fresh encoder is the only
way back.

### What it costs

**The pad's menu needs pixels, and a pass-through stream has none.**
The menu is composited into the video being pushed. Two honest answers:

1. **Decode only while the menu is open.** Keep the decoder idle; when
   the menu opens, switch to decode → composite → re-encode, and switch
   back on close. Costs a keyframe each way, which nobody notices on a
   menu. This is the one to build.
2. Drop the menu, and configure from the host's GTK shell instead.

**Letterboxing moves to the host**, which is free: that chain is already
scaling, so it scales 16:9 into 864 × 480 with the black bars included.
Touch mapping then has to use the same rectangle — the host knows it, so
send it, rather than having the bridge guess.

**The library lives beside the system one.** drc-x264 is
`libx264.so.140`; anything linking libavcodec pulls `164`. Both load in
one process safely (section 8), but prefer **`dlopen` at the moment a
GamePad client connects**: the ordinary build then gains no new
dependency, and a machine with no vendored drc-x264 simply falls back to
the two-pass path instead of failing to build.

### Be honest about the gain

The second pass, at QP 32, is the dominant loss and it stays either way.
So the picture improves, but do not promise a transformation. The
measurable wins are elsewhere and they are real:

- no decode, no scale, no colour conversion, no re-encode in the bridge
  — that is nearly all of what the bridge currently costs;
- one less full frame of buffering in the path;
- and the shared-settings problem disappears.

Measure it: encode the same thirty seconds both ways and compare, before
anyone claims a number.

### Order of work

1. The libdrc entry point. Nothing else can start.
2. `SS_STREAM_DRC` as a fourth chain, drc-x264 behind `dlopen`, fed only
   while a GamePad client holds a slot.
3. A codec value on the wire — `C2S_CODEC_DRC_H264` — so a client can
   ask for it, and the host can refuse when the library is absent.
4. The bridge: hand the five chunks straight to libdrc.
5. The menu's decode-on-open mode.

Keep the two-pass path working the whole time. It is the fallback when
the library is missing, and it is what you compare against.

---

## 10. Still unknown

- **`PollInput`'s real update rate.** Polled at 200 Hz here because that
  is comfortably above 60. What it actually updates at was never
  measured.
- **Mono audio.** The upmix is written and has never run.
- **Whether a second radio is needed** for anything beyond capturing
  your own frames over the air. One is enough to run the AP and stream.
