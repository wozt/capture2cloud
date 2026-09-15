# USB Ethernet on a Wii U — what is actually known

A running log. Everything here is either measured on the console, read
out of a file, or cited. Anything I have not checked says so.

> **Status: research, nothing built.** The goal is a working USB Ethernet
> adapter in **Wii U mode** (vWii would be a bonus). The reason is
> measured: the console's built-in Wi-Fi gives **9.3 Mbit/s** with a ping
> wandering between 9 and 94 ms, and the Wii U video chain is configured
> at 8 Mbit/s. The link is the constraint, not the encoder.

---

## The correction that changes the plan

The idea was to pick up someone's abandoned work. **That work does not
apply to Wii U mode.**

The existing AX88772B/C patcher patches **IOS80, IOS58 and IOS36** —
those are **vWii** IOSes, the Wii-compatibility side. It gives Ethernet
in *Wii mode*. Wii U titles, and a Wii U homebrew like ours, do not go
anywhere near them.

So for Wii U mode there is **no prior art to continue**. There is prior
art to *learn the shape of the problem from*, which is not the same
thing, and the difference is most of the work.

## The adapter in hand

| chipset | on this desk | Wii U |
|---|---|---|
| ASIX **AX88772** | no | ✅ what the official adapter (WUP-025) uses |
| ASIX **AX88179** | ✅ `0b95:1790` | ❌ not supported |
| Realtek **RTL8153** | ✅ `0bda:8153` | ❌ not supported |

Someone did try AX88179 by copying the init sequence out of the Linux
driver. The adapter's LED would not even light, and the attempt was
abandoned. That is the honest baseline: the last person to try this got
nothing at all out of the hardware.

## Where the Wii U's driver lives

Read off the console over FTP with `systemFilesAllowed`, read-only:

```
/storage_slc/sys/title/00050010/1000400a/code/fw.img   <- IOSU firmware, the ARM side
/storage_slc/sys/title/00050010/10004009/code/fw.img
/storage_slc/sys/title/00050010/100040ff/code/fw.img
```

The same directories hold the PPC-side libraries — `nsysnet.rpl`,
`nn_nets2.rpl`. Those are the *socket API* that a program calls; they are
not the driver. The USB Ethernet driver is an IOSU module, ARM, inside
`fw.img`.

vWii's own IOSes, for reference, are at
`/slccmpt01/title/00000001/{00000050,0000003a,00000024}` — IOS80, IOS58,
IOS36. Present on this console.

## What is set up here

- `reference/` — the Linux drivers for both chipsets, from kernel.org:
  `asix.h`, `asix_common.c`, `asix_devices.c` (AX88772 family) and
  `ax88179_178a.c` (AX88179). **GPL-2.0, reference only.** They are
  gitignored and must not be copied into this project — the same rule
  that kept Moonlight's `font.c` out of `wiiu_console/`.
- `ios/` — the pulled firmware, gitignored: it is Nintendo's, and it is
  console-specific.
- Ghidra 12.1.2 is at `~/logiciels/ghidra_12.1.2_PUBLIC`
  (`support/analyzeHeadless` for scripted runs).

## Reading the two Linux drivers is the first real task

Not to copy them — to measure the distance. `asix_devices.c` and
`ax88179_178a.c` are different drivers for a reason, and the size of
that reason is the size of this project. What matters:

- the register map, and whether it is the same space at all
- how the PHY is brought up
- the RX header format — the 772 patcher had to fix RX header alignment
  for the B/C, and getting that wrong "silently corrupts every inbound
  packet", which is exactly the failure that looks like nothing at all
- the URB/endpoint layout, since the 179 is a USB 3.0 part

## The firmware is open — done

`fw.img`, 14,668,288 bytes, pulled read-only off the console. An **ancast
image**: magic `EFA282D9`, device type **2 (Starbuck, the ARM side)`,
body `0xDFD000` bytes at offset `0x200`, which accounts for the file
exactly.

**The key is at OTP offset 0x090.** Not looked up — found, and the method
matters because the obvious one does not work. The image carries a SHA-1
at `0x1B0`, and it is tempting to brute-force the OTP against it. That
fails: checked, and that hash is of the body **as stored, encrypted**.
It cannot confirm a decryption.

What does confirm one is entropy. Every 16-byte window of the OTP was
tried as an AES-128-CBC key and the result measured:

    ciphertext                     7.9971 bits/byte
    otp 0x090                      6.0438      <- structure
    every other candidate          7.9963 .. 7.9979

One candidate, no ambiguity, and the plaintext it produces is firmware
padding. The IV does not matter: CBC resynchronises after one block, so
an unknown IV costs the first sixteen bytes and nothing else.

`tools/ancast.py` does this. The key value appears nowhere in this
repository, and neither the OTP nor any decrypted image is committed.

## Where the driver is

Confirmed inside the decrypted image, which names itself:

    usb_eth_asix.c                  the driver's own source filename
    __ax8817xReadCommand            register access
    __ax8817xWriteCommand
    USB Ethernet Network Interface
    BMCR = 0x%04x                   PHY registers, in its debug strings
    ANLPAR = 0x%04x
    BMSR = 0x%04x

and its whole function table beside them: `__handleUhsDevProbe`,
`__uhsIfProbeCallback`, `__UsbEthCtrlrConfigure`, `__UsbEthCtrlSendFrame`,
`__UsbEthCtrlReceiveFrame`, `__UsbEthCtrlHandleUrbCompletion`,
`__postLinkStatusUrb`, `__openControllerDriver`, `__removeDriver`.

The image is an ARM ELF at file offset `0x604`, 72 program headers --
each IOSU module is a segment with its own load address.

| what | segment | vaddr | file offset | size |
|---|---|---|---|---|
| the module's **code** | 29 | `0x12300000` | `0x2E2694` | `0x131844` |
| its strings/rodata | 30 | `0x12440000` | `0x413ED8` | `0x288E8` |
| `/dev/uhs` (USB host stack) | 18 | `0x10140000` | `0x0F51D0` | `0x4694` |

**For Ghidra**: load `ios/fw.dec` as raw ARM (big-endian off, 32-bit),
map file `0x2E2694` at `0x12300000` for the code and `0x413ED8` at
`0x12440000` for the data, then let it find the string references. The
function names above are in the data segment, so the labels come free.

## The thing worth thinking about before writing any ARM

`/dev/uhs` is the USB host stack, and **it is reachable from PPC
userspace** -- wut exposes it (`UhsClientOpen`, bulk and control
transfers). A homebrew can already drive an arbitrary USB device without
touching IOSU at all.

That opens a second route that nobody seems to have tried, and it avoids
patching a system component entirely: implement the AX88179 in the
homebrew, in userspace, over `/dev/uhs`. The cost is that the console's
own network stack would not know about it -- packets would have to be
carried by something of ours. For this project specifically that may be
acceptable, since what we want is one TCP stream from one host.

It is worth measuring before choosing: **does `/dev/uhs` even enumerate
the AX88179?** If the answer is no, both routes are dead and the adapter
is simply the wrong one. That is one small homebrew and an afternoon,
and it is the cheapest question in this whole document.

## The cheapest question, answered: YES

`uhsprobe/` asks `/dev/uhs` for every interface it has, with
`MATCH_ANY`. On this console, with the adapter plugged in:

    UhsClientOpen -> 0
    UhsQueryInterfaces -> 2 interface(s)
      [0] 0b95:1790  dev class ff vendor specific  if ff/ff/00  handle 131074
            in  ep 81  attr 03 (interrupt)  max 8      <- link status
            in  ep 82  attr 02 (bulk)       max 512    <- RX
            out ep 03  attr 02 (bulk)       max 512    <- TX
      [1] 1058:25a2  mass storage 08/06/50             <- the WD drive

**The host stack enumerates the AX88179 perfectly.** It is only the
*driver* that will not bind to it. And the endpoint layout is exactly
what the Linux driver expects of this part: interrupt IN for link
status, bulk IN, bulk OUT.

`max 512` says it negotiated USB 2.0 high speed. The 179 is a USB 3.0
part and this console has no USB 3.0 ports, so it is running in its
high-speed mode -- which is what the Linux driver handles anyway, and
which caps the link at well above the 9.3 Mbit/s we are trying to beat.

So a userspace driver over `/dev/uhs` is not hypothetical. The device is
there, addressable, with a handle.

What is still unproven is whether `UhsAcquireInterface` will hand it
over. Nothing else has claimed it -- no driver binds a vendor-specific
class it does not know -- so it should be free, but should is not
measured.

## And the console can TALK to it

`macread/` acquires the interface and performs one control transfer --
the one Linux's `ax88179_178a.c` uses to read the MAC address, taken
from that driver rather than guessed: `bmRequestType 0xC0`, `bRequest
0x01` (AX_ACCESS_MAC), `wValue 0x10` (AX_NODE_ID), `wIndex 6`, six bytes.

    UhsAcquireInterface -> 0 (granted)
    control transfer (read MAC) -> 6
    MAC: 00:0e:c6:b0:41:dc

`00:0e:c6` is ASIX's own OUI. The adapter answered with its real
hardware address.

So the entire userspace path is proven: **enumerate, acquire, control
transfer, read a register.** Nothing here is hypothetical any more.
Everything that follows is work rather than doubt -- and the doubt is
what usually kills a project like this.

One loose end, noted rather than chased: the filtered query reported
different endpoint addresses and a different interface handle than the
MATCH_ANY query did. The MAC came back correct, so the right device was
addressed, but the endpoint array is not being read the way this code
assumes. That has to be right before a single packet moves, because a
bulk transfer to the wrong endpoint fails silently.

## Could it be an Aroma plugin instead of an app?

Yes, and it is probably the right final shape. A WUPS plugin runs in the
background beside whatever title is loaded, which is exactly what a
driver wants to be. The plugin format is not the hard part.

The hard part is what a driver is *for*. Moving frames over the adapter
is now clearly possible; making the console's own TCP/IP stack send its
traffic through them is a separate problem, and a bigger one. Two shapes:

- **Transparent.** Aroma's FunctionPatcherModule can patch functions in
  `nsysnet`, so a plugin could intercept the socket layer and carry it
  over our own stack. Everything on the console would benefit. This is
  the ambitious version and it needs a TCP/IP implementation.
- **Just for us.** capture2cloud wants exactly one TCP stream from one
  host. A small stack that does ARP, IPv4 and one TCP connection is a
  weekend, not a career, and it would not care about the rest of the
  system.

The second is the one that gets a picture on the television. The first
is the one worth doing if this turns out to be interesting on its own.

## The chip works. The host stack will not carry data.

`driver/ax88179.c` is a driver written from the register documentation
in Linux's `ax88179_178a.c` -- numbers a chip answers to are facts about
the chip -- and on the console it gets this far:

    adapter up, MAC 00:0e:c6:b0:41:dc
    endpoints: bulk in 2, bulk out 3, interrupt 1
    link UP at 100 Mbit/s

**The bring-up sequence works.** PHY powered out of reset, the half
second it needs, clocks selected, receive control started. The PHY
negotiated 100 Mbit/s with the switch on its own. The chip is alive and
doing its job.

**Every data transfer is refused.** Not a timeout -- a refusal, in the
same `0xFFDEFF..` family as the two status codes wut names:

    UhsAdministerEndpoint(ENABLE)   -> -2162715  (0xFFDEFFE5)
    UhsSubmitBulkRequest    in ep2  -> -2162713  (0xFFDEFFE7)
    UhsSubmitInterruptRequest ep1   -> -2162713
    UhsSubmitControlRequest         ->        6  (works)

Ruled out, each by measurement rather than by reasoning:

- **the endpoint number** -- tried 2, 0x82, and every reading of the
  "mask" argument: 1<<2, 1<<(0x82&0xF), 0x82, 0x04|0x08. All the same.
- **the transfer size** -- 512, 2048, 16384. All the same.
- **the buffer** -- our own aligned static, and a pointer inside the
  work buffer UHS was given at open. All the same.
- **the transfer type** -- interrupt is refused exactly like bulk, so it
  is not something specific to bulk.
- **the wrong interface** -- the theory was that the filtered query
  returned a device-level handle, since control transfers go to endpoint
  0 and would work on one. Disproved: a MATCH_ANY listing gives the same
  handle with all three endpoints present, and it still refuses. (The
  handle value itself changes between runs -- 65537 one time, 131074
  another -- so handles are reassigned and are not a clue.)
- **timing** -- a long wait after acquiring changes nothing.

So the position is precise: **the console will let a homebrew configure
this adapter but not move packets through it.** Control transfers reach
the device; endpoint 0 is not the problem.

### Found: the interface is never actually handed over

`UhsAcquireInterface` takes a **completion callback**, which means its
return value says "accepted", not "done". Every probe passed NULL and
used the interface on the very next line.

Given a real callback and three seconds to arrive:

    acquire submitted -> 0
    acquire never called back after 3000 ms

**It never completes.** That explains every symptom at once and far
better than "bulk is refused": control transfers go to endpoint 0 and
never needed the interface, while everything else was asked for on an
interface the stack had not given us.

Also eliminated in the same run, and worth recording because it was a
real suspicion: the **controller number**. Moving the dongle from the
front port to the back changes nothing -- the adapter is on controller 0
either way, and controllers 1 and 2 will not open at all (-1 and -6).

### The flow was wrong, and now it is right -- and it still stops

WiiUBrew's `/dev/uhs` page settles what a dozen permutations could not.
Acquiring is only half a protocol. **ioctl 0x01, `UhsClassDrvReg`**,
registers a driver with a filter, and UHS then *calls you* when a
matching interface appears and hands you its profile. That is exactly
the shape of the IOSU driver in the decrypted firmware --
`__uhsIfProbeCallback`, `__handleUhsDevProbe` -- and every probe before
this queried and acquired instead, which is not the supported flow.

Registered properly:

    UhsClassDrvReg -> 1
    probe callback ARRIVED
    offered interface 196611
    acquire -> 0
    acquire callback never came
    enable endpoints -> -2162715
    bulk in ep2 -> -2162713

**The registration works and the probe arrives.** Note the handle: UHS
offers 196611, where a query returns 65537 for the same adapter. They
are different handle spaces, so the earlier attempts were not even
naming the same thing.

And on the handle UHS itself offered, through the flow the documentation
describes, the acquire is still accepted and still never completes.

### So the question is now much narrower

Not "why is bulk refused" but **"why does an acquire that was accepted
never complete"**. Candidates:

1. ~~The completion needs a thread or a queue serviced.~~ **Eliminated.**
   Tried four ways in one run -- sleeping, yielding, pumping ProcUI, and
   asking from a thread of its own. The callback never arrives in any of
   them.
2. ~~`UhsAdministerDevice` first.~~ **Eliminated.** It returns 0, so the
   device level is administrable; the interface level still is not.
3. ~~The controller number.~~ **Eliminated.** Front port or back, the
   adapter is on controller 0, and controllers 1 and 2 do not open.
4. ~~Querying instead of registering as a class driver.~~ **Fixed, and
   it was genuinely wrong** -- the probe callback now arrives with a
   handle from a different space. The acquire still does not complete.

What is left is the one that decides the project: **IOSU may simply not
hand an interface to a Cafe OS client for a device it has itself
probed**, and it says so by never answering rather than by refusing.

That is now a question to answer in the firmware rather than by
experiment, and the firmware is decrypted, mapped, and named: segment
29 at `0x12300000`, with `__handleUhsDevProbe` and `__uhsIfProbeCallback`
sitting in the strings beside it. The next session starts in Ghidra,
looking at what the stack does with an acquire request and under what
condition it declines to answer one.

## Next, in order

1. Try `UhsAdministerDevice` and a non-zero controller number -- two
   cheap experiments that would explain the refusal if either is it.
2. Read both Linux drivers and write the differences down here --
   register map, PHY bring-up, RX header format, endpoint layout.
3. Ghidra on segment 29 and compare its init sequence with Linux's
   `asix_devices.c`.
4. Only then choose: patch IOSU, or a userspace driver over `/dev/uhs`.

A rewrite inside a system component, with no debugger and "the LED does
not light" as the only feedback, is where the last attempt stopped. Step
1 exists so that we find out cheaply whether we would be walking into
the same wall.

## An honest alternative, kept in view

An AX88772-based adapter costs a few euros and works immediately. This
project is worth doing if it is interesting; it is not the cheap way to
get the console onto a wire.

## Sources

- [We can now use the AX88772B on the Wii U](https://gbatemp.net/threads/we-can-now-use-the-ax88772b-on-the-wii-u.670646/)
- [AX88772B/C/D USB Ethernet Patcher for vWii](https://gbatemp.net/threads/ax88772b-c-d-usb-ethernet-patcher-for-vwii.680252/)
- [Advice for Wii U - USB to Ethernet](https://gbatemp.net/threads/advice-for-wii-u-usb-to-ethernet-2021.602287/)
