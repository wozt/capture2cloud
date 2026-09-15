/*
 * Which memory may an ioctlv vector point at?
 *
 * Established: the acquire is granted (IOSU's own log says so), the
 * endpoints are enabled, control transfers work -- and only the bulk
 * request is refused, by IOS itself rather than by nsysuhs.rpl. A small
 * ioctl payload is copied; an ioctlv vector is used where it lies. So
 * the buffer's region is the first thing to vary.
 *
 * Four sources, one run: the program's own .bss, the default heap, and
 * the MEM1 and MEM2 base heaps. Whichever the transfer accepts is the
 * answer; the log also prints each address so the regions are visible.
 */
#include <stdio.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

#include "probe.h"

#define ASIX_VID    0x0b95
#define AX88179_PID 0x1790

/* 1 = write (2 in, 0 out), 2 = read (1 in, 1 out). Read out of
 * nsysuhs.rpl; zero takes neither branch. */
#define UHS_DIR_OUT 1
#define UHS_DIR_IN  2

#define XFER 2048

static UhsHandle g_handle;
static uint8_t g_work[128 * 1024] __attribute__((aligned(0x40)));
static uint8_t g_bss[16 * 1024] __attribute__((aligned(0x40)));
static UhsInterfaceProfile g_profiles[16];

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

static void try_buffer(const char *what, void *buf)
{
    if (!buf) {
        probe_say("%-14s: could not allocate", what);
        return;
    }
    memset(buf, 0, XFER);
    DCFlushRange(buf, XFER);
    const int32_t r = UhsSubmitBulkRequest(&g_handle, g_if, 0x02, UHS_DIR_IN, buf, XFER, 400);
    DCInvalidateRange(buf, XFER);
    probe_say("%-14s @ %08X -> %d %s", what, (unsigned)(uintptr_t)buf, (int)r,
              r >= 0 ? "*** ACCEPTED ***" : "");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("UHS: which memory for a vector?") != 0) {
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
    OSSleepTicks(OSMillisecondsToTicks(200));
    const int32_t en = UhsAdministerEndpoint(&g_handle, g_if, UHS_ADMIN_EP_ENABLE, 0xFFFF, 4,
                                             XFER);
    probe_say("interface %u, enable -> %d", (unsigned)g_if, (int)en);

    try_buffer("own .bss", g_bss);
    try_buffer("inside work", g_work + 64 * 1024);
    try_buffer("default heap", MEMAllocFromDefaultHeapEx(XFER, 0x40));

    MEMHeapHandle mem1 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
    MEMHeapHandle mem2 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
    try_buffer("MEM1 heap", mem1 ? MEMAllocFromExpHeapEx(mem1, XFER, 0x40) : NULL);
    try_buffer("MEM2 heap", mem2 ? MEMAllocFromExpHeapEx(mem2, XFER, 0x40) : NULL);

    UhsReleaseInterface(&g_handle, g_if, false);
    UhsClassDrvUnReg(&g_handle, (uint32_t)reg);
    UhsClientClose(&g_handle);
    probe_wait();
    probe_shutdown();
    return 0;
}
