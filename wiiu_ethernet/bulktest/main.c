/*
 * Register as a class driver, the way the console's own driver does.
 *
 * The /dev/uhs documentation settles what a dozen permutations could
 * not. Acquiring is only half a protocol: ioctl 0x01, UhsClassDrvReg,
 * registers a driver with a filter, and UHS then CALLS YOU when a
 * matching interface appears, handing you its profile. That is exactly
 * the shape of the IOSU driver found in the decrypted firmware --
 * `__uhsIfProbeCallback`, `__handleUhsDevProbe`.
 *
 * Every probe so far queried and then acquired, which is not the
 * supported flow, and an acquire that is accepted and never completes
 * is precisely what one would expect of it.
 */
#include <stdio.h>
#include <string.h>

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

#include "probe.h"

#define ASIX_VID    0x0b95
#define AX88179_PID 0x1790
#define UHS_DIR_IN  1

static UhsHandle g_handle;
static uint8_t g_work[128 * 1024] __attribute__((aligned(0x40)));
static uint8_t g_buf[16 * 1024] __attribute__((aligned(0x40)));

static volatile int g_probed;
static volatile uint32_t g_probed_if;
static volatile int g_acquired;
static volatile int32_t g_acquire_result;

/* UHS offering us an interface it thinks we drive. */
static void on_probe(void *context, UhsInterfaceProfile *profile)
{
    (void)context;
    if (profile) {
        g_probed_if = profile->if_handle;
        g_probed = 1;
    }
}

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

    if (probe_init("UHS: register as a class driver") != 0) {
        probe_shutdown();
        return 1;
    }

    UhsConfig config;
    memset(&config, 0, sizeof(config));
    config.controller_num = 0;
    config.buffer = g_work;
    config.buffer_size = sizeof(g_work);
    if (UhsClientOpen(&g_handle, &config) < 0) {
        probe_say("client will not open");
        probe_wait(); probe_shutdown(); return 1;
    }

    UhsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.match_params = MATCH_DEV_VID | MATCH_DEV_PID;
    filter.vid = ASIX_VID;
    filter.pid = AX88179_PID;

    const int32_t reg = UhsClassDrvReg(&g_handle, &filter, NULL, on_probe);
    probe_say("UhsClassDrvReg -> %d", (int)reg);

    for (int waited = 0; waited < 3000 && !g_probed; waited += 20) {
        OSSleepTicks(OSMillisecondsToTicks(20));
    }
    probe_say("probe callback %s", g_probed ? "ARRIVED" : "never came");

    if (g_probed) {
        probe_say("offered interface %u", (unsigned)g_probed_if);
        g_acquired = 0;
        const int32_t a = UhsAcquireInterface(&g_handle, g_probed_if, NULL, on_acquired);
        probe_say("acquire -> %d", (int)a);
        for (int waited = 0; waited < 3000 && !g_acquired; waited += 20) {
            OSSleepTicks(OSMillisecondsToTicks(20));
        }
        probe_say("acquire callback %s, result %d", g_acquired ? "ARRIVED" : "never came",
                  (int)g_acquire_result);

        int32_t r = UhsAdministerEndpoint(&g_handle, g_probed_if, UHS_ADMIN_EP_ENABLE,
                                          0xFFFF, 4, 2048);
        probe_say("enable endpoints -> %d", (int)r);
        r = UhsSubmitBulkRequest(&g_handle, g_probed_if, 0x02, UHS_DIR_IN, g_buf, 2048, 500);
        probe_say("bulk in ep2 -> %d %s", (int)r, r >= 0 ? "*** DATA MOVES ***" : "");
        UhsReleaseInterface(&g_handle, g_probed_if, false);
    }

    UhsClassDrvUnReg(&g_handle, (uint32_t)reg);
    UhsClientClose(&g_handle);
    probe_wait();
    probe_shutdown();
    return 0;
}
