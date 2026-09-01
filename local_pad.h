#ifndef CAPTURE2CLOUD_LOCAL_PAD_H
#define CAPTURE2CLOUD_LOCAL_PAD_H

#include "app_settings.h"

/*
 * A controller plugged into THIS machine, driving the console.
 *
 * Until now the only ways in were a browser and the Switch client, which
 * is odd when the console is on the desk next to the keyboard: the
 * adapter is right there and there was no way to reach it without going
 * through a web page.
 *
 * It is one more source into the same merge the browser and the console
 * client use, so a pad here and a pad elsewhere combine rather than
 * fight -- a button is pressed if either says so.
 *
 * Off in headless mode and never started there: nobody is sitting at a
 * machine that has no screen, and a controller left plugged into it
 * would be sending whatever it was resting on.
 */

int  local_pad_init(void);
void local_pad_shutdown(void);

/* Reads the controller and hands the result to the bridge. Call once per
 * frame; does nothing when disabled or when no controller is chosen. */
void local_pad_poll(const AppSettings *settings);

/* Names of the controllers SDL can see, for the settings window's list.
 * Returns how many were written; the strings stay valid until the next
 * call. */
int  local_pad_list(const char **names, int max_names);

#endif
