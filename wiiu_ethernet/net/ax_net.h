#ifndef AX_NET_H
#define AX_NET_H
#include "../driver/ax88179.h"
/* Single-threaded lwIP NO_SYS port: call poll regularly in the owner thread. */
int ax_net_start(Ax88179 *ax);
/* -2: repeated control failures, close and reopen the adapter. */
int ax_net_poll(void);
const char *ax_net_address(void);
void ax_net_stop(void);
#endif
