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

Validated on Wii U hardware: H.264 video through H264DEC/GX2, PCM audio
through AX, GamePad input, password login, saved session token and normal
HOME-menu suspend/resume/exit.

The touch menu is implemented in `src/menu.c/.h`: layout, hit testing and
layered drawing live outside the application loop. Connection and Console
are the first two pages; Stream and Controls are the next UI iterations.

The first settings frame and first two in-stream menu openings are saved as
`menu-debug-0.bmp` through `menu-debug-2.bmp` beside the WUHB for hardware
rendering checks.

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

For the normal development loop, `tools/dev.sh` discovers the Wii U from
its FTP service, builds the client, starts `udplogserver`, then launches
the RPX through wiiload:

```sh
./tools/dev.sh
```

The persistent WUHB lives at
`sd:/wiiu/apps/capture2cloud/capture2wiiu.wuhb`. Client-only updates do
not require a console reboot.

## Configuring it

Host, native port, web/login port and the temporary session token are
stored in `sd:/wiiu/apps/capture2cloud/capture2cloud.cfg`. The password
is entered with the Wii U keyboard and is never stored.

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
