/*
 * nsysnet shim: replaces nsysnet.rpl socket/DNS exports with our lwIP
 * stack, so a title's traffic goes through the USB adapter without any
 * change on its side.
 *
 * Two ABI gaps are bridged here:
 *  - sockaddr: nsysnet has no sa_len byte and a 16-bit family; lwIP uses
 *    the BSD layout (8-bit len + 8-bit family). Port/address offsets
 *    coincide, only the first two bytes are translated.
 *  - constants: MSG_* flags, SOL_SOCKET (-1), SO_NBIO/SO_BIO/SO_NONBLOCK,
 *    EAI_* and h_errno values all differ between nsysnet and lwIP.
 *
 * Not covered (left to the original nsysnet, i.e. the IOSU/Wi-Fi stack):
 * sendto_multi(_ex), recvfrom_ex/_multi, getaddrinfo_async(_rs),
 * gethostbyaddr, netconf_*, NSSL (TLS needs a system fd), socket_lib_init/
 * finish (kept real so the untouched exports keep working).
 */
#include "nsysnet_shim.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <function_patcher/function_patching.h>
#include <whb/log.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "../net/ax_net.h"

/* ------------------------------------------------------------------ */
/* nsysnet ABI (from wut's headers, copied to avoid clashing with      */
/* lwIP's own socket headers)                                          */

struct nsn_fd_set { uint32_t fds_bits; };
struct nsn_timeval { long tv_sec, tv_usec; };
struct nsn_sockaddr { uint16_t sa_family; char sa_data[14]; };
struct nsn_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;   /* offsets 2/4 match lwIP's sockaddr_in */
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};
struct nsn_addrinfo {   /* wut order: canonname BEFORE addr (lwIP swaps) */
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    uint32_t ai_addrlen;
    char *ai_canonname;
    struct nsn_sockaddr *ai_addr;
    struct nsn_addrinfo *ai_next;
};

#define NSN_AF_INET      2
#define NSN_SOL_SOCKET   (-1)
#define NSN_SO_RXDATA    0x1011
#define NSN_SO_TXDATA    0x1012
#define NSN_SO_MYADDR    0x1013
#define NSN_SO_NBIO      0x1014
#define NSN_SO_BIO       0x1015
#define NSN_SO_NONBLOCK  0x1016

#define NSN_MSG_OOB       0x0001
#define NSN_MSG_PEEK      0x0002
#define NSN_MSG_DONTWAIT  0x0020

/* wut netdb values */
#define NSN_HOST_NOT_FOUND 1
#define NSN_TRY_AGAIN      2
#define NSN_NO_RECOVERY    3
#define NSN_NO_DATA        4
#define NSN_EAI_AGAIN      2
#define NSN_EAI_FAIL       4
#define NSN_EAI_FAMILY     5
#define NSN_EAI_MEMORY     6
#define NSN_EAI_NONAME     8
#define NSN_EAI_SERVICE    9
#define NSN_NI_NAMEREQD    0x0004

extern int h_errno;

static uint16_t nsn_ntohs(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static atomic_uint open_mask;

static void track_fd(int s)   { atomic_fetch_or(&open_mask, 1u << s); }
static void untrack_fd(int s) { atomic_fetch_and(&open_mask, ~(1u << s)); }

static int msg_flags_to_lwip(int f)
{
    int r = 0;
    if (f & NSN_MSG_PEEK)     r |= MSG_PEEK;
    if (f & NSN_MSG_DONTWAIT) r |= MSG_DONTWAIT;
    if (f & NSN_MSG_OOB)      r |= MSG_OOB;
    return r;
}

/* nsysnet sockaddr (no len byte, 16-bit family) -> lwIP sockaddr_in.
 * Returns bytes written, 0 if the family is unsupported. */
static socklen_t sockaddr_to_lwip(struct sockaddr_in *out, const struct nsn_sockaddr *in,
                                  socklen_t inlen)
{
    if (!in || inlen < 8 || in->sa_family != NSN_AF_INET) return 0;
    const struct nsn_sockaddr_in *n = (const struct nsn_sockaddr_in *)in;
    memset(out, 0, sizeof(*out));
    out->sin_len = sizeof(*out);
    out->sin_family = AF_INET;
    out->sin_port = n->sin_port;
    out->sin_addr.s_addr = n->sin_addr;
    return sizeof(*out);
}

static socklen_t sockaddr_to_nsn(struct nsn_sockaddr *out, socklen_t *outlen,
                                 const struct sockaddr *in, socklen_t cap)
{
    if (!out || !outlen || cap < 16) return 0;
    struct nsn_sockaddr_in *n = (struct nsn_sockaddr_in *)out;
    const struct sockaddr_in *l = (const struct sockaddr_in *)in;
    memset(n, 0, sizeof(*n));
    if (in->sa_family != AF_INET) return 0;
    n->sin_family = NSN_AF_INET;
    n->sin_port = l->sin_port;
    n->sin_addr = l->sin_addr.s_addr;
    *outlen = 16;
    return 16;
}

static int set_nonblocking(int s, int on)
{
    return lwip_ioctl(s, FIONBIO, &on);
}

/* ------------------------------------------------------------------ */
/* sockets                                                             */

DECL_FUNCTION(int, socket, int domain, int type, int protocol)
{
    if (!ax_net_stack_ready()) { errno = ENETDOWN; return -1; }
    int s = lwip_socket(domain, type, protocol);
    if (s >= 0 && s < 32) track_fd(s);
    return s;
}

DECL_FUNCTION(int, socketclose, int sockfd)
{
    int r = lwip_close(sockfd);
    if (r == 0 && sockfd < 32) untrack_fd(sockfd);
    return r;
}

DECL_FUNCTION(int, socketclose_all, void)
{
    uint32_t m = atomic_exchange(&open_mask, 0);
    for (int s = 0; s < 32; s++)
        if (m & (1u << s)) lwip_close(s);
    return 0;
}

DECL_FUNCTION(int, bind, int sockfd, const struct nsn_sockaddr *addr, socklen_t addrlen)
{
    struct sockaddr_in l;
    if (!sockaddr_to_lwip(&l, addr, addrlen)) { errno = EAFNOSUPPORT; return -1; }
    return lwip_bind(sockfd, (struct sockaddr *)&l, sizeof(l));
}

DECL_FUNCTION(int, connect, int sockfd, const struct nsn_sockaddr *addr, socklen_t addrlen)
{
    struct sockaddr_in l;
    if (!sockaddr_to_lwip(&l, addr, addrlen)) { errno = EAFNOSUPPORT; return -1; }
    return lwip_connect(sockfd, (struct sockaddr *)&l, sizeof(l));
}

DECL_FUNCTION(int, listen, int sockfd, int backlog)
{
    return lwip_listen(sockfd, backlog);
}

DECL_FUNCTION(int, accept, int sockfd, struct nsn_sockaddr *addr, socklen_t *addrlen)
{
    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    int s = lwip_accept(sockfd, addr ? (struct sockaddr *)&l : NULL, addr ? &llen : NULL);
    if (s >= 0) {
        if (s < 32) track_fd(s);
        if (addr) sockaddr_to_nsn(addr, addrlen, (struct sockaddr *)&l, *addrlen);
    }
    return s;
}

DECL_FUNCTION(int, shutdown, int sockfd, int how)
{
    return lwip_shutdown(sockfd, how);
}

DECL_FUNCTION(int, send, int sockfd, const void *buf, size_t len, int flags)
{
    return (int)lwip_send(sockfd, buf, len, msg_flags_to_lwip(flags));
}

DECL_FUNCTION(int, sendto, int sockfd, const void *buf, size_t len, int flags,
              const struct nsn_sockaddr *dest_addr, socklen_t addrlen)
{
    struct sockaddr_in l;
    if (!dest_addr)
        return (int)lwip_sendto(sockfd, buf, len, msg_flags_to_lwip(flags), NULL, 0);
    if (!sockaddr_to_lwip(&l, dest_addr, addrlen)) { errno = EAFNOSUPPORT; return -1; }
    return (int)lwip_sendto(sockfd, buf, len, msg_flags_to_lwip(flags),
                            (struct sockaddr *)&l, sizeof(l));
}

DECL_FUNCTION(int, recv, int sockfd, void *buf, size_t len, int flags)
{
    return (int)lwip_recv(sockfd, buf, len, msg_flags_to_lwip(flags));
}

DECL_FUNCTION(int, recvfrom, int sockfd, void *buf, size_t len, int flags,
              struct nsn_sockaddr *src_addr, socklen_t *addrlen)
{
    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    int r = (int)lwip_recvfrom(sockfd, buf, len, msg_flags_to_lwip(flags),
                               src_addr ? (struct sockaddr *)&l : NULL,
                               src_addr ? &llen : NULL);
    if (r >= 0 && src_addr) sockaddr_to_nsn(src_addr, addrlen, (struct sockaddr *)&l, *addrlen);
    return r;
}

DECL_FUNCTION(int, select, int nfds, struct nsn_fd_set *readfds, struct nsn_fd_set *writefds,
              struct nsn_fd_set *exceptfds, struct nsn_timeval *timeout)
{
    fd_set lr, lw, le;
    fd_set *pr = NULL, *pw = NULL, *pe = NULL;
    if (readfds)   { FD_ZERO(&lr); for (int i = 0; i < nfds && i < 32; i++) if (readfds->fds_bits & (1u << i)) FD_SET(i, &lr); pr = &lr; }
    if (writefds)  { FD_ZERO(&lw); for (int i = 0; i < nfds && i < 32; i++) if (writefds->fds_bits & (1u << i)) FD_SET(i, &lw); pw = &lw; }
    if (exceptfds) { FD_ZERO(&le); for (int i = 0; i < nfds && i < 32; i++) if (exceptfds->fds_bits & (1u << i)) FD_SET(i, &le); pe = &le; }
    struct timeval tv, *ptv = NULL;
    if (timeout) { tv.tv_sec = timeout->tv_sec; tv.tv_usec = timeout->tv_usec; ptv = &tv; }
    int rc = lwip_select(nfds, pr, pw, pe, ptv);
    if (rc > 0) {
        if (readfds)   { readfds->fds_bits = 0;   for (int i = 0; i < nfds && i < 32; i++) if (FD_ISSET(i, &lr)) readfds->fds_bits |= (1u << i); }
        if (writefds)  { writefds->fds_bits = 0;  for (int i = 0; i < nfds && i < 32; i++) if (FD_ISSET(i, &lw)) writefds->fds_bits |= (1u << i); }
        if (exceptfds) { exceptfds->fds_bits = 0; for (int i = 0; i < nfds && i < 32; i++) if (FD_ISSET(i, &le)) exceptfds->fds_bits |= (1u << i); }
    }
    return rc;
}

DECL_FUNCTION(int, getsockname, int sockfd, struct nsn_sockaddr *addr, socklen_t *addrlen)
{
    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    int r = lwip_getsockname(sockfd, (struct sockaddr *)&l, &llen);
    if (r == 0) sockaddr_to_nsn(addr, addrlen, (struct sockaddr *)&l, *addrlen);
    return r;
}

DECL_FUNCTION(int, getpeername, int sockfd, struct nsn_sockaddr *addr, socklen_t *addrlen)
{
    struct sockaddr_in l;
    socklen_t llen = sizeof(l);
    int r = lwip_getpeername(sockfd, (struct sockaddr *)&l, &llen);
    if (r == 0) sockaddr_to_nsn(addr, addrlen, (struct sockaddr *)&l, *addrlen);
    return r;
}

DECL_FUNCTION(int, setsockopt, int sockfd, int level, int optname,
              const void *optval, socklen_t optlen)
{
    if (level == NSN_SOL_SOCKET) {
        switch (optname) {
        case NSN_SO_NBIO:     return set_nonblocking(sockfd, 1);
        case NSN_SO_BIO:      return set_nonblocking(sockfd, 0);
        case NSN_SO_NONBLOCK: return set_nonblocking(sockfd, optval && *(const int *)optval);
        case NSN_SO_RXDATA:
        case NSN_SO_TXDATA:
        case NSN_SO_MYADDR:   errno = ENOPROTOOPT; return -1; /* get-only */
        default:              return lwip_setsockopt(sockfd, SOL_SOCKET, optname, optval, optlen);
        }
    }
    return lwip_setsockopt(sockfd, level, optname, optval, optlen);
}

DECL_FUNCTION(int, getsockopt, int sockfd, int level, int optname,
              void *optval, socklen_t *optlen)
{
    if (level == NSN_SOL_SOCKET) {
        switch (optname) {
        case NSN_SO_NBIO:
        case NSN_SO_NONBLOCK: {
            if (!optval || !optlen || *optlen < 4) { errno = EINVAL; return -1; }
            int fl = lwip_fcntl(sockfd, F_GETFL, 0);
            if (fl < 0) return -1;
            *(int *)optval = !!(fl & O_NONBLOCK);
            *optlen = 4;
            return 0;
        }
        case NSN_SO_RXDATA: {
            if (!optval || !optlen || *optlen < 4) { errno = EINVAL; return -1; }
            int v = 0;
            int r = lwip_ioctl(sockfd, FIONREAD, &v);
            if (r == 0) { *(int *)optval = v; *optlen = 4; }
            return r;
        }
        case NSN_SO_MYADDR: {
            if (!optval || !optlen || *optlen < 4) { errno = EINVAL; return -1; }
            *(uint32_t *)optval = ax_net_ip4();
            *optlen = 4;
            return 0;
        }
        case NSN_SO_BIO:
        case NSN_SO_TXDATA: errno = ENOPROTOOPT; return -1;
        default:            return lwip_getsockopt(sockfd, SOL_SOCKET, optname, optval, optlen);
        }
    }
    return lwip_getsockopt(sockfd, level, optname, optval, optlen);
}

DECL_FUNCTION(int, socketlasterr, void)
{
    return errno;
}

/* ------------------------------------------------------------------ */
/* DNS                                                                 */

DECL_FUNCTION(struct hostent *, gethostbyname, const char *name)
{
    if (!ax_net_stack_ready()) { h_errno = NSN_HOST_NOT_FOUND; return NULL; }
    struct hostent *h = lwip_gethostbyname(name);
    if (!h) {
        switch (h_errno) { /* lwIP -> nsysnet h_errno values */
        case 210: h_errno = NSN_HOST_NOT_FOUND; break;
        case 213: h_errno = NSN_TRY_AGAIN;      break;
        case 212: h_errno = NSN_NO_RECOVERY;    break;
        case 211: h_errno = NSN_NO_DATA;        break;
        }
    }
    return h;
}

static int eai_to_nsn(int e)
{
    switch (e) { /* lwIP -> nsysnet EAI values */
    case 200: return NSN_EAI_NONAME;
    case 201: return NSN_EAI_SERVICE;
    case 203: return NSN_EAI_MEMORY;
    case 204: return NSN_EAI_FAMILY;
    default:  return NSN_EAI_FAIL;
    }
}

static int ai_flags_to_lwip(int f)
{
    int r = 0;
    if (f & 0x01) r |= 0x01; /* PASSIVE */
    if (f & 0x02) r |= 0x02; /* CANONNAME */
    if (f & 0x04) r |= 0x04; /* NUMERICHOST */
    if (f & 0x08) r |= 0x10; /* V4MAPPED (value differs) */
    if (f & 0x10) r |= 0x20; /* ALL */
    if (f & 0x20) r |= 0x40; /* ADDRCONFIG */
    return r;
}

DECL_FUNCTION(int, getaddrinfo, const char *node, const char *service,
              const struct nsn_addrinfo *hints, struct nsn_addrinfo **res)
{
    if (!ax_net_stack_ready()) return NSN_EAI_AGAIN;
    if (!res) return NSN_EAI_FAIL;
    *res = NULL;

    struct addrinfo lh, *lph = NULL;
    if (hints) {
        memset(&lh, 0, sizeof(lh));
        lh.ai_flags = ai_flags_to_lwip(hints->ai_flags);
        lh.ai_family = hints->ai_family;
        lh.ai_socktype = hints->ai_socktype;
        lh.ai_protocol = hints->ai_protocol;
        lph = &lh;
    }
    struct addrinfo *lres = NULL;
    int err = lwip_getaddrinfo(node, service, lph, &lres);
    if (err != 0) return eai_to_nsn(err);

    /* Convert the list: wut's addrinfo swaps ai_addr/ai_canonname and its
     * sockaddr has no length byte. One allocation per node. */
    struct nsn_addrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *la = lres; la; la = la->ai_next) {
        size_t canonlen = la->ai_canonname ? strlen(la->ai_canonname) + 1 : 0;
        struct nsn_addrinfo *na = malloc(sizeof(*na) + sizeof(struct nsn_sockaddr_in) + canonlen);
        if (!na) { err = NSN_EAI_MEMORY; goto fail; }
        na->ai_flags = la->ai_flags;
        na->ai_family = la->ai_family;
        na->ai_socktype = la->ai_socktype;
        na->ai_protocol = la->ai_protocol;
        na->ai_next = NULL;
        char *storage = (char *)(na + 1);
        na->ai_addr = (struct nsn_sockaddr *)storage;
        sockaddr_to_nsn(na->ai_addr, &na->ai_addrlen, la->ai_addr, 16);
        storage += sizeof(struct nsn_sockaddr_in);
        if (canonlen) {
            memcpy(storage, la->ai_canonname, canonlen);
            na->ai_canonname = storage;
        } else {
            na->ai_canonname = NULL;
        }
        *tail = na;
        tail = &na->ai_next;
    }
    lwip_freeaddrinfo(lres);
    *res = head;
    return 0;

fail:
    lwip_freeaddrinfo(lres);
    while (head) {
        struct nsn_addrinfo *next = head->ai_next;
        free(head);
        head = next;
    }
    return err;
}

DECL_FUNCTION(void, freeaddrinfo, struct nsn_addrinfo *res)
{
    while (res) {
        struct nsn_addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

DECL_FUNCTION(int, getnameinfo, const struct nsn_sockaddr *addr, socklen_t addrlen,
              char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
    (void)addrlen;
    if (!addr || addr->sa_family != NSN_AF_INET) return NSN_EAI_FAMILY;
    const struct nsn_sockaddr_in *in = (const struct nsn_sockaddr_in *)addr;
    /* Numeric only: lwIP has no reverse lookup. */
    if (flags & NSN_NI_NAMEREQD) return NSN_EAI_NONAME;
    if (host) {
        const uint8_t *b = (const uint8_t *)&in->sin_addr;
        int n = snprintf(host, hostlen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        if (n < 0 || (socklen_t)n >= hostlen) return 12 /* NSN_EAI_BADHINTS/overflow */;
    }
    if (serv) {
        int n = snprintf(serv, servlen, "%u", nsn_ntohs(in->sin_port));
        if (n < 0 || (socklen_t)n >= servlen) return 12;
    }
    return 0;
}

DECL_FUNCTION(int *, get_h_errno, void)
{
    return &h_errno;
}

DECL_FUNCTION(const char *, gai_strerror, int ecode)
{
    switch (ecode) {
    case 0:  return "success";
    case 1:  return "address family not supported";
    case 2:  return "temporary failure in name resolution";
    case 3:  return "invalid flags";
    case 4:  return "non-recoverable failure in name resolution";
    case 5:  return "address family not supported";
    case 6:  return "out of memory";
    case 7:  return "no address associated with name";
    case 8:  return "name or service not known";
    case 9:  return "service not supported for socket type";
    case 10: return "socket type not supported";
    case 11: return "system error";
    case 12: return "invalid hints";
    case 13: return "unsupported protocol";
    case 14: return "buffer overflow";
    default: return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* registration                                                        */

static PatchedFunctionHandle handles[2 * 24];
static int handle_count;

static void add_patch(function_replacement_data_t *data, const char *name, int process)
{
    PatchedFunctionHandle h = 0;
    bool patched = false;
    FunctionPatcherStatus st = FunctionPatcher_AddFunctionPatch(data, &h, &patched);
    if (st == FUNCTION_PATCHER_RESULT_SUCCESS) {
        if (handle_count < (int)(sizeof(handles) / sizeof(handles[0])))
            handles[handle_count++] = h;
        WHBLogPrintf("AX88179 shim: %s %s for process %d (patched now: %s)",
                     patched ? "patched" : "registered", name, process,
                     patched ? "yes" : "waiting for nsysnet load");
    } else {
        WHBLogPrintf("AX88179 shim: FAILED to add patch %s (proc %d): %s",
                     name, process, FunctionPatcher_GetStatusStr(st));
    }
}

#define SHIM_PATCH(name)                                                          \
    do {                                                                          \
        function_replacement_data_t d1 = REPLACE_FUNCTION_FOR_PROCESS(            \
            name, LIBRARY_NSYSNET, name, FP_TARGET_PROCESS_GAME_AND_MENU);        \
        add_patch(&d1, #name, FP_TARGET_PROCESS_GAME_AND_MENU);                   \
        function_replacement_data_t d2 = REPLACE_FUNCTION_FOR_PROCESS(            \
            name, LIBRARY_NSYSNET, name, FP_TARGET_PROCESS_ROOT_RPX);             \
        add_patch(&d2, #name, FP_TARGET_PROCESS_ROOT_RPX);                        \
    } while (0)

static int installed;

int nsysnet_shim_install(void)
{
    if (installed) return 0;
    FunctionPatcherStatus st = FunctionPatcher_InitLibrary();
    if (st != FUNCTION_PATCHER_RESULT_SUCCESS) {
        WHBLogPrintf("AX88179 shim: function patcher unavailable (%s) -- "
                     "titles keep using the console network", FunctionPatcher_GetStatusStr(st));
        return -1;
    }
    uint32_t version = 0;
    FunctionPatcher_GetVersion(&version);
    WHBLogPrintf("AX88179 shim: function patcher API v%u, registering nsysnet hooks", version);

    SHIM_PATCH(socket);
    SHIM_PATCH(socketclose);
    SHIM_PATCH(socketclose_all);
    SHIM_PATCH(bind);
    SHIM_PATCH(connect);
    SHIM_PATCH(listen);
    SHIM_PATCH(accept);
    SHIM_PATCH(shutdown);
    SHIM_PATCH(send);
    SHIM_PATCH(sendto);
    SHIM_PATCH(recv);
    SHIM_PATCH(recvfrom);
    SHIM_PATCH(select);
    SHIM_PATCH(setsockopt);
    SHIM_PATCH(getsockopt);
    SHIM_PATCH(getsockname);
    SHIM_PATCH(getpeername);
    SHIM_PATCH(socketlasterr);
    SHIM_PATCH(gethostbyname);
    SHIM_PATCH(getaddrinfo);
    SHIM_PATCH(freeaddrinfo);
    SHIM_PATCH(getnameinfo);
    SHIM_PATCH(get_h_errno);
    SHIM_PATCH(gai_strerror);

    installed = 1;
    WHBLogPrintf("AX88179 shim: %d patches registered", handle_count);
    return 0;
}

/*
 * Called when the title hosting this module instance exits. The
 * replacement functions live in this process's memory: leaving the
 * patches registered would let them fire on dead code after finalize,
 * and the patcher's list would grow with every title switch. The next
 * title's module instance re-registers from its own APPLICATION_STARTS.
 */
void nsysnet_shim_uninstall(void)
{
    if (!installed) return;
    for (int i = handle_count - 1; i >= 0; i--)
        FunctionPatcher_RemoveFunctionPatch(handles[i]);
    handle_count = 0;
    installed = 0;
    FunctionPatcher_DeInitLibrary();
}
