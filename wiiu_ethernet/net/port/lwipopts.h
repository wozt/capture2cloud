#ifndef AX_LWIP_OPTS_H
#define AX_LWIP_OPTS_H
/* Threaded stack (NO_SYS=0): a tcpip thread owns the core; the driver
 * worker only moves frames in/out and everything else goes through
 * tcpip callbacks or the socket/netconn API. */
#define NO_SYS 0
#define SYS_LIGHTWEIGHT_PROT 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE (256 * 1024)
#define PBUF_POOL_SIZE 48
#define PBUF_POOL_BUFSIZE 1600
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_DHCP 1
#define DHCP_DOES_ARP_CHECK 1
#define LWIP_AUTOIP 0
#define LWIP_UDP 1
#define LWIP_TCP 1
#define TCP_MSS 1460
#define TCP_WND (16 * TCP_MSS)
#define TCP_SND_BUF (16 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define MEMP_NUM_TCP_PCB 8
#define MEMP_NUM_UDP_PCB 8
#define MEMP_NUM_TCP_SEG 64
#define MEMP_NUM_NETCONN 16
#define MEMP_NUM_TCPIP_MSG_INPKT 16
#define LWIP_DNS 1
#define LWIP_NETCONN 1
#define LWIP_SOCKET 1
#define LWIP_TCPIP_CORE_LOCKING 0
#define TCPIP_MBOX_SIZE 16
#define TCPIP_THREAD_STACKSIZE (32 * 1024)
#define TCPIP_THREAD_PRIO 1
#define DEFAULT_THREAD_STACKSIZE (16 * 1024)
#define DEFAULT_THREAD_PRIO 2
#define DEFAULT_TCP_RECVMBOX_SIZE 16
#define DEFAULT_UDP_RECVMBOX_SIZE 16
#define DEFAULT_ACCEPTMBOX_SIZE 8
#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_SNDTIMEO 1
#define SO_REUSE 1
#define LWIP_TCP_KEEPALIVE 1
/* newlib (pulled in by wut headers) already defines struct timeval. */
#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define IP_REASSEMBLY 1
#define IP_FRAG 1
#define LWIP_STATS 1
#define LWIP_NETIF_LOOPBACK 0
#endif
