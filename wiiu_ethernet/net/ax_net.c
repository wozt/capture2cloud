#include "ax_net.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <coreinit/messagequeue.h>
#include <whb/log.h>
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

/*
 * Threaded lwIP port: the tcpip thread owns the lwIP core, the caller of
 * ax_net_poll (the module's worker thread) exclusively owns UHS and the
 * adapter. Received frames go worker -> tcpip_input; outgoing frames go
 * tcpip -> a slot queue -> the worker, which is the only thread that ever
 * calls into the AX88179 driver.
 */

#define TX_SLOTS 8

static struct netif iface;
static uint8_t rx_frame[1600];
static uint8_t tx_pool[TX_SLOTS][1600];
static atomic_int tx_in_use[TX_SLOTS];
static OSMessageQueue tx_queue;
static OSMessage tx_storage[TX_SLOTS];
static int initialized, active, link_errors, last_link_up;
static uint32_t last_link;
static char address[16];

static err_t send_frame(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (p->tot_len > sizeof(tx_pool[0])) return ERR_BUF;
    /* Only the tcpip thread reaches here, so slot picking is single-threaded. */
    int slot = -1;
    for (int i = 0; i < TX_SLOTS; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&tx_in_use[i], &expected, 1)) { slot = i; break; }
    }
    if (slot < 0) return ERR_MEM;
    if (pbuf_copy_partial(p, tx_pool[slot], p->tot_len, 0) != p->tot_len) {
        atomic_store(&tx_in_use[slot], 0);
        return ERR_BUF;
    }
    OSMessage m;
    memset(&m, 0, sizeof(m));
    m.message = (void *)(uintptr_t)slot;
    m.args[0] = p->tot_len;
    OSSendMessage(&tx_queue, &m, OS_MESSAGE_FLAGS_BLOCKING);
    return ERR_OK;
}

static err_t init_interface(struct netif *n)
{
    n->name[0] = 'a'; n->name[1] = 'x';
    n->hostname = "wiiu-ax88179";
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, ax88179_mac(n->state), 6);
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = send_frame;
    return ERR_OK;
}

struct setup_ctx {
    Ax88179 *ax;
    int link_up;
    err_t err;
};

static void setup_cb(void *v)
{
    struct setup_ctx *c = v;
    ip4_addr_t zero = {0};
    memset(&iface, 0, sizeof(iface));
    if (!netif_add(&iface, &zero, &zero, &zero, c->ax, init_interface, tcpip_input)) {
        c->err = ERR_IF;
        return;
    }
    netif_set_default(&iface);
    netif_set_up(&iface);
    if (c->link_up) netif_set_link_up(&iface);
    if (dhcp_start(&iface) != ERR_OK) {
        dhcp_cleanup(&iface);
        netif_remove(&iface);
        c->err = ERR_IF;
        return;
    }
    c->err = ERR_OK;
}

int ax_net_start(Ax88179 *ax)
{
    if (active || !ax) return -1;
    if (!initialized) {
        srand((unsigned)OSGetTime());
        for (int i = 0; i < TX_SLOTS; i++) atomic_store(&tx_in_use[i], 0);
        OSInitMessageQueue(&tx_queue, tx_storage, TX_SLOTS);
        tcpip_init(NULL, NULL);
        initialized = 1;
    }
    struct setup_ctx ctx = { .ax = ax, .link_up = 0, .err = ERR_ARG };
    int speed;
    if (ax88179_link(ax, &speed) == 1) ctx.link_up = 1;
    tcpip_callback_with_block(setup_cb, &ctx, 1);
    if (ctx.err != ERR_OK) return -1;
    active = 1;
    address[0] = 0;
    last_link = sys_now();
    last_link_up = ctx.link_up;
    link_errors = 0;
    return 0;
}

static void link_cb(void *v)
{
    if ((uintptr_t)v) netif_set_link_up(&iface);
    else netif_set_link_down(&iface);
}

static void drain_tx(Ax88179 *ax, int send)
{
    OSMessage m;
    while (OSReceiveMessage(&tx_queue, &m, OS_MESSAGE_FLAGS_NONE)) {
        int slot = (int)(uintptr_t)m.message;
        if (send) ax88179_send(ax, tx_pool[slot], m.args[0]);
        atomic_store(&tx_in_use[slot], 0);
    }
}

int ax_net_poll(void)
{
    if (!active) return -1;
    uint32_t now = sys_now();
    if ((uint32_t)(now - last_link) >= 500) {
        int speed, up = ax88179_link(iface.state, &speed);
        last_link = now;
        if (up >= 0 && up != last_link_up) {
            tcpip_callback_with_block(link_cb, (void *)(uintptr_t)up, 0);
            last_link_up = up;
        }
        if (up < 0) link_errors++;
        else link_errors = 0;
        if (link_errors >= 3) return -2;
    }
    drain_tx(iface.state, 1);
    /* One bounded receive; timers live in the tcpip thread now. */
    int n = ax88179_receive(iface.state, rx_frame, sizeof(rx_frame), 100);
    if (n > 0) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (p) {
            if (pbuf_take(p, rx_frame, (u16_t)n) != ERR_OK || iface.input(p, &iface) != ERR_OK)
                pbuf_free(p);
        }
    }
    if (n <= 0) OSSleepTicks(OSMillisecondsToTicks(5));
    return n;
}

const char *ax_net_address(void)
{
    if (!active || !netif_is_link_up(&iface) || !dhcp_supplied_address(&iface)) return NULL;
    return ip4addr_ntoa_r(netif_ip4_addr(&iface), address, sizeof(address));
}

static void stop_cb(void *v)
{
    (void)v;
    dhcp_release_and_stop(&iface);
    dhcp_cleanup(&iface);
    netif_set_down(&iface);
    netif_remove(&iface);
}

void ax_net_stop(void)
{
    if (!active) return;
    tcpip_callback_with_block(stop_cb, NULL, 1);
    /* Drop whatever the tcpip thread queued for TX before the adapter
     * handle goes away. */
    drain_tx(iface.state, 0);
    active = 0;
}
