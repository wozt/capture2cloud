#include <wums.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include "../net/ax_net.h"
#include "iosu_patch.h"

WUMS_MODULE_EXPORT_NAME("homebrew_ax88179");
WUMS_MODULE_AUTHOR("wozt");
WUMS_MODULE_VERSION("0.1.0");
WUMS_MODULE_DESCRIPTION("AX88179 usermode Ethernet, DHCP and ICMP");

static OSThread worker __attribute__((aligned(0x40)));
static uint8_t stack[64 * 1024] __attribute__((aligned(0x40)));
static atomic_bool stopping;
static int started;

/* The worker exclusively owns UHS and lwIP. App transition hooks only signal
 * stop, then join before per-title resources/newlib are finalized. */
static int run_network(int argc, const char **argv)
{
    (void)argc; (void)argv;
    WHBLogUdpInit();
    WHBLogPrintf("AX88179 module: worker started");

    /*
     * The IOSU patch, applied here and not by a separate app.
     *
     * Without it the endpoint enable is refused and nothing moves. The
     * write is volatile, so this reapplies it every boot before the
     * adapter is touched -- which is the whole difference between a
     * module that runs in the background and an app someone has to
     * remember to launch. Applying it is idempotent and it refuses an
     * unexpected instruction, so a re-run or a firmware it does not
     * recognise cannot make things worse.
     */
    {
        char pwhy[160];
        if (iosu_patch_apply(pwhy, sizeof(pwhy)) != 0) {
            WHBLogPrintf("AX88179 module: IOSU patch NOT applied (%s) -- "
                         "endpoints will stay locked", pwhy);
        } else {
            WHBLogPrintf("AX88179 module: IOSU patch %s", pwhy);
        }
    }

    while (!atomic_load_explicit(&stopping, memory_order_acquire)) {
        char why[160];
        Ax88179 *ax = ax88179_open(why, sizeof(why));
        if (!ax) {
            WHBLogPrintf("AX88179 module: %s", why);
        } else if (ax_net_start(ax) != 0) {
            WHBLogPrintf("AX88179 module: network initialization failed");
            ax88179_close(ax);
        } else {
            char previous_ip[16] = "";
            while (!atomic_load_explicit(&stopping, memory_order_acquire)) {
                int n = ax_net_poll();
                if (n == -2) {
                    WHBLogPrintf("AX88179 module: control path lost, reopening adapter");
                    break;
                }
                const char *ip = ax_net_address();
                if (ip && strcmp(ip, previous_ip)) {
                    snprintf(previous_ip, sizeof(previous_ip), "%s", ip);
                    WHBLogPrintf("AX88179 module: DHCP BOUND %s, ICMP ready", ip);
                } else if (!ip && previous_ip[0]) {
                    previous_ip[0] = 0;
                    WHBLogPrintf("AX88179 module: link/lease unavailable");
                }
                /* Prevent a tight spin if UHS returns an immediate error. */
                if (n <= 0) OSSleepTicks(OSMillisecondsToTicks(1));
            }
            ax_net_stop();
            ax88179_close(ax);
        }
        for (int i = 0; i < 30 && !atomic_load_explicit(&stopping, memory_order_acquire); i++)
            OSSleepTicks(OSMillisecondsToTicks(100));
    }
    WHBLogPrintf("AX88179 module: worker stopped, interface released");
    WHBLogUdpDeinit();
    return 0;
}

static void stop_worker(void)
{
    if (!started) return;
    atomic_store_explicit(&stopping, true, memory_order_release);
    OSJoinThread(&worker, NULL);
    started = 0;
}

WUMS_INITIALIZE(args)
{
    (void)args;
    /* Device handles belong to a title; create them in APPLICATION_STARTS. */
}

WUMS_APPLICATION_STARTS()
{
    if (started) return;
    atomic_store_explicit(&stopping, false, memory_order_release);
    if (OSCreateThread(&worker, run_network, 0, NULL, stack + sizeof(stack),
                       sizeof(stack), 16, OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        started = 1;
        OSSetThreadName(&worker, "AX88179 network");
        OSResumeThread(&worker);
    }
}

WUMS_APPLICATION_REQUESTS_EXIT() { stop_worker(); }
WUMS_APPLICATION_ENDS() { stop_worker(); }
WUMS_DEINITIALIZE() { stop_worker(); }
