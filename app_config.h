#ifndef CAPTURE2CLOUD_APP_CONFIG_H
#define CAPTURE2CLOUD_APP_CONFIG_H

#include <stddef.h>

/*
 * Runtime configuration and path resolution.
 *
 * Two problems this solves, both of the "works on my machine" family:
 *
 * 1. Paths. Files used to be located either from `__FILE__` (the
 *    absolute path of the SOURCE at compile time -- wrong the moment the
 *    binary is built elsewhere or moved) or from a hardcoded
 *    "$HOME/scripts/capture2cloud/..." (wrong for anyone who checks the
 *    project out somewhere else). Everything now resolves relative to
 *    the running executable, so the whole directory can be moved,
 *    renamed or cloned anywhere and still work.
 *
 * 2. Hardware and limits. The capture device node, the adapter's USB
 *    ids, the client limit and the web port were compile-time
 *    constants, so using a different capture card or adapter meant
 *    editing C and rebuilding. They now live in the same git-ignored
 *    .env as the other settings.
 *
 * The .env is parsed once and kept in memory. It used to be re-read on
 * every lookup, which is fine for the handful of startup calls but not
 * for the password: that is consulted on every HTTP request and on every
 * native handshake, so a busy page meant opening, reading and parsing
 * the file continuously. The file's timestamp is still checked (at most
 * a couple of times a second), so editing it and having it apply without
 * a restart still works.
 */

/* Forgets the cached copy of the .env, so the next lookup re-reads it.
 *
 * Needed only by something that has just rewritten the file itself: the
 * cache notices ordinary edits by their timestamp, but two writes inside
 * the same filesystem clock tick with the same length are
 * indistinguishable, which is not something a person does and exactly
 * what a test does. */
void app_config_invalidate(void);

/* Asks the program to stop and start itself again, keeping its pid,
 * its arguments and its log.
 *
 * Sound has been seen to stop arriving with everything still claiming to
 * work, and restarting the whole program is the one thing that has
 * always brought it back. Rather than make that a trip to the machine,
 * it is a button -- and rather than a script that would have to kill and
 * relaunch from outside, the program replaces its own image once it has
 * shut everything down in the order it already knows to use.
 *
 * Set from an HTTP thread or the native transport's; acted on by the
 * main loop, which is the only thing allowed to tear the program down. */
void app_request_restart(void);
int  app_restart_requested(void);

/* Whether the periodic, high-volume messages are printed at all.
 *
 * Off by default. The per-second adapter statistics, the capture's frame
 * gaps and the full SDP of every negotiation are worth having while
 * something is being diagnosed and are pure noise the rest of the time
 * -- and in a long session they are a continuous stream of writes to
 * whatever the log is. Set VERBOSE=1 in the .env to get them back.
 * Errors, state changes and connections are never suppressed. */
int app_verbose(void);

/* Directory containing the running executable, with no trailing slash.
 * Resolved once via /proc/self/exe; falls back to "." if that fails. */
const char *app_dir(void);

/* Builds "<app_dir>/<relative>" into `out`. */
void app_path(char *out, size_t out_size, const char *relative);

/* Path of the config file itself (<app_dir>/scripts/.env) -- the same
 * file the shell scripts source, so there is one place to configure. */
void app_config_path(char *out, size_t out_size);

/* Reads a key. Returns 1 and fills `out` if present and non-empty,
 * otherwise returns 0 and leaves `out` as an empty string. Handles the
 * `KEY=value` lines, `#` comments, leading whitespace and optionally
 * quoted values that a shell-sourced .env can contain. */
int config_get(const char *key, char *out, size_t out_size);

/* Same, but returns `fallback` when the key is absent/empty. `out` is
 * scratch space the returned pointer may point into. */
const char *config_get_str(const char *key, char *out, size_t out_size, const char *fallback);

/* Integer variant. Missing/unparseable values give `fallback`; anything
 * present is clamped into [min_value, max_value] rather than rejected,
 * so a typo degrades to a sane limit instead of a broken run. */
long config_get_int(const char *key, long fallback, long min_value, long max_value);

#endif
