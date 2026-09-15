/*
 * Which handle does the acquire want?
 *
 * Ghidra settled what the handler requires:
 *
 *   iface = lookup(server, handle);        // (handle & 0xFFFF) < 0x80, and
 *                                          // handle must EQUAL iface[0x20]
 *   if (iface && iface[0x28] == 0          // unowned
 *             && (iface[0x10] == 4 || iface[0x10] == 1)   // ORPHANED or PROBING
 *             && (iface[0x2c] < 0 || iface[0x2c] == pid))
 *       ... reply(request, 0);             // the ONLY reply
 *
 * A failure returns an error internally and never answers, so silence
 * IS the rejection. What cannot be seen from outside is which of the
 * four tests failed.
 *
 * The cheapest candidate is the first. A probe indication offers
 * 196611 = 0x00030003 (slot 3) where a query returns 65537 = 0x00010001
 * (slot 1) for the same adapter, and the lookup demands an exact match
 * against what the slot holds. So: try both, in both orders, and see
 * whether either is ever answered.
 */
#include <stdio.h>
#include <string.h>

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

#include "probe.h"

#define ASIX_VID    0x0b95
#define AX88179_PID 0x1790

static UhsHandle g_handle;
static uint8_t g_work[128 * 1024] __attribute__((aligned(0x40)));
static UhsInterfaceProfile g_profiles[16];

static volatile int g_probed;
static volatile uint32_t g_probe_handle;
static volatile int g_acquired;
static volatile uint32_t g_acquired_handle;

static void on_acquired(void *context, int32_t arg1, int32_t arg2)
{
    (void)arg2;
    g_acquired_handle = (uint32_t)(uintptr_t)context;
    (void)arg1;
    g_acquired = 1;
}

static void on_probe(void *context, UhsInterfaceProfile *profile)
{
    (void)context;
    if (profile) {
        g_probe_handle = profile->if_handle;
        g_probed = 1;
    }
}

static void try_handle(const char *label, uint32_t h)
{
    g_acquired = 0;
    const int32_t r = UhsAcquireInterface(&g_handle, h, (void *)(uintptr_t)h, on_acquired);
    for (int waited = 0; waited < 800 && !g_acquired; waited += 20) {
        OSSleepTicks(OSMillisecondsToTicks(20));
    }
    probe_say("%s: handle %u (0x%08X, slot %u) submit %d -> %s", label, (unsigned)h,
              (unsigned)h, (unsigned)(h & 0xFFFF), (int)r,
              g_acquired ? "*** ANSWERED ***" : "silence");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("UHS: which handle?") != 0) {
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

    /* What a plain query calls it. */
    UhsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.match_params = MATCH_DEV_VID | MATCH_DEV_PID;
    filter.vid = ASIX_VID;
    filter.pid = AX88179_PID;
    uint32_t query_handle = 0;
    if (UhsQueryInterfaces(&g_handle, &filter, g_profiles, 16) > 0) {
        query_handle = g_profiles[0].if_handle;
    }
    probe_say("query says handle %u (slot %u)", (unsigned)query_handle,
              (unsigned)(query_handle & 0xFFFF));

    /* What a registration is offered. */
    const int32_t reg = UhsClassDrvReg(&g_handle, &filter, NULL, on_probe);
    for (int waited = 0; waited < 3000 && !g_probed; waited += 20) {
        OSSleepTicks(OSMillisecondsToTicks(20));
    }
    probe_say("registration %d, probe %s, handle %u (slot %u)", (int)reg,
              g_probed ? "arrived" : "never came", (unsigned)g_probe_handle,
              (unsigned)(g_probe_handle & 0xFFFF));

    if (g_probe_handle) {
        try_handle("probe handle ", g_probe_handle);
    }
    if (query_handle && query_handle != g_probe_handle) {
        try_handle("query handle ", query_handle);
    }
    /* And the query again, now that a registration exists -- the slot
     * may have been renumbered by the registration itself. */
    if (UhsQueryInterfaces(&g_handle, &filter, g_profiles, 16) > 0) {
        const uint32_t again = g_profiles[0].if_handle;
        probe_say("query after registering says %u (slot %u)", (unsigned)again,
                  (unsigned)(again & 0xFFFF));
        if (again != g_probe_handle && again != query_handle) {
            try_handle("re-query    ", again);
        }
    }

    UhsClassDrvUnReg(&g_handle, (uint32_t)reg);
    UhsClientClose(&g_handle);
    probe_wait();
    probe_shutdown();
    return 0;
}
