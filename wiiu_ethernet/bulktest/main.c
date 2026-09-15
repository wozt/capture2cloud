/*
 * What does AdministerEndpoint actually establish?
 *
 * It reports -2162715 to us while IOSU's log shows it received and
 * processed the call with exactly the mask passed. So its return value
 * is not to be trusted, and neither is the assumption that whatever it
 * set up is usable -- it may have recorded a request size that bounds
 * every later transfer.
 *
 * Several (pending, size) pairs, each followed by a bulk attempt at the
 * size it was given. The interesting output is not on this screen: it
 * is in /storage_slc/sys/logs, where the UHS server traces what it
 * really did. Each attempt is numbered so the two can be lined up.
 */
#include <stdio.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

#include "probe.h"

#define ASIX_VID    0x0b95
#define AX88179_PID 0x1790
#define UHS_DIR_OUT 1
#define UHS_DIR_IN  2

static UhsHandle g_handle;
static uint8_t g_work[128 * 1024] __attribute__((aligned(0x40)));
static uint8_t g_buf[64 * 1024] __attribute__((aligned(0x40)));

static volatile int g_probed;
static volatile uint32_t g_if;

static void on_probe(void *context, UhsInterfaceProfile *profile)
{
    (void)context;
    if (profile) {
        g_if = profile->if_handle;
        g_probed = 1;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("UHS: endpoint sizes") != 0) {
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
    for (int w = 0; w < 3000 && !g_probed; w += 20) {
        OSSleepTicks(OSMillisecondsToTicks(20));
    }
    if (!g_probed) {
        probe_say("no probe (reg %d)", (int)reg);
        probe_wait(); probe_shutdown(); return 1;
    }
    UhsAcquireInterface(&g_handle, g_if, NULL, NULL);
    OSSleepTicks(OSMillisecondsToTicks(300));
    probe_say("interface %u acquired", (unsigned)g_if);

    /* Endpoint 2 alone as well as everything, and a spread of sizes. */
    static const struct { uint32_t mask; uint32_t pending; uint32_t size; } tries[] = {
        { 0xFFFF, 4,  512 },
        { 0xFFFF, 1,  512 },
        { 1u << 2, 4, 512 },
        { 1u << 2, 4, 2048 },
        { 0xFFFF, 8, 16384 },
    };

    for (unsigned i = 0; i < sizeof(tries) / sizeof(*tries); i++) {
        const int32_t en = UhsAdministerEndpoint(&g_handle, g_if, UHS_ADMIN_EP_ENABLE,
                                                 tries[i].mask, tries[i].pending, tries[i].size);
        memset(g_buf, 0, tries[i].size);
        DCFlushRange(g_buf, tries[i].size);
        const int32_t r = UhsSubmitBulkRequest(&g_handle, g_if, 0x02, UHS_DIR_IN, g_buf,
                                               (int)tries[i].size, 300);
        DCInvalidateRange(g_buf, tries[i].size);
        probe_say("#%u mask %04X pend %u size %5u: enable %d, bulk %d %s", i,
                  (unsigned)tries[i].mask, (unsigned)tries[i].pending, (unsigned)tries[i].size,
                  (int)en, (int)r, r >= 0 ? "*** ACCEPTED ***" : "");
        OSSleepTicks(OSMillisecondsToTicks(100));
    }

    probe_say("now read /storage_slc/sys/logs -- the truth is there");
    UhsReleaseInterface(&g_handle, g_if, false);
    UhsClassDrvUnReg(&g_handle, (uint32_t)reg);
    UhsClientClose(&g_handle);
    probe_wait();
    probe_shutdown();
    return 0;
}
