/*
 * Why does the host stack refuse a bulk transfer?
 *
 * Everything else works: the chip initialises, the PHY negotiates, the
 * link comes up at 100 Mbit/s, control transfers are answered. Only the
 * bulk IN is refused, with 0xFFDEFFE7 -- the same 0xFFDEFF.. family as
 * the two UHS status codes wut does name, so a refusal rather than a
 * timeout.
 *
 * Each round trip to the console costs a minute, so rather than guess
 * once per trip this tries every plausible cause in one run and reports
 * each: whether enabling the endpoint even succeeded, whether the
 * endpoint wants its direction bit, whether the request size is the
 * problem, and whether a smaller buffer is accepted.
 */
#include <stdio.h>
#include <string.h>

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

#include "probe.h"

#define ASIX_VID    0x0b95
#define AX88179_PID 0x1790
#define UHS_DIR_OUT 0
#define UHS_DIR_IN  1

static UhsHandle g_handle;
static uint8_t g_work[128 * 1024] __attribute__((aligned(0x40)));
static uint8_t g_buf[16 * 1024] __attribute__((aligned(0x40)));
static UhsInterfaceProfile g_profiles[16];

/*
 * Acquiring an interface is ASYNCHRONOUS.
 *
 * UhsAcquireInterface takes a completion callback, which means its
 * return value says "accepted", not "done" -- and every probe so far
 * passed NULL and used the interface on the very next line. That would
 * explain everything seen: control transfers go to endpoint 0 and never
 * needed the interface, while every data transfer was asked for before
 * the stack had finished handing it over.
 */
static volatile int g_acquired;
static volatile int32_t g_acquire_result;

static void on_acquired(void *context, int32_t arg1, int32_t arg2)
{
    (void)context;
    (void)arg2;
    g_acquire_result = arg1;
    g_acquired = 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("AX88179: why is bulk refused?") != 0) {
        probe_shutdown();
        return 1;
    }

    /*
     * Every controller, not just zero.
     *
     * This console has more than one USB controller and the front and
     * back ports need not be on the same one. Every probe so far opened
     * controller 0 without asking -- so "the adapter is refused" may all
     * along have been "the adapter is on a controller we never opened",
     * which would explain a refusal that was never a timeout.
     *
     * Moving the dongle from the front to the back is what raised the
     * question. This answers it for every controller at once.
     */
    for (int controller = 0; controller < 3; controller++) {
        UhsConfig config;
        memset(&config, 0, sizeof(config));
        config.controller_num = controller;
        config.buffer = g_work;
        config.buffer_size = sizeof(g_work);

        int32_t r = UhsClientOpen(&g_handle, &config);
        if (r < 0) {
            probe_say("controller %d: will not open (%d)", controller, (int)r);
            continue;
        }

        UhsInterfaceFilter filter;
        memset(&filter, 0, sizeof(filter));
        filter.match_params = MATCH_ANY;
        const int32_t found = UhsQueryInterfaces(&g_handle, &filter, g_profiles, 16);
        probe_say("controller %d: %d interface(s)", controller, (int)found);

        uint32_t ifh = 0;
        for (int32_t i = 0; i < found; i++) {
            const UhsInterfaceProfile *p = &g_profiles[i];
            probe_say("   %04x:%04x", p->dev_desc.idVendor, p->dev_desc.idProduct);
            if (p->dev_desc.idVendor == ASIX_VID && p->dev_desc.idProduct == AX88179_PID) {
                ifh = p->if_handle;
            }
        }
        if (!ifh) {
            UhsClientClose(&g_handle);
            continue;
        }

        probe_say("   ASIX is HERE, on controller %d", controller);
        g_acquired = 0;
        g_acquire_result = 0;
        r = UhsAcquireInterface(&g_handle, ifh, NULL, on_acquired);
        probe_say("   acquire submitted -> %d", (int)r);
        if (r < 0) {
            UhsClientClose(&g_handle);
            continue;
        }
        /* Wait for the callback rather than assuming it has happened. */
        int waited = 0;
        while (!g_acquired && waited < 3000) {
            OSSleepTicks(OSMillisecondsToTicks(10));
            waited += 10;
        }
        probe_say("   acquire %s after %d ms, result %d",
                  g_acquired ? "COMPLETED" : "never called back", waited,
                  (int)g_acquire_result);
        r = UhsAdministerEndpoint(&g_handle, ifh, UHS_ADMIN_EP_ENABLE, 0xFFFF, 4, 2048);
        probe_say("   enable endpoints -> %d", (int)r);
        r = UhsSubmitBulkRequest(&g_handle, ifh, 0x02, UHS_DIR_IN, g_buf, 2048, 500);
        probe_say("   bulk in ep2 -> %d %s", (int)r,
                  r >= 0 ? "*** DATA MOVES ***" : "(refused)");
        UhsReleaseInterface(&g_handle, ifh, false);
        UhsClientClose(&g_handle);
    }

    probe_wait();
    probe_shutdown();
    return 0;
}
