# AX88179 background module for Aroma

A WUMS module that runs from boot, applies the IOSU endpoint-ownership
patch itself, opens the AX88179 USB Ethernet adapter, and brings up its
own lwIP stack (DHCP + ICMP today).

## Why a module, not an app

The endpoint patch is a volatile write to IOSU kernel memory: it is gone
at the next reboot. A background module reapplies it every boot, before
it touches the adapter, so nothing has to be launched by hand. That is
the whole reason this is a `.wms` and not the standalone `.rpx` in
`../wiiu_patch_iosu` (which still exists, for testing the patch in
isolation).

## What it does, and does not, do yet

- **Does:** patch IOSU, acquire the adapter, DHCP a lease, answer ping.
  Its own IP, its own stack.
- **Does not:** carry the console's own TCP/IP traffic. Routing the
  system's sockets through this adapter (the "transparent" version) is
  the next milestone and is a separate, larger piece -- it means
  intercepting `nsysnet` with Aroma's function patcher.

## Building

Needs WUMS and libmocha, vendored under `../vendor` (see
`../vendor/README.md`); with those present:

    make            # -> AX88179Module.wms

## Installing

Copy `AX88179Module.wms` to
`sd:/wiiu/environments/aroma/modules/` and reboot. It loads with the
environment; there is nothing to launch.

Watch it with `udplogserver` on the PC -- every line is prefixed
`AX88179 module:`. On a good boot you will see the patch applied, then
`DHCP BOUND <ip>, ICMP ready`.

## Not yet run on hardware

Built and wired; the patch logic is idempotent and refuses an
unexpected instruction. It has not been loaded as a boot module and
watched through a real DHCP yet -- that needs the file installed and the
console rebooted.
