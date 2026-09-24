/* nx_socket.c -- bionic-ABI BSD sockets and name resolution over libnx.
 *
 * libnative.so was built against bionic (Linux numbering); libnx sockets are
 * FreeBSD-flavoured with newlib errno values. Every crossing is converted:
 *
 *   sockaddr_in   bionic {u16 family; u16 port; u32 addr; zero[8]}
 *                 BSD    {u8 len; u8 family; u16 port; u32 addr; zero[8]}
 *   addrinfo      same member order, but ai_addr must be rebuilt (IPv4 only)
 *   msghdr        bionic uses size_t iovlen/controllen; libnx int/socklen_t.
 *                 sendmsg/recvmsg are done over send/recv (libnx routes its
 *                 own sendmsg through sendmmsg, a newer-firmware IPC)
 *   SOL_SOCKET    1 vs 0xffff, and every SO_* differs
 *   MSG_*         DONTWAIT 0x40 vs 0x80; bionic MSG_NOSIGNAL 0x4000 is libnx
 *                 MSG_NBIO -- it is dropped, never forwarded
 *   O_NONBLOCK    0x800 vs newlib 0x4000; libnx F_SETFL accepts ONLY that bit
 *   FIONBIO/READ  0x5421/0x541B vs _IOW/_IOR encodings
 *   errno         newlib -> Linux via a table generated from libnx's own
 *                 convert_errno.c (tools/gen_errno.py). EXCEPT SO_ERROR: the
 *                 bsd service reports that raw, and it already uses Linux
 *                 numbers, so it passes through untouched.
 *
 * IPv4 only. The engine takes every socket's family from resolver results
 * (disassembly of the Asio call sites: `cmp w8,#2 ... mov w8,#0xa`), so an
 * IPv4-only resolver keeps the whole stack on IPv4.
 *
 * Edge-triggered epoll bookkeeping: every EAGAIN / short transfer re-arms the
 * fd (nxf_arm), which is what lets nx_fdemu.c emulate EPOLLET for Asio.
 *
 * MIT license -- see LICENSE.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nx_net.h"
#include "nx_net_internal.h"

#ifdef NXNET_HOST
int     __real_socket(int, int, int);
int     __real_connect(int, const struct sockaddr *, socklen_t);
int     __real_bind(int, const struct sockaddr *, socklen_t);
int     __real_listen(int, int);
int     __real_accept(int, struct sockaddr *, socklen_t *);
int     __real_shutdown(int, int);
ssize_t __real_send(int, const void *, size_t, int);
ssize_t __real_recv(int, void *, size_t, int);
ssize_t __real_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t __real_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
int     __real_setsockopt(int, int, int, const void *, socklen_t);
int     __real_getsockopt(int, int, int, void *, socklen_t *);
int     __real_getsockname(int, struct sockaddr *, socklen_t *);
int     __real_getpeername(int, struct sockaddr *, socklen_t *);
int     __real_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void    __real_freeaddrinfo(struct addrinfo *);
int     __real_fcntl(int, int, ...);
int     __real_ioctl(int, unsigned long, ...);
ssize_t __real_read(int, void *, size_t);
ssize_t __real_write(int, const void *, size_t);
int     __real_close(int);
#endif

/* ------------------------------------------------------------------ */
/* guest structs and constants                                         */
/* ------------------------------------------------------------------ */
typedef struct { uint16_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; } GSockaddrIn;
typedef struct { void *base; size_t len; } GIovec;
typedef struct {
  void *name; uint32_t namelen; GIovec *iov; size_t iovlen;
  void *control; size_t controllen; int flags;
} GMsghdr;
typedef struct GAddrInfo {
  int flags, family, socktype, protocol;
  uint32_t addrlen;
  char *canonname;
  GSockaddrIn *addr;
  struct GAddrInfo *next;
} GAddrInfo;
typedef struct { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; } GHostent;

_Static_assert(sizeof(GSockaddrIn) == 16, "bionic sockaddr_in is 16 bytes");
_Static_assert(offsetof(GMsghdr, iov) == 16 && offsetof(GMsghdr, flags) == 48, "bionic arm64 msghdr");
_Static_assert(offsetof(GAddrInfo, canonname) == 24 && offsetof(GAddrInfo, addr) == 32 &&
               offsetof(GAddrInfo, next) == 40, "bionic arm64 addrinfo");
_Static_assert(offsetof(GHostent, h_addr_list) == 24, "bionic arm64 hostent");

#define G_AF_UNSPEC  0
#define G_AF_INET    2
#define G_AF_INET6   10
#define G_SOL_SOCKET 1
#define G_IPPROTO_IP 0
#define G_IPPROTO_TCP 6
#define G_IPPROTO_UDP 17
#define G_SOCK_TYPE_MASK 0xf

#define G_MSG_OOB       0x1
#define G_MSG_PEEK      0x2
#define G_MSG_DONTROUTE 0x4
#define G_MSG_TRUNC     0x20
#define G_MSG_DONTWAIT  0x40
#define G_MSG_EOR       0x80
#define G_MSG_WAITALL   0x100

#define G_SO_ERROR 4

#define G_AI_PASSIVE     0x1
#define G_AI_CANONNAME   0x2
#define G_AI_NUMERICHOST 0x4
#define G_AI_NUMERICSERV 0x8
#define G_EAI_AGAIN   2
#define G_EAI_BADFLAGS 3
#define G_EAI_FAIL    4
#define G_EAI_FAMILY  5
#define G_EAI_MEMORY  6
#define G_EAI_NONAME  8
#define G_EAI_SERVICE 9
#define G_EAI_SOCKTYPE 10
#define G_EAI_SYSTEM  11

/* ------------------------------------------------------------------ */
/* errno                                                               */
/* ------------------------------------------------------------------ */
#if defined(__SWITCH__)
#include "nx_errno_table.inc"
int nxs_guest_errno(int e) {
  if (e >= 10000) return e - 10000;       /* libnx: unmapped bsd errno + 10000 */
  if (e > 0 && e < (int)sizeof k_newlib_to_linux && k_newlib_to_linux[e]) return k_newlib_to_linux[e];
  return e;
}
#else
int nxs_guest_errno(int e) { return e; }  /* host test: already Linux */
#endif

static inline int host_again(int e) {
  return e == EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
      || e == EWOULDBLOCK
#endif
      ;
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static volatile int g_fail_logs, g_trace_logs;
#define NET_FAIL(...) do { if (g_fail_logs < 48) { g_fail_logs++; nxn_log(__VA_ARGS__); } } while (0)
#define NET_TRACE(...) do { if (nx_net_trace() && g_trace_logs < 600) { g_trace_logs++; nxn_log(__VA_ARGS__); } } while (0)

static const char *ip4(uint32_t be, char *buf) {
  const uint8_t *o = (const uint8_t *)&be;
  static const char digits[] = "0123456789";
  char *p = buf;
  for (int i = 0; i < 4; i++) {
    unsigned v = o[i];
    if (v >= 100) *p++ = digits[v / 100];
    if (v >= 10)  *p++ = digits[(v / 10) % 10];
    *p++ = digits[v % 10];
    if (i < 3) *p++ = '.';
  }
  *p = 0;
  return buf;
}

/* ------------------------------------------------------------------ */
/* conversions                                                         */
/* ------------------------------------------------------------------ */
static int g2h_addr(const void *ga, unsigned glen, struct sockaddr_in *h) {
  if (!ga || glen < 2) { errno = G_EINVAL; return 0; }
  uint16_t fam;
  memcpy(&fam, ga, sizeof fam);
  if (fam != G_AF_INET) { errno = G_EAFNOSUPPORT; return 0; }
  if (glen < sizeof(GSockaddrIn)) { errno = G_EINVAL; return 0; }
  const GSockaddrIn *g = (const GSockaddrIn *)ga;
  memset(h, 0, sizeof *h);
#if defined(__SWITCH__)
  h->sin_len = (uint8_t)sizeof *h;
#endif
  h->sin_family = AF_INET;
  h->sin_port = g->port;
  h->sin_addr.s_addr = g->addr;
  return 1;
}

static void h2g_addr(const struct sockaddr_in *h, void *ga, void *glenp) {
  if (!ga || !glenp) return;
  GSockaddrIn g;
  memset(&g, 0, sizeof g);
  g.family = G_AF_INET;
  g.port = h->sin_port;
  g.addr = h->sin_addr.s_addr;
  const unsigned cap = *(unsigned *)glenp;
  memcpy(ga, &g, cap < sizeof g ? cap : sizeof g);
  *(unsigned *)glenp = (unsigned)sizeof g;
}

static int msg_to_host(int f) {
  int h = 0;
  if (f & G_MSG_OOB)       h |= MSG_OOB;
  if (f & G_MSG_PEEK)      h |= MSG_PEEK;
  if (f & G_MSG_DONTROUTE) h |= MSG_DONTROUTE;
  if (f & G_MSG_TRUNC)     h |= MSG_TRUNC;
  if (f & G_MSG_DONTWAIT)  h |= MSG_DONTWAIT;
  if (f & G_MSG_EOR)       h |= MSG_EOR;
  if (f & G_MSG_WAITALL)   h |= MSG_WAITALL;
  return h;   /* MSG_NOSIGNAL / MSG_MORE / MSG_CONFIRM ...: no host equivalent */
}

static int so_to_host(int n) {
  switch (n) {
    case 1:  return SO_DEBUG;
    case 2:  return SO_REUSEADDR;
    case 3:  return SO_TYPE;
    case 4:  return SO_ERROR;
    case 5:  return SO_DONTROUTE;
    case 6:  return SO_BROADCAST;
    case 7:  return SO_SNDBUF;
    case 8:  return SO_RCVBUF;
    case 9:  return SO_KEEPALIVE;
    case 10: return SO_OOBINLINE;
    case 13: return SO_LINGER;
#ifdef SO_REUSEPORT
    case 15: return SO_REUSEPORT;
#endif
    case 18: return SO_RCVLOWAT;
    case 19: return SO_SNDLOWAT;
    case 20: return SO_RCVTIMEO;
    case 21: return SO_SNDTIMEO;
    case 30: return SO_ACCEPTCONN;
    default: return -1;            /* SO_PRIORITY, SO_PASSCRED, SO_PEERCRED ... */
  }
}

static int tcp_to_host(int n) {
  switch (n) {
    case 1: return TCP_NODELAY;
#ifdef TCP_MAXSEG
    case 2: return TCP_MAXSEG;
#endif
#ifdef TCP_KEEPIDLE
    case 4: return TCP_KEEPIDLE;   /* libnx 0x100 */
#endif
#ifdef TCP_KEEPINTVL
    case 5: return TCP_KEEPINTVL;  /* libnx 0x200 */
#endif
#ifdef TCP_KEEPCNT
    case 6: return TCP_KEEPCNT;    /* libnx 0x400 */
#endif
    default: return -1;
  }
}

/* The descriptor readiness is polled on (see NXN_POLL in nx_net_internal.h).
 * On Switch: the bsd-service fd behind the newlib handle, looked up exactly as
 * libnx's own _socketGetFd does. The caller holds the fd-table lock, so the
 * handle cannot be released underneath the lookup. */
#if defined(__SWITCH__)
#include <sys/iosupport.h>
static int host_poll_fd(int fd) {
  __handle *h = __get_handle(fd);
  if (!h || !devoptab_list[h->device] || strcmp(devoptab_list[h->device]->name, "soc") != 0) return -1;
  return *(int *)h->fileStruct;
}
#else
static int host_poll_fd(int fd) { return fd; }
#endif

static int host_set_nb(int fd, int on) {
  /* libnx F_SETFL accepts O_NONBLOCK or 0 and nothing else */
  return NXN_NATIVE(fcntl)(fd, F_SETFL, on ? O_NONBLOCK : 0);
}

static uint32_t maybe_broadcast(uint32_t addr_be) {
  /* 255.255.255.255 is not routed by the Switch stack; the subnet form is
   * (revoltnx's LAN discovery finding). */
  if (addr_be == 0xffffffffu) {
    const uint32_t b = nx_net_subnet_broadcast();
    if (b) return b;
  }
  return addr_be;
}

static int is_loopback(uint32_t addr_be) { return (ntohl(addr_be) >> 24) == 127; }

/* online=0: nothing leaves the console. */
static int may_reach(const struct sockaddr_in *h) {
  if (nx_net_enabled() || is_loopback(h->sin_addr.s_addr)) return 1;
  errno = G_ENETUNREACH;
  return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */
int nxs_socket(int domain, int type, int protocol) {
  /* online=0 does NOT refuse sockets: Asio's throwing constructors would turn
   * that into an exception at object creation. Traffic is refused instead
   * (DNS, and connect/sendto to anything but loopback) -- see may_reach(). */
  if (!nx_net_sockets_ok()) { errno = G_ENETDOWN; return -1; }   /* service never came up */
  if (domain != G_AF_INET) {
    NET_TRACE("[net] socket(domain %d) -> EAFNOSUPPORT (IPv4 only)\n", domain);
    errno = G_EAFNOSUPPORT;
    return -1;
  }
  const int base = type & G_SOCK_TYPE_MASK;
  const int nb = (type & G_O_NONBLOCK) != 0;           /* SOCK_NONBLOCK == O_NONBLOCK */
  if (base != G_SOCK_STREAM && base != G_SOCK_DGRAM) { errno = G_EPROTONOSUPPORT; return -1; }
  if (protocol != 0 && protocol != G_IPPROTO_TCP && protocol != G_IPPROTO_UDP) { errno = G_EPROTONOSUPPORT; return -1; }

  shim_fdtable_lock();                                  /* newlib handle alloc */
  const int fd = NXN_NATIVE(socket)(AF_INET, base == G_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, protocol);
  const int e = errno;
  const int hfd = fd >= 0 ? host_poll_fd(fd) : -1;
  shim_fdtable_unlock();
  if (fd < 0) {
    NET_FAIL("[net] socket(type %d) FAILED errno %d -- out of sockets / bsd sessions?\n", base, e);
    errno = nxs_guest_errno(e);
    return -1;
  }
  if (nxf_sock_track(fd, base, nb, hfd) != 0) {
    NET_FAIL("[net] socket fd %d not trackable (table / bsd fd %d) -- refused\n", fd, hfd);
    shim_fdtable_lock(); NXN_NATIVE(close)(fd); shim_fdtable_unlock();
    errno = G_EMFILE;
    return -1;
  }
  if (nb) host_set_nb(fd, 1);
  if (base == G_SOCK_DGRAM) {
    const int on = 1;
    (void)NXN_NATIVE(setsockopt)(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
  }
  NET_TRACE("[net] socket(%s%s) -> %d\n", base == G_SOCK_STREAM ? "tcp" : "udp", nb ? ",nb" : "", fd);
  return fd;
}

int nxs_socketpair(int domain, int type, int protocol, int sv[2]) {
  (void)domain; (void)protocol;
  return nxf_socketpair(sv, (type & G_O_NONBLOCK) != 0);
}

/* Called by close_fake() before newlib's close. */
int nxs_close_hook(int fd, int *ret) {
  if (nxf_is_fake(fd)) { *ret = nxf_close(fd); return 1; }
  if (nxf_sock_tracked(fd)) {
    /* A thread blocked in recv()/poll() on this socket is not woken by a plain
     * close on Horizon (nor on Linux) and would pin a bsd session forever.
     * Shutting the connection down first wakes it with EOF. */
    if (nxf_sock_type(fd) == G_SOCK_STREAM && nxf_sock_connected(fd))
      (void)NXN_NATIVE(shutdown)(fd, SHUT_RDWR);
    nxf_sock_forget(fd);
    NET_TRACE("[net] close(%d)\n", fd);
  }
  return 0;
}

int nxs_bind(int fd, const void *addr, unsigned addrlen) {
  struct sockaddr_in h;
  if (!g2h_addr(addr, addrlen, &h)) return -1;
  if (NXN_NATIVE(bind)(fd, (const struct sockaddr *)&h, sizeof h) < 0) {
    const int e = errno;
    char b[16];
    NET_FAIL("[net] bind(%d, %s:%u) FAILED errno %d\n", fd, ip4(h.sin_addr.s_addr, b), ntohs(h.sin_port), e);
    errno = nxs_guest_errno(e);
    return -1;
  }
  return 0;
}

int nxs_listen(int fd, int backlog) {
  if (NXN_NATIVE(listen)(fd, backlog) < 0) { errno = nxs_guest_errno(errno); return -1; }
  return 0;
}

int nxs_shutdown(int fd, int how) {
  if (nxf_is_fake(fd)) return 0;
  if (NXN_NATIVE(shutdown)(fd, how) < 0) { errno = nxs_guest_errno(errno); return -1; }
  return 0;
}

static int wait_host(int fd, short ev, int timeout_ms) {
  /* bounded poll on one socket; gives up if the fd is closed meanwhile */
  const uint64_t start = nxn_now_ms();
  for (;;) {
    int slice = 500;
    if (timeout_ms >= 0) {
      const int left = timeout_ms - (int)(nxn_now_ms() - start);
      if (left <= 0) return 0;
      if (left < slice) slice = left;
    }
    const int hfd = nxf_sock_hostfd(fd);
    if (hfd < 0) { errno = EBADF; return -1; }
    struct pollfd p = { hfd, ev, 0 };
    const int r = NXN_POLL(&p, 1, slice);
    if (r > 0 && (p.revents & POLLNVAL)) { errno = EBADF; return -1; }
    if (r != 0) return r;
    if (!nxf_sock_tracked(fd)) { errno = EBADF; return -1; }
  }
}

int nxs_connect(int fd, const void *addr, unsigned addrlen) {
  if (nxf_is_fake(fd)) { errno = G_EISCONN; return -1; }
  struct sockaddr_in h;
  if (!g2h_addr(addr, addrlen, &h)) return -1;
  char b[16];
  if (!may_reach(&h)) return -1;
  if (nxf_sock_type(fd) == G_SOCK_DGRAM) h.sin_addr.s_addr = maybe_broadcast(h.sin_addr.s_addr);

  const int nb = nxf_sock_nonblock(fd);
  const int bounded = nb && nxf_sock_type(fd) == G_SOCK_STREAM && nx_net_connect_timeout_ms() > 0;

  int rc = NXN_NATIVE(connect)(fd, (const struct sockaddr *)&h, sizeof h);
  int e = rc < 0 ? errno : 0;

  /* EINPROGRESS is the BSD answer for a non-blocking connect. EAGAIN is taken
   * the same way defensively (a Linux stack can say it; reporting "in
   * progress" lets the caller learn the outcome from SO_ERROR). EALREADY --
   * a second connect() while one is pending -- falls through and is reported
   * as itself. */
  if (rc < 0 && (e == EINPROGRESS || host_again(e))) {
    if (bounded) {
      /* Fallback mode (s_connect_timeout > 0 in nx_net.c): finish the handshake here
       * and only ever report success or a final error. */
      const int r = wait_host(fd, POLLOUT, nx_net_connect_timeout_ms());
      if (r > 0) {
        int soerr = 0; socklen_t sl = sizeof soerr;
        if (NXN_NATIVE(getsockopt)(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) {
          errno = nxs_guest_errno(errno); return -1;
        }
        if (soerr) {
          NET_FAIL("[net] connect %s:%u FAILED (SO_ERROR %d)\n", ip4(h.sin_addr.s_addr, b), ntohs(h.sin_port), soerr);
          errno = soerr;                                    /* already Linux numbering */
          return -1;
        }
        rc = 0;
      } else {
        NET_FAIL("[net] connect %s:%u TIMED OUT after %d ms\n", ip4(h.sin_addr.s_addr, b), ntohs(h.sin_port),
                 nx_net_connect_timeout_ms());
        errno = r == 0 ? G_ETIMEDOUT : nxs_guest_errno(errno);
        return -1;
      }
    } else {
      /* True non-blocking semantics: Asio's reactor / curl wait for writability
       * and read SO_ERROR. Arm OUT so the reactor reports the completion. */
      nxf_arm(fd, ARM_OUT | ARM_ERR | ARM_HUP);
      NET_TRACE("[net] connect(%d, %s:%u) in progress\n", fd, ip4(h.sin_addr.s_addr, b), ntohs(h.sin_port));
      errno = G_EINPROGRESS;
      nxf_sock_set_connected(fd, 1);                        /* for shutdown-on-close */
      return -1;
    }
  }
  if (rc < 0) {
    NET_FAIL("[net] connect %s:%u FAILED errno %d\n", ip4(h.sin_addr.s_addr, b), ntohs(h.sin_port), e);
    errno = nxs_guest_errno(e);
    return -1;
  }
  nxf_sock_set_connected(fd, 1);
  nxf_arm(fd, ARM_OUT);
  {
    static int told;
    if (!told) { told = 1; nxn_log("[net] first connect ok: %s:%u\n", ip4(h.sin_addr.s_addr, b), ntohs(h.sin_port)); }
  }
  NET_TRACE("[net] connect(%d, %s:%u) ok\n", fd, ip4(h.sin_addr.s_addr, b), ntohs(h.sin_port));
  return 0;
}

int nxs_accept(int fd, void *addr, void *addrlen) {
  if (!nxf_sock_tracked(fd)) { errno = nxf_is_fake(fd) ? G_EOPNOTSUPP : G_ENOTSOCK; return -1; }
  const int nb = nxf_sock_nonblock(fd);
  for (;;) {
    if (!nb) {
      /* libnx accept() allocates a newlib handle, so it runs under the port's
       * fd-table lock. Blocking inside that lock would freeze every asset
       * open until a peer arrived, so wait for readiness first, outside it. */
      const int r = wait_host(fd, POLLIN, -1);
      if (r < 0) { errno = nxs_guest_errno(errno); return -1; }
      host_set_nb(fd, 1);
    }
    struct sockaddr_in in;
    socklen_t il = sizeof in;
    memset(&in, 0, sizeof in);
    shim_fdtable_lock();
    const int nfd = NXN_NATIVE(accept)(fd, (struct sockaddr *)&in, &il);
    const int e = errno;
    const int nhfd = nfd >= 0 ? host_poll_fd(nfd) : -1;
    shim_fdtable_unlock();
    if (!nb) host_set_nb(fd, 0);
    if (nfd < 0) {
      if (host_again(e)) {
        if (!nb) continue;                          /* another thread took it */
        nxf_arm(fd, ARM_IN);
      } else {
        NET_FAIL("[net] accept(%d) FAILED errno %d\n", fd, e);
      }
      errno = nxs_guest_errno(e);
      return -1;
    }
    /* Linux: an accepted socket starts blocking. FreeBSD inherits O_NONBLOCK. */
    host_set_nb(nfd, 0);
    if (nxf_sock_track(nfd, G_SOCK_STREAM, 0, nhfd) != 0) {
      shim_fdtable_lock(); NXN_NATIVE(close)(nfd); shim_fdtable_unlock();
      errno = G_EMFILE;
      return -1;
    }
    nxf_sock_set_connected(nfd, 1);
    h2g_addr(&in, addr, addrlen);
    char b[16];
    NET_TRACE("[net] accept(%d) -> %d from %s:%u\n", fd, nfd, ip4(in.sin_addr.s_addr, b), ntohs(in.sin_port));
    return nfd;
  }
}

/* ------------------------------------------------------------------ */
/* data path                                                           */
/* ------------------------------------------------------------------ */
static long sock_send(int fd, const void *buf, size_t len, int gflags, const struct sockaddr_in *to) {
  if (nxf_is_fake(fd)) return nxf_write(fd, buf, len, (gflags & G_MSG_DONTWAIT) != 0);
  const ssize_t r = to
      ? NXN_NATIVE(sendto)(fd, buf, len, msg_to_host(gflags), (const struct sockaddr *)to, sizeof *to)
      : NXN_NATIVE(send)(fd, buf, len, msg_to_host(gflags));
  if (r < 0) {
    const int e = errno;
    if (host_again(e)) nxf_arm(fd, ARM_OUT);
    else NET_FAIL("[net] send(%d, %zu) FAILED errno %d\n", fd, len, e);
    errno = nxs_guest_errno(e);
    return -1;
  }
  if ((size_t)r < len) nxf_arm(fd, ARM_OUT);        /* send buffer filled */
  return (long)r;
}

static long sock_recv(int fd, void *buf, size_t len, int gflags, struct sockaddr_in *from, socklen_t *fromlen) {
  if (nxf_is_fake(fd)) {
    if (fromlen) *fromlen = 0;
    return nxf_read(fd, buf, len, (gflags & G_MSG_DONTWAIT) != 0);
  }
  const ssize_t r = from
      ? NXN_NATIVE(recvfrom)(fd, buf, len, msg_to_host(gflags), (struct sockaddr *)from, fromlen)
      : NXN_NATIVE(recv)(fd, buf, len, msg_to_host(gflags));
  if (r < 0) {
    const int e = errno;
    if (host_again(e)) nxf_arm(fd, ARM_IN);
    else NET_FAIL("[net] recv(%d, %zu) FAILED errno %d\n", fd, len, e);
    errno = nxs_guest_errno(e);
    return -1;
  }
  /* A short read (or EOF) means the receive queue was drained: the next data
   * is a new edge. MSG_PEEK leaves the data where it was. */
  if (!(gflags & G_MSG_PEEK) && (size_t)r < len) nxf_arm(fd, ARM_IN);
  return (long)r;
}

long nxs_send(int fd, const void *buf, size_t len, int flags) {
  return sock_send(fd, buf, len, flags, NULL);
}
long nxs_recv(int fd, void *buf, size_t len, int flags) {
  return sock_recv(fd, buf, len, flags, NULL, NULL);
}

long nxs_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen) {
  if (!addr) return sock_send(fd, buf, len, flags, NULL);
  struct sockaddr_in h;
  if (!g2h_addr(addr, addrlen, &h)) return -1;
  if (!may_reach(&h)) return -1;
  h.sin_addr.s_addr = maybe_broadcast(h.sin_addr.s_addr);
  return sock_send(fd, buf, len, flags, &h);
}

long nxs_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, void *addrlen) {
  if (!addr || !addrlen) return sock_recv(fd, buf, len, flags, NULL, NULL);
  struct sockaddr_in h;
  socklen_t hl = sizeof h;
  memset(&h, 0, sizeof h);
  const long r = sock_recv(fd, buf, len, flags, &h, &hl);
  if (r >= 0) {
    if (hl >= 8) h2g_addr(&h, addr, addrlen);
    else *(unsigned *)addrlen = 0;                   /* connected socket: no address */
  }
  return r;
}

#define IOV_MAX_G 1024
/* Multi-buffer calls are coalesced into one send/recv. A stream may transfer
 * less than asked (callers loop), so the copy is capped: copying a whole
 * multi-megabyte request on every partial write would be quadratic. A
 * datagram must go in one piece and cannot exceed 64 KiB anyway. */
#define GATHER_MAX (256 * 1024)
long nxs_sendmsg(int fd, const void *msgv, int flags) {
  const GMsghdr *m = (const GMsghdr *)msgv;
  if (!m || (m->iovlen && !m->iov)) { errno = G_EFAULT; return -1; }
  if (m->iovlen > IOV_MAX_G) { errno = G_EINVAL; return -1; }
  struct sockaddr_in h, *to = NULL;
  if (m->name && m->namelen) {
    if (!g2h_addr(m->name, m->namelen, &h)) return -1;
    if (!may_reach(&h)) return -1;
    h.sin_addr.s_addr = maybe_broadcast(h.sin_addr.s_addr);
    to = &h;
  }
  size_t total = 0, used = 0, last = 0;
  for (size_t i = 0; i < m->iovlen; i++) if (m->iov[i].len) { total += m->iov[i].len; used++; last = i; }
  if (used <= 1)                                   /* the common case: no copy */
    return sock_send(fd, used ? m->iov[last].base : "", used ? m->iov[last].len : 0, flags, to);
  if (total > GATHER_MAX && nxf_sock_type(fd) != G_SOCK_DGRAM) total = GATHER_MAX;
  uint8_t stackbuf[2048];
  uint8_t *tmp = total <= sizeof stackbuf ? stackbuf : (uint8_t *)malloc(total);
  if (!tmp) { errno = G_ENOMEM; return -1; }
  size_t off = 0;
  for (size_t i = 0; i < m->iovlen && off < total; i++) {
    if (!m->iov[i].len) continue;
    const size_t n = m->iov[i].len < total - off ? m->iov[i].len : total - off;
    memcpy(tmp + off, m->iov[i].base, n);
    off += n;
  }
  const long r = sock_send(fd, tmp, total, flags, to);
  const int e = errno;
  if (tmp != stackbuf) free(tmp);
  errno = e;
  return r;
}

long nxs_recvmsg(int fd, void *msgv, int flags) {
  GMsghdr *m = (GMsghdr *)msgv;
  if (!m || (m->iovlen && !m->iov)) { errno = G_EFAULT; return -1; }
  if (m->iovlen > IOV_MAX_G) { errno = G_EINVAL; return -1; }
  size_t total = 0, used = 0, last = 0;
  for (size_t i = 0; i < m->iovlen; i++) if (m->iov[i].len) { total += m->iov[i].len; used++; last = i; }

  struct sockaddr_in h;
  socklen_t hl = sizeof h;
  memset(&h, 0, sizeof h);
  struct sockaddr_in *from = m->name ? &h : NULL;
  socklen_t *fromlen = m->name ? &hl : NULL;

  long r;
  if (used <= 1) {
    uint8_t dummy;
    r = sock_recv(fd, used ? m->iov[last].base : &dummy, used ? m->iov[last].len : 0, flags, from, fromlen);
  } else {
    if (total > GATHER_MAX) total = GATHER_MAX;
    uint8_t stackbuf[2048];
    uint8_t *tmp = total <= sizeof stackbuf ? stackbuf : (uint8_t *)malloc(total);
    if (!tmp) { errno = G_ENOMEM; return -1; }
    r = sock_recv(fd, tmp, total, flags, from, fromlen);
    const int e = errno;
    if (r > 0) {
      size_t off = 0;
      for (size_t i = 0; i < m->iovlen && off < (size_t)r; i++) {
        const size_t n = m->iov[i].len < (size_t)r - off ? m->iov[i].len : (size_t)r - off;
        memcpy(m->iov[i].base, tmp + off, n);
        off += n;
      }
    }
    if (tmp != stackbuf) free(tmp);
    errno = e;
  }
  if (r >= 0) {
    if (m->name) {
      /* Asio resizes its endpoint to namelen and throws if it exceeds the
       * capacity it passed in; 16 always fits. A connected socket reports no
       * address, which must read as 0, not as the previous sender. */
      if (hl >= 8) { unsigned nl = m->namelen; h2g_addr(&h, m->name, &nl); m->namelen = nl; }
      else m->namelen = 0;
    }
    m->controllen = 0;
    m->flags = 0;
  }
  return r;
}

/* ------------------------------------------------------------------ */
/* options / names                                                     */
/* ------------------------------------------------------------------ */
int nxs_setsockopt(int fd, int level, int name, const void *val, unsigned len) {
  if (nxf_is_fake(fd)) return 0;
  int hl = -1, hn = -1;
  if (level == G_SOL_SOCKET)       { hl = SOL_SOCKET;  hn = so_to_host(name); }
  else if (level == G_IPPROTO_TCP) { hl = IPPROTO_TCP; hn = tcp_to_host(name); }
  if (hl < 0 || hn < 0) {
    /* IP_TOS, IPV6_V6ONLY, SO_PRIORITY ...: tuning hints with no host
     * equivalent. Linux callers treat these as best effort; succeed. */
    NET_TRACE("[net] setsockopt(%d, %d, %d) ignored\n", fd, level, name);
    return 0;
  }
  if (NXN_NATIVE(setsockopt)(fd, hl, hn, val, (socklen_t)len) < 0) {
    const int e = errno;
    if (e == EBADF || e == ENOTSOCK) { errno = nxs_guest_errno(e); return -1; }
    NET_TRACE("[net] setsockopt(%d, %d, %d) host refused (errno %d) -- reported ok\n", fd, level, name, e);
  }
  return 0;
}

int nxs_getsockopt(int fd, int level, int name, void *val, void *lenp) {
  if (!val || !lenp) { errno = G_EFAULT; return -1; }
  if (nxf_is_fake(fd)) {
    if (level == G_SOL_SOCKET && name == G_SO_ERROR && *(unsigned *)lenp >= sizeof(int)) {
      *(int *)val = 0; *(unsigned *)lenp = sizeof(int); return 0;
    }
    errno = G_ENOPROTOOPT; return -1;
  }
  int hl = -1, hn = -1;
  if (level == G_SOL_SOCKET)       { hl = SOL_SOCKET;  hn = so_to_host(name); }
  else if (level == G_IPPROTO_TCP) { hl = IPPROTO_TCP; hn = tcp_to_host(name); }
  if (hl < 0 || hn < 0) { errno = G_ENOPROTOOPT; return -1; }
  socklen_t l = (socklen_t)*(unsigned *)lenp;
  if (NXN_NATIVE(getsockopt)(fd, hl, hn, val, &l) < 0) { errno = nxs_guest_errno(errno); return -1; }
  *(unsigned *)lenp = (unsigned)l;
  if (level == G_SOL_SOCKET && name == G_SO_ERROR && l >= sizeof(int) && *(int *)val) {
    /* Raw from the bsd service, which already numbers errno like Linux. */
    NET_FAIL("[net] SO_ERROR on fd %d: %d (asynchronous connect/transfer failure)\n", fd, *(int *)val);
  }
  return 0;
}

int nxs_getsockname(int fd, void *addr, void *addrlen) {
  if (!addr || !addrlen) { errno = G_EFAULT; return -1; }
  struct sockaddr_in h; socklen_t hl = sizeof h;
  memset(&h, 0, sizeof h);
  if (NXN_NATIVE(getsockname)(fd, (struct sockaddr *)&h, &hl) < 0) { errno = nxs_guest_errno(errno); return -1; }
  h2g_addr(&h, addr, addrlen);
  return 0;
}

int nxs_getpeername(int fd, void *addr, void *addrlen) {
  if (!addr || !addrlen) { errno = G_EFAULT; return -1; }
  struct sockaddr_in h; socklen_t hl = sizeof h;
  memset(&h, 0, sizeof h);
  if (NXN_NATIVE(getpeername)(fd, (struct sockaddr *)&h, &hl) < 0) { errno = nxs_guest_errno(errno); return -1; }
  h2g_addr(&h, addr, addrlen);
  return 0;
}

/* ------------------------------------------------------------------ */
/* generic fd calls                                                    */
/* ------------------------------------------------------------------ */
long nxs_read(int fd, void *buf, size_t len) {
  if (nxf_is_fake(fd)) return nxf_read(fd, buf, len, 0);
  if (nxf_sock_tracked(fd)) return sock_recv(fd, buf, len, 0, NULL, NULL);
  return (long)NXN_NATIVE(read)(fd, buf, len);
}

long nxs_write(int fd, const void *buf, size_t len) {
  if (nxf_is_fake(fd)) return nxf_write(fd, buf, len, 0);
  if (nxf_sock_tracked(fd)) return sock_send(fd, buf, len, 0, NULL);
  return (long)NXN_NATIVE(write)(fd, buf, len);
}

int nxs_fcntl(int fd, int cmd, long arg, int *ret) {
  if (nxf_is_fake(fd)) { *ret = nxf_fcntl(fd, cmd, arg); return 1; }
  if (!nxf_sock_tracked(fd)) return 0;
  switch (cmd) {
    case G_F_GETFL:
      *ret = G_O_RDWR | (nxf_sock_nonblock(fd) ? G_O_NONBLOCK : 0);
      break;
    case G_F_SETFL: {
      const int on = (arg & G_O_NONBLOCK) != 0;
      if (host_set_nb(fd, on) < 0) { errno = nxs_guest_errno(errno); *ret = -1; break; }
      nxf_sock_set_nonblock(fd, on);
      *ret = 0;
      break;
    }
    case G_F_GETFD: case G_F_SETFD:
      *ret = 0;                                    /* FD_CLOEXEC: no exec() here */
      break;
    default:
      errno = G_EINVAL; *ret = -1;
      break;
  }
  return 1;
}

int nxs_ioctl(int fd, int request, ...) {
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (nxf_is_fake(fd)) return nxf_ioctl(fd, request, arg);
  if (!nxf_sock_tracked(fd)) { errno = G_ENOTTY; return -1; }
  if (request == G_FIONBIO) {
    const int on = arg && *(int *)arg != 0;
    if (host_set_nb(fd, on) < 0) { errno = nxs_guest_errno(errno); return -1; }
    nxf_sock_set_nonblock(fd, on);
    return 0;
  }
  if (request == G_FIONREAD) {
    int n = 0;
    if (NXN_NATIVE(ioctl)(fd, FIONREAD, &n) < 0) { errno = nxs_guest_errno(errno); return -1; }
    if (arg) *(int *)arg = n;
    return 0;
  }
  errno = G_ENOTTY;
  return -1;
}

/* ------------------------------------------------------------------ */
/* name resolution                                                     */
/* ------------------------------------------------------------------ */
static int map_eai(int rc) {
  if (rc == 0) return 0;
#ifdef EAI_AGAIN
  if (rc == EAI_AGAIN) return G_EAI_AGAIN;
#endif
#ifdef EAI_BADFLAGS
  if (rc == EAI_BADFLAGS) return G_EAI_BADFLAGS;
#endif
#ifdef EAI_FAMILY
  if (rc == EAI_FAMILY) return G_EAI_FAMILY;
#endif
#ifdef EAI_MEMORY
  if (rc == EAI_MEMORY) return G_EAI_MEMORY;
#endif
#ifdef EAI_NONAME
  if (rc == EAI_NONAME) return G_EAI_NONAME;
#endif
#ifdef EAI_NODATA
  if (rc == EAI_NODATA) return G_EAI_NONAME;
#endif
#ifdef EAI_SERVICE
  if (rc == EAI_SERVICE) return G_EAI_SERVICE;
#endif
#ifdef EAI_SOCKTYPE
  if (rc == EAI_SOCKTYPE) return G_EAI_SOCKTYPE;
#endif
#ifdef EAI_SYSTEM
  if (rc == EAI_SYSTEM) return G_EAI_AGAIN;      /* IPC failure: worth a retry */
#endif
  return G_EAI_FAIL;
}

static int parse_service(const char *svc, int flags, uint16_t *port_be) {
  *port_be = 0;
  if (!svc || !*svc) return 0;
  char *end = NULL;
  const long v = strtol(svc, &end, 10);
  if (end && *end == 0 && v >= 0 && v <= 65535) { *port_be = htons((uint16_t)v); return 0; }
  if (flags & G_AI_NUMERICSERV) return G_EAI_NONAME;
  static const struct { const char *n; uint16_t p; } k[] = {
    { "http", 80 }, { "https", 443 }, { "ftp", 21 }, { "domain", 53 }, { "ntp", 123 }, { NULL, 0 }
  };
  for (int i = 0; k[i].n; i++) if (!strcasecmp(svc, k[i].n)) { *port_be = htons(k[i].p); return 0; }
  return G_EAI_SERVICE;
}

#define MAX_ADDRS 16

/* ------------------------------------------------------------------ */
/* Ninja Kiwi's IPv4 rewrite                                           */
/* ------------------------------------------------------------------ */
/* Before connecting, the engine runs every IPv4 literal through
 * IPv4_to_domain_name() (libnative.so 0x7abef4; caller 0x7638b4 does it
 * unconditionally): 54.197.29.169 becomes "ip-54-197-29-169.souparea.com",
 * the classic trick for IPv6-only (NAT64/DNS64) networks. The pattern comes
 * from NK's downloaded SKU settings (settings.network.ipv4_rewrite_pattern),
 * with that souparea.com name as the built-in default -- and the default no
 * longer resolves anywhere, so a console without the server override could
 * never reach a co-op server.
 *
 * The name only ever stands for the address it encodes, so: ask DNS first
 * (a working server-side pattern is honoured), and if the name does not
 * resolve, use the embedded address. A domain that failed once is not asked
 * again this session -- each failed lookup cost up to a second. */
static int parse_rewrite_ip(const char *node, uint32_t *out_be, const char **domain) {
  if (!node || strncasecmp(node, "ip-", 3) != 0) return 0;
  const char *p = node + 3;
  uint32_t v = 0;
  for (int k = 0; k < 4; k++) {
    unsigned o = 0; int d = 0;
    while (*p >= '0' && *p <= '9' && d < 4) { o = o * 10 + (unsigned)(*p - '0'); p++; d++; }
    if (d == 0 || d > 3 || o > 255) return 0;
    v = (v << 8) | o;
    if (k < 3) { if (*p != '-') return 0; p++; }
  }
  if (*p != '.' || !p[1]) return 0;              /* must continue with a domain */
  *out_be = htonl(v);
  if (domain) *domain = p + 1;
  return 1;
}

#define DEAD_DOMAINS 4
static nxn_mutex g_dead_lock = NXN_MUTEX_INIT;
static char g_dead[DEAD_DOMAINS][96];
static int domain_dead(const char *d) {
  int r = 0;
  nxn_lock(&g_dead_lock);
  for (int i = 0; i < DEAD_DOMAINS; i++) if (g_dead[i][0] && !strcasecmp(g_dead[i], d)) r = 1;
  nxn_unlock(&g_dead_lock);
  return r;
}
static void domain_mark_dead(const char *d) {
  nxn_lock(&g_dead_lock);
  int slot = -1;
  for (int i = 0; i < DEAD_DOMAINS; i++) {
    if (g_dead[i][0] && !strcasecmp(g_dead[i], d)) { slot = -2; break; }
    if (!g_dead[i][0] && slot == -1) slot = i;
  }
  if (slot >= 0) snprintf(g_dead[slot], sizeof g_dead[slot], "%s", d);
  nxn_unlock(&g_dead_lock);
}

/* Every distinct host resolved, once: shows in btd5_net.log what the game
 * talked to (e.g. whether NK's settings download ran), without a full trace. */
#define SEEN_HOSTS 32
static nxn_mutex g_seen_lock = NXN_MUTEX_INIT;
static char g_seen[SEEN_HOSTS][80];
static int  g_seen_n;
static int first_time_seen(const char *host) {
  int fresh = 1;
  nxn_lock(&g_seen_lock);
  for (int i = 0; i < g_seen_n; i++) if (!strcasecmp(g_seen[i], host)) { fresh = 0; break; }
  if (fresh && g_seen_n < SEEN_HOSTS) snprintf(g_seen[g_seen_n++], sizeof g_seen[0], "%s", host);
  else if (fresh) fresh = 0;                     /* table full: stay quiet */
  nxn_unlock(&g_seen_lock);
  return fresh;
}

/* Resolve node to IPv4 addresses (network order). Literals and the passive /
 * loopback forms never reach sfdnsres. Returns 0 or a guest EAI_* code. */
static int resolve_v4(const char *node, int flags, int family, uint32_t *out, int *n) {
  *n = 0;
  struct in_addr a;
  if (!node || !*node) {
    out[(*n)++] = htonl((flags & G_AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    return 0;
  }
  if (inet_pton(AF_INET, node, &a) == 1) { out[(*n)++] = a.s_addr; return 0; }
  if (family == G_AF_UNSPEC && !strcmp(node, "::"))  { out[(*n)++] = htonl(INADDR_ANY); return 0; }
  if (family == G_AF_UNSPEC && !strcmp(node, "::1")) { out[(*n)++] = htonl(INADDR_LOOPBACK); return 0; }
  if (!strcasecmp(node, "localhost")) { out[(*n)++] = htonl(INADDR_LOOPBACK); return 0; }
  if (flags & G_AI_NUMERICHOST) return G_EAI_NONAME;

  uint32_t embedded = 0;
  const char *rw_domain = NULL;
  const int is_rewrite = parse_rewrite_ip(node, &embedded, &rw_domain);
  if (is_rewrite && domain_dead(rw_domain)) { out[(*n)++] = embedded; return 0; }

  if (!nx_net_enabled()) return G_EAI_AGAIN;
  if (!nx_net_online()) {
    static int told;
    if (!told) { told = 1; nxn_log("[net] no internet connection (nifm) -- lookups of '%s' and later fail fast\n", node); }
    return G_EAI_AGAIN;
  }
  struct addrinfo hh, *res = NULL;
  memset(&hh, 0, sizeof hh);
  hh.ai_family = AF_INET;
  hh.ai_socktype = SOCK_STREAM;                  /* one entry per address */
  const uint64_t t0 = nxn_now_ms();
  const int rc = NXN_NATIVE(getaddrinfo)(node, NULL, &hh, &res);
  if (rc != 0) {
    if (is_rewrite) {
      char b[16];
      domain_mark_dead(rw_domain);
      nxn_log("[net] %s does not resolve (rc %d) -- using the address it encodes, %s. "
              "(Ninja Kiwi's IPv4-rewrite domain '%s' no longer answers.)\n",
              node, rc, ip4(embedded, b), rw_domain);
      out[(*n)++] = embedded;
      return 0;
    }
    NET_FAIL("[net] DNS %s FAILED (rc %d, errno %d, %llu ms)\n", node, rc, errno,
             (unsigned long long)(nxn_now_ms() - t0));
    return map_eai(rc);
  }
  for (struct addrinfo *p = res; p && *n < MAX_ADDRS; p = p->ai_next) {
    if (p->ai_family != AF_INET || !p->ai_addr) continue;
    const uint32_t v = ((const struct sockaddr_in *)p->ai_addr)->sin_addr.s_addr;
    int dup = 0;
    for (int k = 0; k < *n; k++) if (out[k] == v) dup = 1;
    if (!dup) out[(*n)++] = v;
  }
  NXN_NATIVE(freeaddrinfo)(res);
  if (*n == 0) return G_EAI_NONAME;
  {
    char b[16];
    if (first_time_seen(node))
      nxn_log("[net] DNS %s -> %s%s (%llu ms)\n", node, ip4(out[0], b), *n > 1 ? " (+more)" : "",
              (unsigned long long)(nxn_now_ms() - t0));
    NET_TRACE("[net] DNS %s -> %s (+%d more)\n", node, ip4(out[0], b), *n - 1);
  }
  return 0;
}

int nxs_getaddrinfo(const char *node, const char *service, const void *hintsv, void **res) {
  if (!res) return G_EAI_FAIL;
  *res = NULL;
  if ((!node || !*node) && (!service || !*service)) return G_EAI_NONAME;

  const GAddrInfo *hints = (const GAddrInfo *)hintsv;
  int flags = 0, family = G_AF_UNSPEC, socktype = 0, protocol = 0;
  if (hints) { flags = hints->flags; family = hints->family; socktype = hints->socktype; protocol = hints->protocol; }
  if (family != G_AF_UNSPEC && family != G_AF_INET) return G_EAI_FAMILY;
  if (socktype != 0 && socktype != G_SOCK_STREAM && socktype != G_SOCK_DGRAM) return G_EAI_SOCKTYPE;

  uint16_t port;
  int rc = parse_service(service, flags, &port);
  if (rc) return rc;

  uint32_t addrs[MAX_ADDRS];
  int naddr = 0;
  rc = resolve_v4(node, flags, family, addrs, &naddr);
  if (rc) return rc;

  if (!socktype && protocol == G_IPPROTO_TCP) socktype = G_SOCK_STREAM;
  if (!socktype && protocol == G_IPPROTO_UDP) socktype = G_SOCK_DGRAM;
  static const int both[2] = { G_SOCK_STREAM, G_SOCK_DGRAM };
  const int *types = socktype ? &socktype : both;
  const int ntypes = socktype ? 1 : 2;
  const size_t cn = ((flags & G_AI_CANONNAME) && node) ? strlen(node) + 1 : 0;

  GAddrInfo *head = NULL, **tail = &head;
  for (int i = 0; i < naddr; i++) {
    for (int t = 0; t < ntypes; t++) {
      const int first = (head == NULL);
      GAddrInfo *g = (GAddrInfo *)calloc(1, sizeof *g + sizeof(GSockaddrIn) + (first ? cn : 0));
      if (!g) { nxs_freeaddrinfo(head); return G_EAI_MEMORY; }
      GSockaddrIn *sa = (GSockaddrIn *)(g + 1);
      sa->family = G_AF_INET; sa->port = port; sa->addr = addrs[i];
      g->flags = flags;
      g->family = G_AF_INET;
      g->socktype = types[t];
      g->protocol = protocol ? protocol : (types[t] == G_SOCK_STREAM ? G_IPPROTO_TCP : G_IPPROTO_UDP);
      g->addrlen = sizeof *sa;
      g->addr = sa;
      if (first && cn) { g->canonname = (char *)(sa + 1); memcpy(g->canonname, node, cn); }
      *tail = g; tail = &g->next;
    }
  }
  *res = head;
  return 0;
}

void nxs_freeaddrinfo(void *res) {
  GAddrInfo *g = (GAddrInfo *)res;
  while (g) { GAddrInfo *n = g->next; free(g); g = n; }    /* one block per node */
}

/* Classic non-reentrant contract, like glibc's: one static result. */
static nxn_mutex g_hb_lock = NXN_MUTEX_INIT;
static struct {
  GHostent h;
  char name[256];
  char *aliases[1];
  uint32_t addrs[MAX_ADDRS];
  char *list[MAX_ADDRS + 1];
} g_hb;

void *nxs_gethostbyname(const char *name) {
  if (!name) return NULL;
  uint32_t addrs[MAX_ADDRS];
  int n = 0;
  if (resolve_v4(name, 0, G_AF_INET, addrs, &n) != 0 || n == 0) return NULL;
  nxn_lock(&g_hb_lock);
  memset(&g_hb, 0, sizeof g_hb);
  strncpy(g_hb.name, name, sizeof g_hb.name - 1);
  for (int i = 0; i < n; i++) { g_hb.addrs[i] = addrs[i]; g_hb.list[i] = (char *)&g_hb.addrs[i]; }
  g_hb.h.h_name = g_hb.name;
  g_hb.h.h_aliases = g_hb.aliases;
  g_hb.h.h_addrtype = G_AF_INET;
  g_hb.h.h_length = 4;
  g_hb.h.h_addr_list = g_hb.list;
  nxn_unlock(&g_hb_lock);
  return &g_hb.h;
}

int nxs_gethostname(char *name, size_t len) {
  if (!name || !len) { errno = G_EINVAL; return -1; }
  static const char host[] = "nintendo-switch";
  if (len < sizeof host) { errno = G_EINVAL; return -1; }
  memcpy(name, host, sizeof host);
  return 0;
}

void *nxs_getservbyname(const char *name, const char *proto) { (void)name; (void)proto; return NULL; }
unsigned nxs_if_nametoindex(const char *name) { (void)name; errno = 19 /* ENODEV */; return 0; }
char *nxs_if_indextoname(unsigned index, char *name) { (void)index; (void)name; errno = 6 /* ENXIO */; return NULL; }

int nxs_inet_pton(int af, const char *src, void *dst) {
  if (!src || !dst) { errno = G_EFAULT; return -1; }
  if (af == G_AF_INET) {
    const int r = inet_pton(AF_INET, src, dst);
    if (r < 0) errno = nxs_guest_errno(errno);
    return r;
  }
  if (af == G_AF_INET6) {
#ifdef AF_INET6
    const int r = inet_pton(AF_INET6, src, dst); /* parsing only; never routed */
    if (r < 0) errno = nxs_guest_errno(errno);
    return r;
#else
    return 0;
#endif
  }
  errno = G_EAFNOSUPPORT;
  return -1;
}

const char *nxs_inet_ntop(int af, const void *src, char *dst, unsigned size) {
  if (!src || !dst) { errno = G_EFAULT; return NULL; }
  const char *r = NULL;
  if (af == G_AF_INET) r = inet_ntop(AF_INET, src, dst, (socklen_t)size);
#ifdef AF_INET6
  else if (af == G_AF_INET6) r = inet_ntop(AF_INET6, src, dst, (socklen_t)size);
#endif
  else { errno = G_EAFNOSUPPORT; return NULL; }
  if (!r) errno = nxs_guest_errno(errno);
  return r;
}

uint32_t nxs_inet_addr(const char *cp) {
  struct in_addr a;
  return (cp && inet_pton(AF_INET, cp, &a) == 1) ? a.s_addr : 0xffffffffu;
}
