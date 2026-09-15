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

## Next, in order

1. Read both Linux drivers and write down the differences here.
2. Pull `fw.img` and find out what it actually is — packed, encrypted,
   or plain. The OTP and SEEPROM dumps are on the SD card, so the keys
   exist if it needs them.
3. Locate the USB Ethernet module inside it. The stock driver knows
   product id `0x7720`; a search for that value is the cheapest way in.
4. Ghidra on the ARM module, and note what the init sequence looks like
   next to Linux's.
5. Only then decide whether this is a patch or a rewrite.

Step 5 is where this gets decided, and it is fair to say now that a
rewrite inside a system component, with no debugger and "the LED does
not light" as the only feedback, is where the last attempt stopped.

## An honest alternative, kept in view

An AX88772-based adapter costs a few euros and works immediately. This
project is worth doing if it is interesting; it is not the cheap way to
get the console onto a wire.

## Sources

- [We can now use the AX88772B on the Wii U](https://gbatemp.net/threads/we-can-now-use-the-ax88772b-on-the-wii-u.670646/)
- [AX88772B/C/D USB Ethernet Patcher for vWii](https://gbatemp.net/threads/ax88772b-c-d-usb-ethernet-patcher-for-vwii.680252/)
- [Advice for Wii U - USB to Ethernet](https://gbatemp.net/threads/advice-for-wii-u-usb-to-ethernet-2021.602287/)
