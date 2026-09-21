# Capture2Cloud on a Wii U console — specification

A fifth client: a homebrew running **on the Wii U itself**, drawing the
stream on the TV or on the GamePad's screen, with the GamePad's buttons
and sticks going back to the console being captured.

This is not the GamePad client in `../wiiu_gamepad/`. That one runs on
the PC and talks to a GamePad over a radio, with no Wii U involved. This
one runs on the console, uses its hardware decoder, and needs nothing
but a network.

> **Status:** the client is running on real Wii U hardware. This document
> keeps the original design; current validated state is in `README.md` and
> `../WORKINPROGRESS.md`.
>
> The four questions that shaped it have been answered and the answers
> are folded in. They are kept at the end as decisions, with what each
> one costs.

---

## What it does

- **Video and sound** from the host, over the same binary protocol the
  Switch homebrew and the Android app already speak (`../c2s_protocol.h`).
- **The GamePad drives the console being captured**: buttons, both
  sticks, triggers, sent as the same twenty-one bytes every other client
  sends.
- **Two places to put the picture**, chosen from the menu:
  - **TV mode** — the stream on the television, the settings menu on the
    GamePad's screen, always visible and usable while playing.
  - **GamePad mode** — the stream on the GamePad, the menu hidden behind
    a small corner button.
- **A password**, entered with the console's own on-screen keyboard.

---

## The one requirement that has to change

**1080p60 is asked for and is not realistic.** Every report of Moonlight
on this console says the same thing: 720p60 runs well, and 1080p
produces blocky video or colour errors. The console's own Wi-Fi is
single-band 802.11n and is the first thing to give out; a USB Ethernet
adapter is what makes 720p60 comfortable.

**Decided: 720p60 is the default and the tested path.** 1080p is in the
menu, labelled as unlikely to hold, and nothing stops it on a console
that manages it. The client is built and measured at 720p60; 1080p is
offered rather than supported.

---

## How it is put together

| Piece | What it uses |
|---|---|
| Toolchain | devkitPro **wut**, building an `.rpx` |
| Video decode | the console's **hardware H.264** library — `H264DECOpen`, `H264DECSetBitstream`, `H264DECExecute` |
| Display | **GX2**, which has separate scan targets for the TV and the DRC, so the picture goes to either without re-rendering |
| Sound | Opus decoded with `ppc-libopus`, played through the console's audio |
| Input | **VPAD** for the GamePad |
| Password | **`nn::swkbd`**, the console's own keyboard, which has a password mode and draws itself with `DrawTV()` / `DrawDRC()` |
| Network | BSD sockets, as the Switch client uses |

Moonlight's Wii U port builds against `wiiu-sdl2`, `wiiu-curl`,
`wiiu-mbedtls`, `ppc-freetype`, `ppc-libopus`, `ppc-libexpat`. We need
far less: no curl, no TLS, no XML. SDL2 is worth considering for the
menu and the input, and worth avoiding for the video, which should go
through the hardware decoder into a GX2 texture rather than through a
software surface.

---

## The menu

The same idea as the GamePad client's: it lives on the pad's screen,
opens from a **small corner marker**, and everything in it is something
you only notice while playing.

In **TV mode** the menu is on the GamePad and can simply stay open — the
television has the game on it, so nothing is being covered. The corner
marker hides and shows it.

In **GamePad mode** the stream has the screen, the menu is hidden, and
the same marker brings it back over the picture.

### Settings it should carry

Taken from what the other four clients ended up needing, because those
were all learnt the hard way:

**Picture**
- Resolution: 720p / 1080p / 480p, with 720p the default
- Frame rate: 60 / 30
- Bitrate
- Where the picture goes: TV or GamePad

**Sticks and buttons** — every one of these exists because it was wrong
on another client first:
- Deadzone, per stick
- Range (where the stick counts as fully pushed), per stick
- **Invert stick Y**
- **Face buttons by position rather than by letter**
- Trigger threshold

**Connection**
- Host address and port
- The password, through the console's keyboard
- Reconnect behaviour

**Diagnostics**
- Frames a second, bitrate, frames dropped, keyframes asked for
- A line that says plainly whether input is being accepted or ignored

---

## Sticks and buttons: read this before writing the mapping

Both are wrong by default, and both were wrong on the last client too.

**The vertical axes are inverted.** `gamepad_bridge.h` wants up
positive. The handover document for the other client says libdrc already
reports it that way and not to negate it — and on the bench, pushing up
moved the character down until it was negated. Whatever VPAD reports,
**check it against the console before believing a document**, and make
it a setting so it can be flipped without a rebuild.

**The face buttons do not mean what they say.** This is a Nintendo pad
and the adapter pretends to be an Xbox 360, whose four letters sit in
different places: Nintendo's A is on the right where Xbox's B is, and
Nintendo's X is on top where Xbox's Y is. Map by **position**, not by
letter, or every on-screen prompt lands on the wrong button. Also a
setting.

---

## The password

`nn::swkbd` is the console's own keyboard, and `InputFormArg` has a
password mode, so nothing has to be typed with a stick.

It is not a dialog that blocks: it draws itself inside the render loop —
`Calc()` with the controller state, then `DrawTV()` and `DrawDRC()` —
so the client has to be built around a loop that can host it. Worth
knowing before the loop is written rather than after.

Once accepted, the host returns a session token. **The token is kept on
the SD card, and the password never is.**

That is a credential on removable media, and the thing that makes it
acceptable is not a mitigation anyone added: the host keeps its session
table in memory only, so every token dies when the host process does. A
token read off a card after a restart is already worthless, and the
keyboard is there to type the password again. Worth knowing so nobody
"improves" the host by persisting its sessions.

What must NOT be stored is the password itself -- the same rule the page
and the Android client follow.

---

## What is already decided by the host

- The transport is `c2s_protocol.h`.
- **It gets its own encode and its own port.** A fifth `SS_STREAM_*`
  beside VP8, H264, WEB and DRC, on port **5083**, fed only while a Wii U
  client holds it — the same shape the GamePad client ended up needing.
  Port 5081 is shared with the Switch and the phone, and there the
  resolution, frame rate and bitrate belong to everyone at once: a
  handheld asking for 480p30 took the GamePad down to 30 with it, which
  is the failure this avoids. `../SHARED_SETTINGS.md` has the rule.
- That costs host work -- a chain, a slot, a codec value on the wire, a
  demand gate -- and the work is already patterned four times over.

---

## Two rules this chain inherits, and one it needs built

**Nothing is encoded for nobody.** Every chain here is fed only while a
client is holding it: `switch_wanted[]` is set from the transport's own
count, and an appsrc that is not fed produces nothing. A Wii U client
that is not connected must cost exactly zero -- not a few percent, zero
-- for the same reason the other four do. Measured on the browser chain:
4% of a core idle against 18.5% with one client watching. This is not an
optimisation, it is the thing that makes five encodes affordable on one
machine.

So the fifth chain is gated the same way, and the demand callback is the
only place that decides it.

**Hardware where there is hardware.** The host already prefers the GPU:

    varenderD129h264enc   integrated GPU -- idle, cheapest in system time
    vah264enc             discrete GPU -- also fine, it has other work
    x264enc               no GPU encoder: back to the CPU

This machine has two render nodes, `renderD128` and `renderD129`, so
there are two video engines to spread across.

**Spreading them across the engines now happens; falling back when one
is busy still does not.** The host used to pick ONE encoder name at
startup and build every H.264 chain with it, so on this machine one
video engine did all the encoding while the other sat idle. Each chain
now gets its own name, round-robin over the engines that open, and the
fifth chain joins that array for free.

What is still not answered is "this engine is busy, use the other one".
Measured while adding the above: when a render node is unusable the VA
plugin does not register its element at all, so the factory is already
absent and a probe never sees it. Whether an engine will grant another
encode *session* is only answerable by asking for one, and the pipeline
is parsed in a single piece -- a chain that cannot start fails at start,
with nothing left to fall back to. Moving a chain to another engine at
run time is the real answer and it is not written.

---

## Order of work

1. An `.rpx` that connects, decodes, and draws on the TV. Nothing else.
2. The GamePad's input back to the host.
3. The menu, and the TV/GamePad switch.
4. The password through `nn::swkbd`.
5. Diagnostics, and whatever the first real session turns out to need.

---

## Decisions, and what each one costs

**720p60 is the path.** 1080p is offered and not supported. Cost: a menu
entry that can disappoint, which is better than a client built around a
resolution it cannot hold.

**Its own chain, port 5083.** Cost: host work, four times patterned.
Buys: the console and the Switch stop deciding each other's picture.

**The network is not known yet, so plan for the worse one.** Defaults are
set for the console's own single-band Wi-Fi -- roughly 20 to 30 Mbit in
practice -- and the menu goes up from there for whoever has a USB
Ethernet adapter. Getting this wrong in the generous direction is a
client that stutters out of the box; getting it wrong in the careful
direction is one setting to raise.

**The token lives on the SD card, the password never does.** See above
for why the host's in-memory session table is what makes that tolerable.

**The menu is composited over live video.** The corner marker in both
modes says so: in GamePad mode the stream has the screen and the menu is
drawn on top of it, not instead of it. That is more work than swapping
buffers and it is what the GamePad client needed too.

## Still open

- **Whether SDL2 is worth it.** It brings the menu, the input and the
  font for free, and it is what Moonlight's port uses. It is also a
  software surface, and the video must not go through one: the hardware
  decoder's output belongs in a GX2 texture. Possibly SDL2 for the menu
  and GX2 for the picture, possibly neither.
- **What the decoder actually does at 1080p.** The reports say colour
  errors, which sounds like a buffer layout rather than a hard limit.
  Worth ten minutes with the real decoder before believing a forum.
