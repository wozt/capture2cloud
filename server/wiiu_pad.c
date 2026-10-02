#include "wiiu_pad.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long it may take to stop before it is made to. */
#define WIIU_PAD_GRACE_SECONDS 5

struct WiiuPad {
    pid_t pid;
    int   log_fd;          /* the child's stderr, non-blocking */
    char  status[256];     /* as long as a line can be, so nothing is cut */
    char  partial[256];    /* a line the child has not finished writing */
    size_t partial_len;
    time_t asked_to_stop;  /* 0 while it has not been asked */
    /* When to start it again after it exited on its own. 0 = do not. */
    time_t restart_after;

    /*
     * A manual restart is asynchronous: ask the current child to stop,
     * then spawn its replacement when poll() observes that it is gone.
     */
    int restart_on_stop;
    /* Kept so a session can be started again without the caller.
     * The client exits every time a pad goes away, which is normal. */
    char binary[1024];
    char port_text[16];
};

static void set_status(WiiuPad *pad, const char *text)
{
    snprintf(pad->status, sizeof(pad->status), "%s", text);
}

/*
 * Starts one session. The client waits for a pad, serves it, and exits
 * when it goes -- so this is called again each time, not once.
 */
static void spawn(WiiuPad *pad)
{
    int pipes[2];
    if (pipe(pipes) != 0) {
        set_status(pad, "could not make a pipe for its output");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        set_status(pad, "could not start it");
        return;
    }
    if (pid == 0) {
        /*
         * Its own process group, so a Ctrl-C in the terminal that
         * started this host does not also hit the client -- it has a
         * shutdown to do, and being killed in the middle of it leaves
         * the pad associated to a transport that has gone.
         */
        setsid();
        dup2(pipes[1], STDERR_FILENO);
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execl(pad->binary, pad->binary, "--port", pad->port_text, (char *)NULL);
        _exit(127);
    }

    close(pipes[1]);
    if (pad->log_fd >= 0) {
        close(pad->log_fd);
    }
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    pad->pid = pid;
    pad->log_fd = pipes[0];
}

WiiuPad *wiiu_pad_start(const char *project_dir, uint16_t port)
{
    WiiuPad *pad = calloc(1, sizeof(*pad));
    if (!pad)
        return NULL;
    pad->log_fd = -1;

    char binary[1024];
    snprintf(binary, sizeof(binary), "%s/wiiu_gamepad/wiiu_pad", project_dir);
    if (access(binary, X_OK) != 0) {
        /*
         * Said plainly rather than left as a toggle that does nothing.
         * This build is the common case: the client is not part of the
         * ordinary build because almost nobody has the radio adapter.
         */
        snprintf(pad->status, sizeof(pad->status),
                 "not built: run make in %s/wiiu", project_dir);
        return pad;
    }

    snprintf(pad->binary, sizeof(pad->binary), "%s", binary);
    snprintf(pad->port_text, sizeof(pad->port_text), "%u", (unsigned)port);
    spawn(pad);
    if (pad->pid > 0) {
        set_status(pad, "starting…");
    }
    return pad;
}

/*
 * Whatever it has said since last time, one line at a time.
 *
 * Only the last line is kept, because that is what a status label can
 * show, and the client is written so that its last line is the one worth
 * reading: it says a thing once when the thing changes, rather than a
 * line a second for as long as it runs.
 */
static void drain(WiiuPad *pad)
{
    if (pad->log_fd < 0)
        return;
    char buf[512];
    ssize_t n;
    while ((n = read(pad->log_fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (pad->partial_len > 0) {
                    pad->partial[pad->partial_len] = '\0';
                    /*
                     * A "status:" line is the client describing itself
                     * once a second; anything else is an event. Both go
                     * to the terminal, but only the first is allowed to
                     * become the label, so a one-off message -- "the
                     * stream is now 1280x720" -- does not sit there as
                     * the state of things for the next ten minutes.
                     */
                    if (strncmp(pad->partial, "status: ", 8) == 0) {
                        set_status(pad, pad->partial + 8);
                        /* A line a second is a wall in a terminal, so it
                         * goes to the label and stays there -- unless
                         * somebody is actually watching, which is what
                         * VERBOSE means everywhere else here. */
                        if (getenv("VERBOSE")) {
                            fprintf(stderr, "wiiu_pad: %s\n", pad->partial);
                        }
                    } else {
                        set_status(pad, pad->partial);
                        fprintf(stderr, "wiiu_pad: %s\n", pad->partial);
                    }
                }
                pad->partial_len = 0;
            } else if (pad->partial_len + 1 < sizeof(pad->partial)) {
                pad->partial[pad->partial_len++] = buf[i];
            }
        }
    }
    if (n == 0) {       /* it closed its end */
        close(pad->log_fd);
        pad->log_fd = -1;
    }
}

void wiiu_pad_request_start(WiiuPad *pad)
{
    if (!pad)
        return;

    /* A handle returned for a missing binary cannot become runnable
     * without a server restart/build; keep its useful error status. */
    if (!pad->binary[0])
        return;

    pad->restart_after = 0;

    if (pad->pid > 0) {
        /*
         * It may still be completing a previous asynchronous stop. In
         * that case "start" means start again as soon as it is gone.
         */
        if (pad->asked_to_stop) {
            pad->restart_on_stop = 1;
            set_status(pad, "stopping; will start again");
        }
        return;
    }

    pad->asked_to_stop = 0;
    pad->restart_on_stop = 0;
    spawn(pad);

    if (pad->pid > 0)
        set_status(pad, "starting…");
}

void wiiu_pad_request_stop(WiiuPad *pad)
{
    if (!pad)
        return;

    pad->restart_after = 0;
    pad->restart_on_stop = 0;

    if (pad->pid <= 0) {
        set_status(pad, "stopped");
        return;
    }

    if (!pad->asked_to_stop) {
        kill(pad->pid, SIGTERM);
        pad->asked_to_stop = time(NULL);
    }

    set_status(pad, "stopping…");
}

void wiiu_pad_request_restart(WiiuPad *pad)
{
    if (!pad)
        return;

    if (!pad->binary[0])
        return;

    pad->restart_after = 0;

    if (pad->pid <= 0) {
        wiiu_pad_request_start(pad);
        return;
    }

    pad->restart_on_stop = 1;

    if (!pad->asked_to_stop) {
        kill(pad->pid, SIGTERM);
        pad->asked_to_stop = time(NULL);
    }

    set_status(pad, "restarting…");
}

void wiiu_pad_poll(WiiuPad *pad)
{
    if (!pad)
        return;
    drain(pad);

    /* Back to waiting for a pad. The binary and the port are the ones
     * it was started with; nothing about the session survives it. */
    if (pad->pid == 0 && pad->restart_after && time(NULL) >= pad->restart_after) {
        pad->restart_after = 0;
        spawn(pad);
    }

    if (pad->pid > 0) {
        int status = 0;
        const pid_t done = waitpid(pad->pid, &status, WNOHANG);
        if (done == pad->pid) {
            pad->pid = 0;
            drain(pad);     /* its last words arrive after it has gone */
            if (pad->asked_to_stop) {
                const int restart = pad->restart_on_stop;

                pad->asked_to_stop = 0;
                pad->restart_on_stop = 0;

                if (restart) {
                    spawn(pad);
                    if (pad->pid > 0) {
                        set_status(pad, "starting…");
                    }
                } else {
                    set_status(pad, "stopped");
                }
            } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                set_status(pad, "could not be run");
            } else {
                /*
                 * It stopped by itself, which is now the ordinary way a
                 * session ends rather than a fault: the client waits for
                 * a pad, serves it, and exits when the pad goes. So it
                 * is started again to go back to waiting.
                 *
                 * After a second, not immediately. A client that fails
                 * at once -- no access point, the ports held by
                 * something else -- would otherwise be relaunched as
                 * fast as the machine can fork, and the reason would
                 * scroll past too quickly to read.
                 */
                /* How it ended, because "it stopped" is not a
                 * diagnosis: a clean exit at the end of a session and
                 * a signal in the middle of one look identical from
                 * here, and only one of them is normal. */
                if (WIFSIGNALED(status)) {
                    char text[96];
                    snprintf(text, sizeof(text), "killed by signal %d; starting again",
                             WTERMSIG(status));
                    set_status(pad, text);
                    fprintf(stderr, "wiiu_pad: killed by signal %d\n", WTERMSIG(status));
                } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                    char text[96];
                    snprintf(text, sizeof(text), "exited with %d; starting again",
                             WEXITSTATUS(status));
                    set_status(pad, text);
                    fprintf(stderr, "wiiu_pad: exited with %d\n", WEXITSTATUS(status));
                } else {
                    set_status(pad, "waiting for a pad");
                }
                pad->restart_after = time(NULL) + 1;
            }
        } else if (pad->asked_to_stop &&
                   time(NULL) - pad->asked_to_stop >= WIIU_PAD_GRACE_SECONDS) {
            /*
             * It has had its five seconds. A client stuck here is one
             * that stopped answering SIGTERM -- which has happened, and
             * which matters more than usual because it is still holding
             * libdrc's three UDP ports and nothing else can take the pad
             * until it lets go.
             */
            kill(pad->pid, SIGKILL);
            pad->asked_to_stop = time(NULL);    /* do not spin on it */
            set_status(pad, "did not stop; closing it by force");
        }
    }
}

void wiiu_pad_stop(WiiuPad *pad)
{
    if (!pad)
        return;

    if (pad->pid > 0) {
        wiiu_pad_request_stop(pad);

        /*
         * Waited for here rather than across later polls, so that the
         * handle has exactly one lifetime and the caller has nothing to
         * remember. It costs a pause in the interface only when
         * something is already wrong -- a client that is answering
         * takes about a tenth of a second.
         */
        for (int i = 0; i < WIIU_PAD_GRACE_SECONDS * 20; i++) {
            int status = 0;
            if (waitpid(pad->pid, &status, WNOHANG) == pad->pid) {
                pad->pid = 0;
                break;
            }
            drain(pad);
            const struct timespec pause = { 0, 50 * 1000 * 1000 };
            nanosleep(&pause, NULL);
        }
    }

    if (pad->pid > 0) {
        /*
         * Out of patience. This is not hypothetical: libdrc's threads
         * have been seen to swallow SIGTERM after a long session, and
         * the process then sits there holding the three UDP ports with
         * nothing able to take the pad until it lets go.
         */
        fprintf(stderr, "wiiu_pad: did not stop in %d s; closing it by force\n",
                WIIU_PAD_GRACE_SECONDS);
        kill(pad->pid, SIGKILL);
        waitpid(pad->pid, NULL, 0);
        pad->pid = 0;
    }

    drain(pad);
    if (pad->log_fd >= 0)
        close(pad->log_fd);
    free(pad);
}

int wiiu_pad_running(const WiiuPad *pad)
{
    return pad && pad->pid > 0;
}

const char *wiiu_pad_status(const WiiuPad *pad)
{
    return pad && pad->status[0] ? pad->status : "off";
}
