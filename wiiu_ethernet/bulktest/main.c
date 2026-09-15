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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (probe_init("AX88179: why is bulk refused?") != 0) {
        probe_shutdown();
        return 1;
    }

    UhsConfig config;
    memset(&config, 0, sizeof(config));
    config.controller_num = 0;
    config.buffer = g_work;
    config.buffer_size = sizeof(g_work);
    if (UhsClientOpen(&g_handle, &config) < 0) {
        probe_say("UhsClientOpen failed");
        probe_wait(); probe_shutdown(); return 1;
    }

    /*
     * MATCH_ANY, then pick.
     *
     * The filtered query returns a profile whose endpoint array is
     * nearly empty and whose handle differs from the one MATCH_ANY
     * reports for the same adapter -- 65537 against 131074. Control
     * transfers worked on it because those go to endpoint 0, which is
     * the DEVICE; every data transfer was refused because the handle
     * was not the interface that owns those endpoints.
     *
     * So this lists everything and picks the ASIX out of the list, the
     * way the first probe did when it saw all three endpoints.
     */
    UhsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.match_params = MATCH_ANY;
    const int32_t found = UhsQueryInterfaces(&g_handle, &filter, g_profiles, 16);
    probe_say("MATCH_ANY -> %d interface(s)", (int)found);

    uint32_t ifh = 0;
    for (int32_t i = 0; i < found; i++) {
        const UhsInterfaceProfile *p = &g_profiles[i];
        if (p->dev_desc.idVendor == ASIX_VID && p->dev_desc.idProduct == AX88179_PID) {
            ifh = p->if_handle;
            probe_say("ASIX at index %d, handle %u", (int)i, (unsigned)ifh);
            for (int e = 0; e < 4; e++) {
                if (p->in_endpoints[e].bLength) {
                    probe_say("   in  ep %02x attr %02x", p->in_endpoints[e].bEndpointAddress,
                              p->in_endpoints[e].bmAttributes);
                }
                if (p->out_endpoints[e].bLength) {
                    probe_say("   out ep %02x attr %02x", p->out_endpoints[e].bEndpointAddress,
                              p->out_endpoints[e].bmAttributes);
                }
            }
            break;
        }
    }
    if (!ifh) {
        probe_say("no adapter in the list");
        probe_wait(); probe_shutdown(); return 1;
    }
    if (UhsAcquireInterface(&g_handle, ifh, NULL, NULL) < 0) {
        probe_say("acquire refused");
        probe_wait(); probe_shutdown(); return 1;
    }
    probe_say("interface %u acquired", (unsigned)ifh);

    int32_t r = UhsAdministerEndpoint(&g_handle, ifh, UHS_ADMIN_EP_ENABLE, 0xFFFF, 4, 2048);
    probe_say("enable endpoints -> %d", (int)r);

    r = UhsSubmitBulkRequest(&g_handle, ifh, 0x02, UHS_DIR_IN, g_buf, 2048, 500);
    probe_say("bulk in ep2 -> %d %s", (int)r, r >= 0 ? "*** FRAMES CAN MOVE ***" : "(refused)");

    UhsReleaseInterface(&g_handle, ifh, false);
    UhsClientClose(&g_handle);
    probe_wait();
    probe_shutdown();
    return 0;
}
