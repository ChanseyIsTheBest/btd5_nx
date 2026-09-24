/* nx_net.h -- online play for the Bloons TD 5 Switch port.
 *
 * libnative.so talks to Ninja Kiwi's servers through three statically linked
 * stacks, all of which reach the OS only through bionic libc imports:
 *
 *   libcurl 7.57 + OpenSSL 1.0.2n   HTTPS to api.ninjakiwi.com, static-api.
 *                                   nkstatic.com, time.ninjakiwi.com ...
 *                                   (socket/connect/fcntl/select/poll/read/
 *                                   write/getaddrinfo, and /dev/urandom to
 *                                   seed OpenSSL's RNG)
 *   Boost 1.63 Asio                 the co-op realtime link (CNetMgrImpl,
 *                                   LegacyMatchmakingClient_BTD5 ->
 *                                   live.btd5mobile.ninjakiwi.com). Its epoll
 *                                   reactor needs epoll_* + eventfd, and it
 *                                   drives sockets with ioctl(FIONBIO),
 *                                   sendmsg/recvmsg, accept, SO_ERROR.
 *   the NK SDK                      device id (MainActivity.getUniqueID) that
 *                                   identifies this console to NK's services.
 *
 * None of it needs Google Play services. Previously every one of these imports
 * was stubbed to fail, and epoll_create failing made Asio throw the moment the
 * co-op screens built their io_service.
 *
 * Module map:
 *   nx_net.c     service bring-up (bsd sessions, nifm), config keys, device id
 *   nx_net_log.c btd5_net.log (works without a DEBUG_LOG build)
 *   nx_socket.c  bionic-ABI BSD sockets + resolver over libnx
 *   nx_fdemu.c   fd registry, emulated fds (eventfd, pipe, socketpair,
 *                /dev/urandom), epoll (edge-triggered), poll/select
 *
 * MIT license -- see LICENSE.
 */
#ifndef NX_NET_H
#define NX_NET_H

#include <stddef.h>
#include <stdint.h>

/* ---- bring-up / status (nx_net.c) --------------------------------------- */
void nx_net_init(void);            /* idempotent; call before the engine loads */
void nx_net_exit(void);
int  nx_net_enabled(void);         /* config.txt online=1 (default)            */
int  nx_net_online(void);          /* nifm says connected (cached 1 s)         */
int  nx_net_trace(void);           /* s_trace in nx_net.c (0 in release)       */
int  nx_net_connect_timeout_ms(void); /* 0 = true async connect (default)      */
uint32_t nx_net_subnet_broadcast(void); /* network order, 0 if unknown         */
const char *nx_net_device_id(void);   /* stable per-console id for getUniqueID */
const char *nx_net_player_name(void); /* config player_name, or "Player"       */
void nx_net_log_status(const char *why);

/* ---- log (nx_net_log.c): btd5_net.log in the game folder, always on ---- */
int  nx_net_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nx_net_log_close(void);

/* ---- bionic-ABI entry points wired into imports.c (nx_socket.c) ---------- */
int  nxs_socket(int domain, int type, int protocol);
int  nxs_socketpair(int domain, int type, int protocol, int sv[2]);
int  nxs_bind(int fd, const void *addr, unsigned addrlen);
int  nxs_connect(int fd, const void *addr, unsigned addrlen);
int  nxs_listen(int fd, int backlog);
int  nxs_accept(int fd, void *addr, void *addrlen);
int  nxs_shutdown(int fd, int how);
long nxs_send(int fd, const void *buf, size_t len, int flags);
long nxs_recv(int fd, void *buf, size_t len, int flags);
long nxs_sendto(int fd, const void *buf, size_t len, int flags,
                const void *addr, unsigned addrlen);
long nxs_recvfrom(int fd, void *buf, size_t len, int flags,
                  void *addr, void *addrlen);
long nxs_sendmsg(int fd, const void *msg, int flags);
long nxs_recvmsg(int fd, void *msg, int flags);
int  nxs_setsockopt(int fd, int level, int name, const void *val, unsigned len);
int  nxs_getsockopt(int fd, int level, int name, void *val, void *len);
int  nxs_getsockname(int fd, void *addr, void *addrlen);
int  nxs_getpeername(int fd, void *addr, void *addrlen);

int  nxs_getaddrinfo(const char *node, const char *service,
                     const void *hints, void **res);
void nxs_freeaddrinfo(void *res);
void *nxs_gethostbyname(const char *name);
int  nxs_gethostname(char *name, size_t len);
void *nxs_getservbyname(const char *name, const char *proto);
unsigned nxs_if_nametoindex(const char *name);
char *nxs_if_indextoname(unsigned index, char *name);
int  nxs_inet_pton(int af, const char *src, void *dst);
const char *nxs_inet_ntop(int af, const void *src, char *dst, unsigned size);
uint32_t nxs_inet_addr(const char *cp);

/* generic fd calls that must route sockets and emulated fds */
long nxs_read(int fd, void *buf, size_t len);
long nxs_write(int fd, const void *buf, size_t len);
int  nxs_ioctl(int fd, int request, ...);
/* fcntl for sockets/emulated fds. Returns 1 and stores the result in *ret when
 * it handled fd; 0 when fd is an ordinary file the caller should handle. */
int  nxs_fcntl(int fd, int cmd, long arg, int *ret);
/* close() hook, called by close_fake BEFORE the newlib close. Returns 1 and
 * stores the result when fd was an emulated fd (nothing left to close). */
int  nxs_close_hook(int fd, int *ret);

/* ---- emulated fds / readiness (nx_fdemu.c) ------------------------------- */
int  nxf_is_fake(int fd);
int  nxf_pipe(int fds[2]);
int  nxf_eventfd(unsigned initval, int flags);
int  nxf_epoll_create(int size);
int  nxf_epoll_create1(int flags);
int  nxf_epoll_ctl(int epfd, int op, int fd, void *event);
int  nxf_epoll_wait(int epfd, void *events, int maxevents, int timeout);
int  nxf_poll(void *fds, unsigned int nfds, int timeout);   /* bionic nfds_t is 32-bit */
int  nxf_select(int nfds, void *rd, void *wr, void *ex, void *timeout);
void nxf_fd_set_chk(int fd, void *set, size_t setsize);   /* __FD_SET_chk   */
int  nxf_open_urandom(int flags);                         /* /dev/urandom  */
/* fstat for emulated fds: fills mode/ino/rdev; returns 0, or -1 if not fake */
int  nxf_fstat_info(int fd, uint32_t *mode, uint64_t *ino, uint64_t *rdev);

#endif
