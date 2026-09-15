#include "ax_net.h"
#include <string.h>
#include <stdlib.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <whb/log.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

static struct netif iface;
static uint8_t rx_frame[1600], tx_frame[1600];
static int initialized, active, link_errors;
static uint32_t last_link;
static char address[16];

u32_t sys_now(void)
{
    return (u32_t)OSTicksToMilliseconds(OSGetTime());
}

static err_t send_frame(struct netif *n, struct pbuf *p)
{
    if (p->tot_len > sizeof(tx_frame) ||
        pbuf_copy_partial(p, tx_frame, p->tot_len, 0) != p->tot_len) return ERR_BUF;
    return ax88179_send(n->state, tx_frame, p->tot_len) == 0 ? ERR_OK : ERR_IF;
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

int ax_net_start(Ax88179 *ax)
{
    if (active || !ax) return -1;
    if (!initialized) {
        srand((unsigned)OSGetTime());
        lwip_init();
        initialized = 1;
    }
    memset(&iface, 0, sizeof(iface));
    ip4_addr_t zero = {0};
    if (!netif_add(&iface, &zero, &zero, &zero, ax, init_interface, ethernet_input)) return -1;
    netif_set_default(&iface);
    netif_set_up(&iface);
    int speed;
    if (ax88179_link(ax, &speed) == 1) netif_set_link_up(&iface);
    if (dhcp_start(&iface) != ERR_OK) {
        dhcp_cleanup(&iface); netif_remove(&iface); return -1;
    }
    active = 1;
    address[0] = 0;
    last_link = sys_now();
    link_errors = 0;
    return 0;
}

int ax_net_poll(void)
{
    if (!active) return -1;
    uint32_t now = sys_now();
    if ((uint32_t)(now-last_link) >= 500) {
        int speed, up = ax88179_link(iface.state, &speed);
        if (up == 1) netif_set_link_up(&iface);
        else netif_set_link_down(&iface);
        last_link = now;
        if (up < 0) link_errors++;
        else link_errors = 0;
        if (link_errors >= 3) return -2;
    }
    /* One bounded receive: timers run even on an idle network. */
    int n = ax88179_receive(iface.state, rx_frame, sizeof(rx_frame), 100);
    if (n > 0) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (p) {
            if (pbuf_take(p, rx_frame, (u16_t)n) != ERR_OK || iface.input(p, &iface) != ERR_OK)
                pbuf_free(p);
        }
    }
    if (n <= 0) OSSleepTicks(OSMillisecondsToTicks(5));
    sys_check_timeouts();
    return n;
}

const char *ax_net_address(void)
{
    if (!active || !netif_is_link_up(&iface) || !dhcp_supplied_address(&iface)) return NULL;
    return ip4addr_ntoa_r(netif_ip4_addr(&iface), address, sizeof(address));
}

void ax_net_stop(void)
{
    if (!active) return;
    dhcp_release_and_stop(&iface);
    dhcp_cleanup(&iface);
    netif_set_down(&iface);
    netif_remove(&iface);
    active = 0;
}
