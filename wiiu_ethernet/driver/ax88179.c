#include "ax88179.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

/*
 * The direction argument is 1 or 2, and neither is zero.
 *
 * Read out of nsysuhs.rpl rather than assumed. UhsSubmitBulkRequest
 * builds an ioctlv and counts its vectors from the direction:
 *
 *     param_4 == 1  ->  2 in, 0 out   the buffer is SENT      (write)
 *     param_4 == 2  ->  1 in, 1 out   the buffer is FILLED    (read)
 *     anything else ->  neither branch runs, and the call goes out
 *                       malformed
 *
 * The obvious guess -- 0 for out, 1 for in -- is wrong twice over: it
 * asks to WRITE to an IN endpoint, and its "out" is a request with no
 * vectors at all. Control transfers were unaffected because they take
 * no direction argument; theirs comes from bmRequestType, which is why
 * they were the one thing that always worked.
 */
#define UHS_DIR_OUT 1
#define UHS_DIR_IN  2

/* --- the chip, as its registers -------------------------------------- */
/* Numbers from Linux's ax88179_178a.c. A number a chip answers to is a
 * fact about the chip. */
#define AX_ACCESS_MAC            0x01
#define AX_ACCESS_PHY            0x02

#define AX_NODE_ID               0x10
#define AX_RX_CTL                0x0b
#define AX_MEDIUM_STATUS_MODE    0x22
#define AX_MONITOR_MOD           0x24
#define AX_PHYPWR_RSTCTL         0x26
#define AX_RX_BULKIN_QCTRL       0x2e
#define AX_CLK_SELECT            0x33
#define AX_RXCOE_CTL             0x34
#define AX_TXCOE_CTL             0x35
#define AX_PAUSE_WATERLVL_HIGH   0x54
#define AX_PAUSE_WATERLVL_LOW    0x55

#define AX_PHYPWR_RSTCTL_IPRL    0x0020
#define AX_CLK_SELECT_BCS        0x01
#define AX_CLK_SELECT_ACS        0x02

#define AX_RX_CTL_DROPCRCERR     0x0100
#define AX_RX_CTL_IPE            0x0200
#define AX_RX_CTL_START          0x0080
#define AX_RX_CTL_AP             0x0020
#define AX_RX_CTL_AB             0x0008
#define AX_RX_CTL_AMALL          0x0002

#define AX_MONITOR_MODE_RWMP     0x04
#define AX_MONITOR_MODE_PMEPOL   0x20
#define AX_MONITOR_MODE_PMETYPE  0x40

#define AX_RXHDR_CRC_ERR         0x20000000u
#define AX_RXHDR_DROP_ERR        0x80000000u

/* PHY, through the same control endpoint */
#define AX88179_PHY_ID           0x03
#define MII_BMSR                 0x01
#define MII_BMSR_LINK            0x0004
#define AX_PHYSICAL_LINK_STATUS  0x02
#define AX_USB_SS                0x04   /* the speed bits in MEDIUM_STATUS */

#define ASIX_VID                 0x0b95
#define AX88179_PID              0x1790

/* Control transfer direction/type for this chip: vendor request to the
 * device itself. */
/* UhsSubmitBulkRequest's direction argument. The header names no
 * constants for it, so they are named here. */


#define REQ_IN   0xC0
#define REQ_OUT  0x40

#define UHS_WORK_SIZE   (128 * 1024)
#define MAX_IFACES      16
/* The bulk IN carries several frames at once, headers and all. */
#define RX_BUFFER_SIZE  (16 * 1024)

struct Ax88179 {
    UhsHandle handle;
    uint32_t  if_handle;
    uint8_t   ep_in;      /* bulk, frames from the wire */
    uint8_t   ep_out;     /* bulk, frames to it */
    uint8_t   ep_irq;     /* interrupt, link status */
    uint8_t   mac[AX88179_MAC_LEN];

    /* What is left of the last bulk transfer, still to be unwrapped. */
    int       rx_len;
    int       rx_frames;      /* how many frames its header promised */
    int       rx_next;        /* which one comes next */
    uint32_t  rx_hdr_offset;  /* where the descriptors live */
    int       rx_pos;         /* how far into the frames we have walked */
    int32_t   last_bulk;      /* what the last bulk request returned */
};

/* UHS DMAs into these, so they are aligned and static rather than on a
 * stack that moves. */
static uint8_t g_uhs_work[UHS_WORK_SIZE] __attribute__((aligned(0x40)));
static uint8_t g_ctrl[64] __attribute__((aligned(0x40)));
static uint8_t g_rx[RX_BUFFER_SIZE] __attribute__((aligned(0x40)));
static uint8_t g_tx[2048] __attribute__((aligned(0x40)));
static UhsInterfaceProfile g_profiles[MAX_IFACES];

/* The chip speaks little-endian and this console does not. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void sleep_ms(int ms)
{
    OSSleepTicks(OSMillisecondsToTicks((uint32_t)ms));
}

/* --- register access -------------------------------------------------- */

static int reg_write(Ax88179 *ax, uint8_t cmd, uint16_t reg, uint16_t size,
                     const void *data)
{
    if (size > sizeof(g_ctrl)) {
        return -1;
    }
    memcpy(g_ctrl, data, size);
    DCFlushRange(g_ctrl, sizeof(g_ctrl));
    const int32_t r = UhsSubmitControlRequest(&ax->handle, ax->if_handle, g_ctrl, cmd, REQ_OUT,
                                              reg, size, size, 1000);
    return r >= 0 ? 0 : -1;
}

static int reg_read(Ax88179 *ax, uint8_t cmd, uint16_t reg, uint16_t size, void *out)
{
    if (size > sizeof(g_ctrl)) {
        return -1;
    }
    memset(g_ctrl, 0, sizeof(g_ctrl));
    DCFlushRange(g_ctrl, sizeof(g_ctrl));
    const int32_t r = UhsSubmitControlRequest(&ax->handle, ax->if_handle, g_ctrl, cmd, REQ_IN,
                                              reg, size, size, 1000);
    if (r < 0) {
        return -1;
    }
    DCInvalidateRange(g_ctrl, sizeof(g_ctrl));
    memcpy(out, g_ctrl, size);
    return 0;
}

static int mac_write8(Ax88179 *ax, uint16_t reg, uint8_t value)
{
    return reg_write(ax, AX_ACCESS_MAC, reg, 1, &value);
}

static int mac_write16(Ax88179 *ax, uint16_t reg, uint16_t value)
{
    /* The chip is little-endian and this console is not. */
    const uint8_t le[2] = { (uint8_t)(value & 0xFF), (uint8_t)(value >> 8) };
    return reg_write(ax, AX_ACCESS_MAC, reg, 2, le);
}

/* --- finding and opening ---------------------------------------------- */

/*
 * The endpoints, taken from the interface descriptor rather than
 * assumed.
 *
 * The first probes read in_endpoints[0] and [1] and got different
 * answers from a filtered query than from MATCH_ANY -- the array is
 * sparse, not packed. So this walks it and sorts by what each endpoint
 * IS: bulk in, bulk out, interrupt in. A bulk transfer to the wrong
 * endpoint fails silently, which is the worst way for this to be wrong.
 */
static int find_endpoints(Ax88179 *ax, const UhsInterfaceProfile *p)
{
    ax->ep_in = ax->ep_out = ax->ep_irq = 0;
    for (int i = 0; i < 16; i++) {
        const UhsEndpointDescriptor *in = &p->in_endpoints[i];
        if (in->bLength && (in->bEndpointAddress & 0x80)) {
            const uint8_t kind = in->bmAttributes & 0x03;
            if (kind == 0x02 && !ax->ep_in) {
                ax->ep_in = in->bEndpointAddress & 0x0F;
            } else if (kind == 0x03 && !ax->ep_irq) {
                ax->ep_irq = in->bEndpointAddress & 0x0F;
            }
        }
        const UhsEndpointDescriptor *out = &p->out_endpoints[i];
        if (out->bLength && !(out->bEndpointAddress & 0x80)) {
            if ((out->bmAttributes & 0x03) == 0x02 && !ax->ep_out) {
                ax->ep_out = out->bEndpointAddress & 0x0F;
            }
        }
    }
    return (ax->ep_in && ax->ep_out) ? 0 : -1;
}

Ax88179 *ax88179_open(char *why, unsigned why_size)
{
    Ax88179 *ax = calloc(1, sizeof(*ax));
    if (!ax) {
        snprintf(why, why_size, "out of memory");
        return NULL;
    }

    UhsConfig config;
    memset(&config, 0, sizeof(config));
    config.controller_num = 0;
    config.buffer = g_uhs_work;
    config.buffer_size = sizeof(g_uhs_work);
    if (UhsClientOpen(&ax->handle, &config) < 0) {
        snprintf(why, why_size, "cannot open /dev/uhs");
        free(ax);
        return NULL;
    }

    UhsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.match_params = MATCH_DEV_VID | MATCH_DEV_PID;
    filter.vid = ASIX_VID;
    filter.pid = AX88179_PID;

    const int32_t found = UhsQueryInterfaces(&ax->handle, &filter, g_profiles, MAX_IFACES);
    if (found <= 0) {
        snprintf(why, why_size, "no AX88179 on the bus");
        UhsClientClose(&ax->handle);
        free(ax);
        return NULL;
    }

    ax->if_handle = g_profiles[0].if_handle;
    if (find_endpoints(ax, &g_profiles[0]) != 0) {
        snprintf(why, why_size, "no bulk endpoint pair on the interface");
        UhsClientClose(&ax->handle);
        free(ax);
        return NULL;
    }

    if (UhsAcquireInterface(&ax->handle, ax->if_handle, NULL, NULL) < 0) {
        snprintf(why, why_size, "the interface was refused");
        UhsClientClose(&ax->handle);
        free(ax);
        return NULL;
    }

    /*
     * Bring-up, in the order the chip wants it.
     *
     * The PHY is powered down out of reset; it has to be let go, given
     * half a second to come up, and only then can the clocks be
     * selected. Skipping either wait is the classic way to end up with
     * an adapter whose LED never lights.
     */
    uint16_t zero = 0;
    reg_write(ax, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, &zero);
    if (mac_write16(ax, AX_PHYPWR_RSTCTL, AX_PHYPWR_RSTCTL_IPRL) != 0) {
        snprintf(why, why_size, "the chip will not take a register write");
        UhsReleaseInterface(&ax->handle, ax->if_handle, false);
        UhsClientClose(&ax->handle);
        free(ax);
        return NULL;
    }
    sleep_ms(500);

    mac_write8(ax, AX_CLK_SELECT, AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS);
    sleep_ms(200);

    if (reg_read(ax, AX_ACCESS_MAC, AX_NODE_ID, AX88179_MAC_LEN, ax->mac) != 0) {
        snprintf(why, why_size, "cannot read the hardware address");
        UhsReleaseInterface(&ax->handle, ax->if_handle, false);
        UhsClientClose(&ax->handle);
        free(ax);
        return NULL;
    }

    /* Flow-control watermarks, then receive on: drop bad CRCs, accept
     * broadcast and multicast, and run promiscuous -- this is not the
     * system's interface, so nothing else is filtering for us. */
    mac_write8(ax, AX_PAUSE_WATERLVL_HIGH, 0x34);
    mac_write8(ax, AX_PAUSE_WATERLVL_LOW, 0x52);
    mac_write8(ax, AX_RXCOE_CTL, 0);   /* no checksum offload: we check our own */
    mac_write8(ax, AX_TXCOE_CTL, 0);
    mac_write16(ax, AX_RX_CTL, AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE | AX_RX_CTL_START |
                                   AX_RX_CTL_AP | AX_RX_CTL_AMALL | AX_RX_CTL_AB);
    mac_write8(ax, AX_MONITOR_MOD,
               AX_MONITOR_MODE_PMETYPE | AX_MONITOR_MODE_PMEPOL | AX_MONITOR_MODE_RWMP);

    /*
     * The endpoints have to be enabled before a bulk transfer will do
     * anything.
     *
     * Missed on the first attempt, and the symptom was exactly what one
     * would expect from a driver that is otherwise working: the chip
     * initialised, the PHY negotiated, the link came up at 100 Mbit/s
     * -- and not one frame ever arrived. The host stack was simply not
     * carrying them.
     */
    UhsAdministerEndpoint(&ax->handle, ax->if_handle, UHS_ADMIN_EP_ENABLE,
                          1u << ax->ep_in, 4, RX_BUFFER_SIZE);
    UhsAdministerEndpoint(&ax->handle, ax->if_handle, UHS_ADMIN_EP_ENABLE,
                          1u << ax->ep_out, 4, sizeof(g_tx));
    return ax;
}

void ax88179_close(Ax88179 *ax)
{
    if (!ax) {
        return;
    }
    mac_write16(ax, AX_RX_CTL, 0);
    UhsReleaseInterface(&ax->handle, ax->if_handle, false);
    UhsClientClose(&ax->handle);
    free(ax);
}

const uint8_t *ax88179_mac(const Ax88179 *ax)
{
    return ax ? ax->mac : NULL;
}

int ax88179_link(Ax88179 *ax, int *speed)
{
    uint8_t status = 0;
    if (!ax || reg_read(ax, AX_ACCESS_MAC, AX_PHYSICAL_LINK_STATUS, 1, &status) != 0) {
        if (speed) *speed = 0;
        return 0;
    }
    /* The low bits say which speed the PHY settled on. */
    int s = 0;
    if (status & 0x01) s = 10;
    if (status & 0x02) s = 100;
    if (status & 0x04) s = 1000;
    if (speed) *speed = s;
    return s != 0;
}

int ax88179_send(Ax88179 *ax, const void *frame, int length)
{
    if (!ax || length <= 0 || (size_t)length + 8 > sizeof(g_tx)) {
        return -1;
    }
    /*
     * Two little-endian words in front of the frame: the length, and a
     * padding word this chip wants. Without them the adapter accepts
     * the transfer and puts nothing on the wire.
     */
    const uint32_t hdr0 = (uint32_t)length;
    const uint32_t hdr1 = 0;
    g_tx[0] = (uint8_t)(hdr0 & 0xFF);
    g_tx[1] = (uint8_t)((hdr0 >> 8) & 0xFF);
    g_tx[2] = (uint8_t)((hdr0 >> 16) & 0xFF);
    g_tx[3] = (uint8_t)((hdr0 >> 24) & 0xFF);
    memcpy(g_tx + 4, &hdr1, 4);
    memcpy(g_tx + 8, frame, (size_t)length);
    DCFlushRange(g_tx, sizeof(g_tx));

    const int32_t r = UhsSubmitBulkRequest(&ax->handle, ax->if_handle, ax->ep_out,
                                           UHS_DIR_OUT, g_tx, length + 8, 1000);
    return r >= 0 ? 0 : -1;
}

int ax88179_receive(Ax88179 *ax, void *frame, int max_length, int timeout_ms)
{
    if (!ax) {
        return -1;
    }

    /*
     * The chip's receive wrapper, which is the thing to get right.
     *
     * A bulk transfer carries several frames and the metadata is at the
     * END of it, not the front:
     *
     *     <frame 1><pad to 8>  ...  <frame N><pad to 8>
     *     <descriptor 1><dummy>  ...  <descriptor N><dummy>
     *     <pad><trailer>
     *
     * The last four bytes give the descriptor count and where the
     * descriptors start. Each descriptor's bits 16..28 are that frame's
     * length, and each frame begins with TWO bytes of alignment padding
     * before the ethernet header. Descriptors come in pairs -- one real,
     * one dummy with length zero -- so a zero length is skipped rather
     * than treated as the end.
     *
     * Reading any of that from the front instead produces plausible
     * lengths and silently corrupted packets, which is precisely the
     * fault the AX88772B patcher had to fix on the other chip. Hence
     * the care, and hence the bounds checks: a malformed descriptor
     * must not walk off the buffer.
     */
    for (;;) {
        if (ax->rx_next >= ax->rx_frames) {
            const int32_t got = UhsSubmitBulkRequest(&ax->handle, ax->if_handle, ax->ep_in,
                                                     UHS_DIR_IN, g_rx, sizeof(g_rx), timeout_ms);
            ax->last_bulk = got;
            if (got <= 4) {
                return 0;
            }
            DCInvalidateRange(g_rx, sizeof(g_rx));
            ax->rx_len = (int)got;

            const uint8_t *trailer = g_rx + ax->rx_len - 4;
            const uint32_t rx_hdr = le32(trailer);
            ax->rx_frames = (int)(rx_hdr & 0xFFFF);
            ax->rx_hdr_offset = (rx_hdr >> 16) & 0xFFFF;
            ax->rx_next = 0;
            ax->rx_pos = 0;

            if (ax->rx_frames <= 0 ||
                ax->rx_hdr_offset + 4u * (uint32_t)ax->rx_frames > (uint32_t)ax->rx_len - 4u) {
                ax->rx_frames = 0;
                return 0;
            }
        }

        const uint8_t *desc = g_rx + ax->rx_hdr_offset + 4 * ax->rx_next;
        const uint32_t d = le32(desc);
        ax->rx_next++;

        const int length = (int)((d >> 16) & 0x1FFF);
        if (length == 0) {
            continue;   /* the dummy descriptor of the pair */
        }
        const int padded = (length + 7) & ~7;

        if (ax->rx_pos + padded > (int)ax->rx_hdr_offset) {
            ax->rx_frames = 0;   /* it would overlap the descriptors */
            return 0;
        }
        const uint8_t *packet = g_rx + ax->rx_pos + 2;   /* 2 bytes of alignment */
        const int packet_len = length - 2;
        ax->rx_pos += padded;

        /* Bad CRC, or too short to be an ethernet frame at all. */
        if ((d & (AX_RXHDR_CRC_ERR | AX_RXHDR_DROP_ERR)) || packet_len < 14) {
            continue;
        }
        if (packet_len > max_length) {
            continue;
        }
        memcpy(frame, packet, (size_t)packet_len);
        return packet_len;
    }
}

int32_t ax88179_last_bulk(const Ax88179 *ax)
{
    return ax ? ax->last_bulk : 0;
}

void ax88179_endpoints(const Ax88179 *ax, int *in, int *out, int *irq)
{
    if (!ax) {
        return;
    }
    if (in)  *in  = ax->ep_in;
    if (out) *out = ax->ep_out;
    if (irq) *irq = ax->ep_irq;
}
