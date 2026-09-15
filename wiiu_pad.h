#ifndef CAPTURE2CLOUD_WIIU_PAD_H
#define CAPTURE2CLOUD_WIIU_PAD_H

#include <stdint.h>

/*
 * The Wii U GamePad client, started and stopped from here.
 *
 * It is a separate program, not a thread. Three reasons, and the first
 * one decides it on its own:
 *
 *   - It needs libdrc and a fork of x264, both C++ and both vendored
 *     with an rpath. This host builds from one gcc line over a list of
 *     C files, and a feature almost nobody has the radio hardware for
 *     should not turn that into a C++ build with two vendored
 *     libraries.
 *
 *   - libdrc holds three UDP ports and has been seen to stop answering
 *     SIGTERM after a long session -- alive, holding the ports, pushing
 *     nothing. In its own process that is a SIGKILL and a restart. In
 *     this one it would be the capture card and the web server going
 *     down with it.
 *
 *   - It connects over the same native transport the Switch client
 *     uses, so it is a client like any other and needs no privileged
 *     access to anything in here.
 *
 * Note the name. `gamepad_bridge.h` in this directory is the opposite
 * direction of travel -- the USB adapter that drives the console -- and
 * confusing the two is a bad half-hour.
 */

typedef struct WiiuPad WiiuPad;

/*
 * Starts it against the native transport on `port`, which must already
 * be listening. `project_dir` is where this host was started from; the
 * client binary is looked for at `<project_dir>/wiiu_gamepad/wiiu_pad`.
 *
 * Never returns NULL for a missing binary or a failed exec: it returns
 * a handle whose status says what went wrong, because a toggle that
 * silently does nothing is worse than one that explains itself.
 */
/* No token: a pad arrives on its own port, over this machine's own
 * radio, and is a player by virtue of being there. */
WiiuPad *wiiu_pad_start(const char *project_dir, uint16_t port);

/*
 * Stops it and frees the handle. The handle is invalid afterwards.
 *
 * SIGTERM, five seconds, SIGKILL. The grace period is not politeness:
 * the client deauthenticates the pad to take the video transport, and
 * killing it mid-cycle leaves the pad associated to nothing until it
 * gives up on its own. It waits here rather than across later polls so
 * that the handle has one lifetime and the caller has nothing to
 * remember; a client that is answering takes about a tenth of a second.
 */
void wiiu_pad_stop(WiiuPad *pad);

/* Reaps the child if it has exited and takes in whatever it has said.
 * Call it from the loop that already runs; it never blocks. */
void wiiu_pad_poll(WiiuPad *pad);

int wiiu_pad_running(const WiiuPad *pad);

/* One line, for the window. Never NULL. */
const char *wiiu_pad_status(const WiiuPad *pad);

#endif
