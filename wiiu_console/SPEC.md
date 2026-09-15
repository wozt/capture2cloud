# Capture2Cloud on a Wii U console — specification

A fifth client: a homebrew running **on the Wii U itself**, drawing the
stream on the TV or on the GamePad's screen, with the GamePad's buttons
and sticks going back to the console being captured.

This is not the GamePad client in `../wiiu_gamepad/`. That one runs on
the PC and talks to a GamePad over a radio, with no Wii U involved. This
one runs on the console, uses its hardware decoder, and needs nothing
but a network.

> **Status: specification only.** Nothing is built. Everything below that
> is measured is marked; everything else is from research or from how the
> other clients work, and is a plan rather than a fact.

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

So the plan is **720p60 as the default and the tested path**, with 1080p
offered in the menu and honestly labelled as unlikely to hold. If it
does hold on a wired console, nothing stops it.

This is worth settling before any code: the whole shape of the client —
what the host encodes, what the decoder is configured for — follows from
the answer. See the questions at the end.

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

Once accepted, the host returns a session token. Store the **token**, not
the password, the way the page does — and if it is stored on the SD card
at all, that is a decision to take deliberately rather than by accident.

---

## What is already decided by the host

- The transport is `c2s_protocol.h` on port **5081**, the console port,
  shared with the Switch homebrew and the phone.
- **Shared settings are shared.** Resolution, frame rate and bitrate on
  that chain belong to every native client at once. A client that
  arrives and pushes its own saved values changes the picture for
  whoever is already watching, so it is *told* what the stream is and
  moves its own controls to match. `../SHARED_SETTINGS.md` has the rule
  and why.
- If this client should have its own encode instead — the way the
  GamePad client got port 5082 and `SS_STREAM_DRC` — that is a host
  change, and a question below.

---

## Order of work

1. An `.rpx` that connects, decodes, and draws on the TV. Nothing else.
2. The GamePad's input back to the host.
3. The menu, and the TV/GamePad switch.
4. The password through `nn::swkbd`.
5. Diagnostics, and whatever the first real session turns out to need.

---

## Questions

1. **1080p.** Given the reports, is 720p60 acceptable as the default and
   tested path, with 1080p offered but unsupported? Or should the client
   be built for 1080p first and the reports treated as somebody else's
   problem?

2. **Its own encode, or the shared one?** Joining port 5081 means the
   Switch and the phone share its resolution and bitrate. Its own chain —
   a fifth `SS_STREAM_*` and another port — costs host work but means the
   console never drags the others down, and vice versa. The GamePad
   client needed exactly that in the end.

3. **Wired or wireless?** If a USB Ethernet adapter is part of the setup,
   the bitrate can be far higher and 1080p becomes worth trying. If it is
   the console's own Wi-Fi, the ceiling is much lower and the defaults
   should say so.

4. **Where does the token live?** Nowhere, and typed each launch, is the
   simplest and safest. On the SD card is more convenient and is a
   credential sitting on a removable card.

5. **Does the GamePad menu need to work while the GamePad is showing the
   stream?** The corner marker says yes. Confirming it, because it means
   the menu is composited over live video rather than drawn instead of
   it — which is more work and was worth it on the other client.
