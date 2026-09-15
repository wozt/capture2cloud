# capture2cloud on a Wii U console

A homebrew that runs **on the Wii U itself**: it connects to the host
over the network, decodes the stream with the console's own hardware
decoder, and puts it on the television.

Not to be confused with [`../wiiu_gamepad/`](../wiiu_gamepad/), which
runs on the PC and talks to a GamePad over a radio with no Wii U
involved. This one needs nothing but a network.

[`SPEC.md`](SPEC.md) is the design and the reasoning. This file is how to
build it and what works so far.

## What works today

Step one of five: **it connects, it decodes, and it draws on the TV.**

That is all. No sound, no controller input, no menu, no password — those
are steps two to five in the spec, and each is easier to get right once
this one is known to work.

> **Not yet run on a console.** It builds; nothing below has been seen on
> real hardware. Every number in the spec that is measured says so, and
> none of them came from this client yet.

## Building

Needs devkitPPC and wut, which are not installed by default:

```sh
sudo dkp-pacman -S wiiu-dev
```

Then:

```sh
make
```

which produces `capture2wiiu.rpx`. Copy it to the SD card and launch it
from the Homebrew Launcher, or send it over the network with
`wiiload capture2wiiu.rpx`.

## Configuring it

One line, in [`src/main.c`](src/main.c):

```c
#define HOST_ADDRESS "192.168.1.10"
```

Reading it from the SD card comes with step four, along with the
password. Until then the address is compiled in, so there is exactly one
thing to edit.

## How the picture gets there

| Piece | What it uses |
|---|---|
| Video decode | the console's hardware H.264 — `H264DECOpen` / `SetBitstream` / `Execute`, in [`src/video.c`](src/video.c) |
| Display | **OSScreen, temporarily** — see below |
| Network | BSD sockets, ported from the Switch client |
| Transport | [`../c2s_protocol.h`](../c2s_protocol.h), on port **5083** |

### The display is temporary and deliberately so

OSScreen writes one 32-bit pixel at a time from the CPU. 720p60 is 55
million pixel writes a second, so this draws a reduced picture — one
pixel per 4×4 block — rather than pretending otherwise.

It is here because it needs no shaders, which means the network and the
decoder can be proven working before GX2 is written. **GX2 sampling the
decoder's NV12 output directly is what replaces it**, and the only file
that should have to change is `main.c`.

SDL2 is not used and that is not an oversight: its Wii U renderer is real
GX2, but it has no YUV shader, so an NV12 texture would go through SDL's
*software* conversion. On a 1.24 GHz PowerPC that is not a thing that
happens.

## Its own port, and why

Port 5083, beside 5081 (Switch, phone) and 5082 (GamePad).

`C2sShared` makes the size, the frame rate and the bitrate belong to
every client on a port **at once**. On 5081 a handheld asking for 480p30
took the GamePad down to 30 with it — measured, and the reason 5082
exists. A console on a television wants 720p60 and a phone on a train
wants neither, so they do not share a port.

The host serves it from its own encode chain, fed only while a Wii U
client is connected. A client that is not connected costs exactly zero,
which is the rule every chain here follows and the thing that makes five
encodes affordable on one machine.

## Before writing the input mapping

Two things are wrong by default and were wrong on the last client too.
They are written at the top of [`src/input.h`](src/input.h) so step two
cannot miss them: **the vertical stick axes are inverted**, and **the
face buttons must be mapped by position rather than by letter**. Check
both against the console rather than against any document, this one
included.

## Layout

```
Makefile        wut build, produces capture2wiiu.rpx
SPEC.md         the design, the decisions, and what each one costs
src/main.c      the loop: poll, decode, draw, status
src/net.c/.h    the host connection (ported from ../switch_homebrew)
src/video.c/.h  the hardware decoder
src/input.h     the controller layout, and two warnings for step two
```
