#ifndef AX_NET_H
#define AX_NET_H
#include <stdint.h>
#include "../driver/ax88179.h"
/* Threaded lwIP port: start spawns the tcpip thread; poll moves frames
 * between the adapter and the stack and must be called regularly from
 * the owner thread. */
int ax_net_start(Ax88179 *ax);
/* -2: repeated control failures, close and reopen the adapter. */
int ax_net_poll(void);
const char *ax_net_address(void);
void ax_net_stop(void);
/* Non-zero once the tcpip thread exists and lwip_* calls are safe. */
int ax_net_stack_ready(void);
/* Our IPv4 address, network byte order, 0 when down. */
uint32_t ax_net_ip4(void);
#endif
