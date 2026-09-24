/* nx_net_internal.h -- shared by nx_net.c, nx_socket.c and nx_fdemu.c only.
 *
 * Two build targets use these sources:
 *   __SWITCH__   the port itself, over libnx + newlib.
 *   NXNET_HOST   tools/net_test: the same code on Linux, driven by a real
 *                Boost-1.63-era Asio (standalone 1.10.8) and C unit tests, so
 *                the epoll/eventfd emulation can be exercised without hardware.
 *                There the "native" calls are Linux's own, reached through
 *                -Wl,--wrap as __real_*.
 *
 * GUEST vs HOST. "Guest" is what libnative.so was built against: bionic on
 * arm64, i.e. Linux numbering for every constant and errno. "Host" is what we
 * actually call: libnx's FreeBSD-flavoured sockets with newlib errno values.
 * Nothing here may pass a guest constant to a host call unconverted.
 *
 * No __thread anywhere: the port points TPIDR_EL0 at a bionic TLS block for
 * the engine (see tls_setup_guard), so thread-local storage is off limits.
 *
 * MIT license -- see LICENSE.
 */
#ifndef NX_NET_INTERNAL_H
#define NX_NET_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* platform layer                                                      */
/* ------------------------------------------------------------------ */
#if defined(__SWITCH__)
#include <errno.h>
#include <switch.h>
typedef Mutex   nxn_mutex;
typedef CondVar nxn_cond;
#define NXN_MUTEX_INIT 0
#define NXN_COND_INIT  0
static inline void nxn_lock(nxn_mutex *m)   { mutexLock(m); }
static inline void nxn_unlock(nxn_mutex *m) { mutexUnlock(m); }
static inline void nxn_wake_all(nxn_cond *c) { condvarWakeAll(c); }
/* wait at most ms (>=0); returns with the mutex held */
static inline void nxn_wait_ms(nxn_cond *c, nxn_mutex *m, int ms) {
  condvarWaitTimeout(c, m, (u64)(ms < 0 ? 0 : ms) * 1000000ull);
}
static inline uint64_t nxn_now_ms(void) {
  return armTicksToNs(armGetSystemTick()) / 1000000ull;
}
static inline void nxn_sleep_ms(int ms) {
  if (ms > 0) svcSleepThread((s64)ms * 1000000ll);
}
static inline void nxn_random(void *buf, size_t len) { randomGet(buf, len); }
#define NXN_NATIVE(fn) fn
/* Readiness is polled on bsd-service descriptors with bsdPoll(), NOT through
 * libnx's poll(): that one translates every fd via newlib's handle table
 * without a lock, so a socket closed on another thread (Asio closes handlers'
 * sockets while its reactor thread polls) could be read mid-release -- the
 * same unlocked handle-table race libc_shim.c documents as a crash source.
 * bsd descriptors are resolved once, under the fd-table lock, at creation. A
 * stale one is harmless: the bsd service answers POLLNVAL for it. */
#include <poll.h>
#include <switch/services/bsd.h>        /* bsdPoll; <switch.h> leaves it out */
static inline int nxn_host_poll(struct pollfd *fds, nfds_t n, int timeout) {
  const int r = bsdPoll(fds, n, timeout);
  if (r < 0) errno = EIO;              /* detail lives in libnx's g_bsdErrno */
  return r;
}
#define NXN_POLL nxn_host_poll
int nx_net_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define nxn_log(...) nx_net_logf(__VA_ARGS__)
/* the port's newlib fd-table lock (libc_shim.c): libnx socket()/accept()
 * allocate newlib handles, which must never race file open/close. */
void shim_fdtable_lock(void);
void shim_fdtable_unlock(void);

#elif defined(NXNET_HOST)
#include <pthread.h>
#include <stdio.h>
#include <time.h>
typedef pthread_mutex_t nxn_mutex;
typedef pthread_cond_t  nxn_cond;
#define NXN_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define NXN_COND_INIT  PTHREAD_COND_INITIALIZER
static inline void nxn_lock(nxn_mutex *m)   { pthread_mutex_lock(m); }
static inline void nxn_unlock(nxn_mutex *m) { pthread_mutex_unlock(m); }
static inline void nxn_wake_all(nxn_cond *c) { pthread_cond_broadcast(c); }
static inline void nxn_wait_ms(nxn_cond *c, nxn_mutex *m, int ms) {
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  long long ns = ts.tv_nsec + (long long)(ms < 0 ? 0 : ms) * 1000000ll;
  ts.tv_sec += ns / 1000000000ll; ts.tv_nsec = ns % 1000000000ll;
  pthread_cond_timedwait(c, m, &ts);
}
static inline uint64_t nxn_now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
static inline void nxn_sleep_ms(int ms) {
  if (ms > 0) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000l }; nanosleep(&ts, NULL); }
}
void nxn_random(void *buf, size_t len);          /* net_test/host_glue.c */
#define NXN_NATIVE(fn) __real_##fn
/* libnx's poll() fails the WHOLE call (-1) when any fd is not a live socket,
 * where Linux answers POLLNVAL. The host harness reproduces libnx's rule so
 * the recovery paths are exercised (net_test/host_glue.c). */
struct pollfd;
int nxt_libnx_poll(struct pollfd *fds, unsigned long nfds, int timeout);
#define NXN_POLL nxt_libnx_poll
extern volatile unsigned nxt_wait_loops;       /* wakeup counter for tests */
#define nxn_log(...) fprintf(stderr, __VA_ARGS__)
static inline void shim_fdtable_lock(void) {}
static inline void shim_fdtable_unlock(void) {}
#else
#error "build nx_net for __SWITCH__ or NXNET_HOST"
#endif

/* ------------------------------------------------------------------ */
/* guest (bionic arm64 / Linux) constants                              */
/* ------------------------------------------------------------------ */
#define G_EPERM        1
#define G_ENOENT       2
#define G_EINTR        4
#define G_EBADF        9
#define G_EAGAIN       11
#define G_ENOMEM       12
#define G_EFAULT       14
#define G_EEXIST       17
#define G_EINVAL       22
#define G_EMFILE       24
#define G_ENOTTY       25
#define G_ESPIPE       29
#define G_EPIPE        32
#define G_ENOSYS       38
#define G_ENOTSOCK     88
#define G_EDESTADDRREQ 89
#define G_ENOPROTOOPT  92
#define G_EPROTONOSUPPORT 93
#define G_EOPNOTSUPP   95
#define G_EAFNOSUPPORT 97
#define G_ENETDOWN     100
#define G_ENETUNREACH  101
#define G_EISCONN      106
#define G_ENOTCONN     107
#define G_ETIMEDOUT    110
#define G_ECONNREFUSED 111
#define G_EALREADY     114
#define G_EINPROGRESS  115

#define G_O_NONBLOCK   0x800       /* 04000 */
#define G_O_CLOEXEC    0x80000     /* 02000000 */
#define G_O_RDWR       2
#define G_F_DUPFD      0
#define G_F_GETFD      1
#define G_F_SETFD      2
#define G_F_GETFL      3
#define G_F_SETFL      4
#define G_F_DUPFD_CLOEXEC 1030
#define G_FIONREAD     0x541B
#define G_FIONBIO      0x5421

#define G_POLLIN       0x001
#define G_POLLPRI      0x002
#define G_POLLOUT      0x004
#define G_POLLERR      0x008
#define G_POLLHUP      0x010
#define G_POLLNVAL     0x020
#define G_POLLRDNORM   0x040
#define G_POLLRDBAND   0x080
#define G_POLLWRNORM   0x100
#define G_POLLWRBAND   0x200
#define G_POLLRDHUP    0x2000

/* epoll(7) bits are the same numbers as poll's for the base events */
#define G_EPOLLIN      0x001
#define G_EPOLLPRI     0x002
#define G_EPOLLOUT     0x004
#define G_EPOLLERR     0x008
#define G_EPOLLHUP     0x010
#define G_EPOLLRDNORM  0x040
#define G_EPOLLRDBAND  0x080
#define G_EPOLLWRNORM  0x100
#define G_EPOLLWRBAND  0x200
#define G_EPOLLRDHUP   0x2000
#define G_EPOLLONESHOT (1u << 30)
#define G_EPOLLET      (1u << 31)
#define G_EPOLL_CTL_ADD 1
#define G_EPOLL_CTL_DEL 2
#define G_EPOLL_CTL_MOD 3
#define G_EFD_SEMAPHORE 1

#define G_SOCK_STREAM  1
#define G_SOCK_DGRAM   2

/* Edge-triggered "armed" bits kept per fd: an armed event is reported the
 * next time the fd is seen in that state, then disarmed until something
 * (EAGAIN, a short transfer, epoll_ctl MOD, a peer write) re-arms it. */
#define ARM_IN   G_EPOLLIN
#define ARM_PRI  G_EPOLLPRI
#define ARM_OUT  G_EPOLLOUT
#define ARM_ERR  G_EPOLLERR
#define ARM_HUP  G_EPOLLHUP
#define ARM_ALL  (ARM_IN | ARM_PRI | ARM_OUT | ARM_ERR | ARM_HUP)

/* ------------------------------------------------------------------ */
/* fd registry (nx_fdemu.c) -- used by nx_socket.c                     */
/* ------------------------------------------------------------------ */
#define NXF_SOCK_MAX 4096          /* newlib handle numbers we can track */

/* hostfd: the descriptor readiness is polled on (bsd-service fd on Switch,
 * the fd itself on the Linux test host). */
int  nxf_sock_track(int fd, int type, int nonblock, int hostfd);   /* 0 ok */
int  nxf_sock_hostfd(int fd);      /* -1 if untracked */
void nxf_sock_forget(int fd);      /* drop from every epoll set, untrack   */
int  nxf_sock_tracked(int fd);
int  nxf_sock_type(int fd);        /* G_SOCK_STREAM / G_SOCK_DGRAM / 0     */
int  nxf_sock_nonblock(int fd);
void nxf_sock_set_nonblock(int fd, int on);
int  nxf_sock_connected(int fd);
void nxf_sock_set_connected(int fd, int on);
/* Re-arm edge-triggered events on a socket (after EAGAIN / a short transfer
 * / EINPROGRESS). Wakes epoll waiters when an armed bit is newly set. */
void nxf_arm(int fd, uint32_t bits);

/* emulated-fd I/O, dispatched from nxs_read/nxs_write/nxs_send/nxs_recv */
long nxf_read(int fd, void *buf, size_t len, int dontwait);
long nxf_write(int fd, const void *buf, size_t len, int dontwait);
int  nxf_close(int fd);
int  nxf_fcntl(int fd, int cmd, long arg);
int  nxf_ioctl(int fd, int request, void *arg);
int  nxf_socketpair(int sv[2], int nonblock);

int  nx_net_sockets_ok(void);      /* socket service came up (nx_net.c)   */

/* errno: host value -> guest (Linux) value (nx_socket.c) */
int  nxs_guest_errno(int host_errno);

#endif
