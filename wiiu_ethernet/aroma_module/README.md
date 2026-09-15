# AX88179 background module for Aroma

A WUMS module that runs from boot, applies the IOSU endpoint-ownership
patch itself, opens the AX88179 USB Ethernet adapter, brings up its own
threaded lwIP stack (DHCP, TCP, UDP, DNS), and intercepts the socket
exports of `nsysnet.rpl` so titles use the adapter transparently.

## Why a module, not an app

The endpoint patch is a volatile write to IOSU kernel memory: it is gone
at the next reboot. A background module reapplies it every boot, before
it touches the adapter, so nothing has to be launched by hand. That is
the whole reason this is a `.wms` and not the standalone `.rpx` in
`../wiiu_patch_iosu` (which still exists, for testing the patch in
isolation).

## Architecture

- `iosu_patch.c` — NOPs the endpoint-ownership `beq` in IOSU (via
  libmocha), every boot, before the adapter is opened.
- `main.c` — a worker thread (per title process) owns UHS and the
  adapter exclusively.
- `../net/ax_net.c` — lwIP glue. The lwIP `tcpip` thread owns the stack
  core; the worker only moves frames in (via `tcpip_input`) and out
  (via a TX slot queue, so only the worker ever talks to the driver).
- `../net/port/sys_arch.c` — lwIP `NO_SYS=0` port on coreinit
  primitives (threads, semaphores, mutexes, message-queue mailboxes).
- `nsysnet_shim.c` — replaces `nsysnet.rpl` exports (sockets, select,
  sockopts, DNS) through the Aroma FunctionPatcher module for games,
  the Wii U menu and homebrew (`ROOT_RPX`). It translates the nsysnet
  ABI (no `sa_len`, 16-bit family, different `MSG_*`/`SO_*`/`EAI_*`
  constants) to lwIP.

## What it does, and does not, do

- **Does:** patch IOSU, acquire the adapter, DHCP a lease, and serve a
  full socket API (`socket`…`select`, `setsockopt`, sync DNS:
  `gethostbyname`, `getaddrinfo`, `getnameinfo`) to titles over USB
  Ethernet.
- **Does not (falls back to the console's own network):**
  - TLS. `NSSLCreateConnection` wraps a *system* fd handled inside IOSU;
    an lwIP socket cannot be used there, so HTTPS of Nintendo titles
    stays on Wi-Fi.
  - `sendto_multi(_ex)`, `recvfrom_ex/_multi`, `getaddrinfo_async(_rs)`,
    `gethostbyaddr`, `netconf_*` — unpatched, they use the original
    nsysnet (which is why `socket_lib_init` is left real).
  - The system menu's own services, the eShop and the browser (they are
    separate target processes and rely on NSSL).

## Building

Needs WUMS, libmocha and libfunctionpatcher, vendored under `../vendor`
(see `../vendor/README.md`); with those present:

    make            # -> AX88179Module.wms

## Installing

Copy `AX88179Module.wms` to
`sd:/wiiu/environments/aroma/modules/` and reboot. It loads with the
environment; there is nothing to launch. The FunctionPatcher module
(`FunctionPatcherModule.wms`) must be present for title interception;
without it the adapter only serves the module itself.

Watch it with `udplogserver` on the PC -- every line is prefixed
`AX88179`. On a good boot you will see the patch applied, the shim
registrations, then `DHCP BOUND <ip>, ICMP ready`.
