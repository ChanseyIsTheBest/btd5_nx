/* nx_fdemu.c -- fd registry, emulated fds, epoll, poll and select.
 *
 * WHY THIS EXISTS
 * ---------------
 * The co-op link runs on Boost 1.63 Asio, whose Linux backend is an epoll
 * reactor woken by an eventfd. Horizon has neither, and libnx's poll() refuses
 * any fd that is not a socket. Previously epoll_create/eventfd returned -1,
 * so constructing the co-op io_service threw boost::system::system_error.
 *
 * WHAT IS EMULATED
 * ----------------
 *   eventfd            counter object (Asio's reactor interrupter)
 *   pipe, socketpair   in-process byte rings (Asio's fallback interrupter;
 *                      anything else wanting a local wakeup channel)
 *   /dev/urandom       libnx's CSPRNG; OpenSSL 1.0.2 seeds its RNG from it
 *                      (open, fstat, poll, read) and refuses TLS unseeded
 *   epoll              over libnx poll() for sockets + internal state for the
 *                      emulated fds above, with Linux edge-triggered semantics
 *
 * EDGE-TRIGGERED EPOLL OVER A LEVEL-TRIGGERED POLL
 * ------------------------------------------------
 * Asio registers every descriptor EPOLLET and never removes EPOLLOUT once
 * added. Reporting level-triggered would spin: a connected socket is writable
 * almost always. So each fd carries "armed" bits (ARM_*). An armed event is
 * reported once when the fd is seen in that state, then disarmed. It is
 * re-armed by exactly the things that make a new edge on Linux:
 *   - epoll_ctl ADD / MOD                       (Linux re-evaluates on MOD;
 *                                                Asio's interrupt() relies on it)
 *   - an I/O call returning EAGAIN, or a short read/write  (the fd was drained
 *                                                or filled: the next ready state
 *                                                is a new edge)
 *   - connect() returning EINPROGRESS
 *   - poll()/select() observing an event NOT ready
 *   - a write to an emulated fd (arms the reader), a read (arms the writer),
 *     closing one end (arms the other)
 * Asio 1.10's reactor ops always try the I/O speculatively before queueing and
 * loop until EAGAIN, so every op that waits in the reactor was preceded by an
 * EAGAIN (or a MOD) that armed its event. A spurious report is harmless (the op
 * retries and gets EAGAIN again, which re-arms); a missed one would hang. The
 * rules above only ever err towards spurious.
 *
 * WAITING
 * -------
 * A thread blocked in epoll_wait must notice an eventfd write or epoll_ctl
 * from another thread (that is how Asio posts work to its reactor). With no
 * sockets to wait on it sleeps on a condition variable that every such change
 * signals. With sockets it waits in libnx poll() in short slices (4 ms at
 * first, 12 ms once idle) and re-checks between them, so cross-thread wakeups
 * cost at most one slice of latency and an idle reactor wakes ~80 times/s.
 *
 * MIT license -- see LICENSE.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nx_net.h"
#include "nx_net_internal.h"

#ifdef NXNET_HOST
volatile unsigned nxt_wait_loops;
#define COUNT_WAIT() __atomic_fetch_add(&nxt_wait_loops, 1, __ATOMIC_RELAXED)
/* test-only: open deterministic race windows around epoll_wait's poll */
void (*nxt_before_poll)(void), (*nxt_after_poll)(void);
#define TEST_HOOK(h) do { if (h) h(); } while (0)
#else
#define TEST_HOOK(h) ((void)0)
#define COUNT_WAIT() ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* guest structs                                                       */
/* ------------------------------------------------------------------ */
/* bionic arm64 struct epoll_event is NOT packed (only x86_64 packs it). The
 * host test build runs on x86_64 against glibc, which does pack it. */
#if defined(NXNET_HOST) && defined(__x86_64__)
typedef struct __attribute__((packed)) { uint32_t events; uint64_t data; } GEpollEvent;
_Static_assert(sizeof(GEpollEvent) == 12, "x86_64 epoll_event");
#else
typedef struct { uint32_t events; uint32_t pad_; uint64_t data; } GEpollEvent;
_Static_assert(sizeof(GEpollEvent) == 16, "arm64 epoll_event is 16 bytes");
_Static_assert(offsetof(GEpollEvent, data) == 8, "arm64 epoll_event.data at 8");
#endif

typedef struct { int fd; int16_t events; int16_t revents; } GPollfd;
_Static_assert(sizeof(GPollfd) == 8, "pollfd is 8 bytes");

typedef struct { long tv_sec; long tv_usec; } GTimeval;   /* bionic arm64 */
#define G_FD_SETSIZE 1024
#define G_NFDBITS    (8 * (int)sizeof(unsigned long))

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */
#define NXF_BASE  0x40000000
#define NXF_MAX   256
#define RING_CAP  (16 * 1024)

enum { K_FREE = 0, K_STREAM, K_EVENTFD, K_EPOLL, K_URANDOM };

typedef struct Ring {
  uint8_t *buf;
  size_t cap, head, len;
  int rfd, wfd;                   /* fake slot of each end, -1 once closed */
} Ring;

typedef struct EpReg {
  int fd;
  uint32_t events;
  uint64_t data;
  uint32_t serial;                /* changes on MOD: stale poll results skip */
  uint8_t off;                    /* EPOLLONESHOT fired                      */
} EpReg;

typedef struct Epoll {
  EpReg *regs;
  int n, cap;
} Epoll;

typedef struct Fake {
  int kind;
  uint32_t gen_id;                /* bumps on alloc: detects reuse mid-wait  */
  uint8_t nonblock, is_sockpair;
  uint16_t ep_refs;               /* number of epoll sets holding this fd    */
  uint32_t arm;                   /* guarded by g_lk (unlike socket arms)    */
  Ring *rx, *tx;                  /* K_STREAM                                */
  uint64_t counter; uint8_t sem;  /* K_EVENTFD                               */
  Epoll *ep;                      /* K_EPOLL                                 */
} Fake;

typedef struct {
  uint8_t live, type, nonblock, connected;
  uint16_t ep_refs;
  uint32_t arm;                   /* atomic: armed from any thread           */
  int hostfd;                     /* what readiness is polled on             */
  uint32_t gen;                   /* bumps per socket that holds this number */
} SockSlot;

static nxn_mutex g_lk = NXN_MUTEX_INIT;
static nxn_cond  g_cv = NXN_COND_INIT;
static uint64_t  g_gen;           /* bumps on every change a waiter cares about */
static uint32_t  g_serial = 1;
static Fake      g_fake[NXF_MAX];
static SockSlot  g_sock[NXF_SOCK_MAX];

static void wake_locked(void) { g_gen++; nxn_wake_all(&g_cv); }

static inline int fidx(int fd) {
  return (fd >= NXF_BASE && fd < NXF_BASE + NXF_MAX) ? fd - NXF_BASE : -1;
}
int nxf_is_fake(int fd) { return fidx(fd) >= 0; }

static Fake *fk_locked(int fd) {
  const int i = fidx(fd);
  return (i >= 0 && g_fake[i].kind != K_FREE) ? &g_fake[i] : NULL;
}

static int fk_alloc_locked(int kind) {
  for (int i = 0; i < NXF_MAX; i++) {
    if (g_fake[i].kind != K_FREE) continue;
    const uint32_t gen = g_fake[i].gen_id + 1;
    memset(&g_fake[i], 0, sizeof g_fake[i]);
    g_fake[i].kind = kind;
    g_fake[i].gen_id = gen;
    return i;
  }
  return -1;
}

static void touch_locked(int slot, uint32_t bits) {
  if (slot >= 0 && slot < NXF_MAX && g_fake[slot].kind != K_FREE) g_fake[slot].arm |= bits;
  wake_locked();
}

/* ------------------------------------------------------------------ */
/* socket registry                                                     */
/* ------------------------------------------------------------------ */
static inline SockSlot *ss(int fd) {
  return (fd >= 0 && fd < NXF_SOCK_MAX && __atomic_load_n(&g_sock[fd].live, __ATOMIC_ACQUIRE)) ? &g_sock[fd] : NULL;
}

static void ep_drop_fd_everywhere_locked(int fd);

int nxf_sock_track(int fd, int type, int nonblock, int hostfd) {
  if (fd < 0 || fd >= NXF_SOCK_MAX || hostfd < 0) return -1;
  nxn_lock(&g_lk);
  /* Defensive: if this number was ever closed without passing through
   * close_fake (so nxf_sock_forget never ran), a stale registration could
   * still name it. A new socket starts with none, as on Linux. */
  ep_drop_fd_everywhere_locked(fd);
  SockSlot *s = &g_sock[fd];
  s->type = (uint8_t)type;
  s->nonblock = (uint8_t)(nonblock != 0);
  s->connected = 0;
  s->ep_refs = 0;
  s->hostfd = hostfd;
  s->gen++;
  __atomic_store_n(&s->arm, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&s->live, 1, __ATOMIC_RELEASE);
  nxn_unlock(&g_lk);
  return 0;
}
int  nxf_sock_tracked(int fd)   { return ss(fd) != NULL; }
int  nxf_sock_hostfd(int fd)    { SockSlot *s = ss(fd); return s ? s->hostfd : -1; }
int  nxf_sock_type(int fd)      { SockSlot *s = ss(fd); return s ? s->type : 0; }
int  nxf_sock_nonblock(int fd)  { SockSlot *s = ss(fd); return s ? s->nonblock : 0; }
void nxf_sock_set_nonblock(int fd, int on) { SockSlot *s = ss(fd); if (s) s->nonblock = (uint8_t)(on != 0); }
int  nxf_sock_connected(int fd) { SockSlot *s = ss(fd); return s ? s->connected : 0; }
void nxf_sock_set_connected(int fd, int on) { SockSlot *s = ss(fd); if (s) s->connected = (uint8_t)(on != 0); }

void nxf_arm(int fd, uint32_t bits) {
  SockSlot *s = ss(fd);
  if (!s || !bits) return;
  const uint32_t old = __atomic_fetch_or(&s->arm, bits, __ATOMIC_ACQ_REL);
  if ((old & bits) != bits && s->ep_refs) {
    nxn_lock(&g_lk);
    wake_locked();
    nxn_unlock(&g_lk);
  }
}

/* Remove fd from every epoll set. Linux does this implicitly on close(), and
 * Asio depends on it: deregister_descriptor(..., closing=true) skips
 * EPOLL_CTL_DEL. Without it a reused fd number would inherit a dead
 * registration pointing at a recycled descriptor_state. */
static void ep_drop_fd_everywhere_locked(int fd) {
  for (int i = 0; i < NXF_MAX; i++) {
    if (g_fake[i].kind != K_EPOLL) continue;
    Epoll *ep = g_fake[i].ep;
    for (int k = 0; k < ep->n; ) {
      if (ep->regs[k].fd == fd) { ep->regs[k] = ep->regs[--ep->n]; continue; }
      k++;
    }
  }
}

static void forget_locked(int fd) {
  if (g_sock[fd].ep_refs) ep_drop_fd_everywhere_locked(fd);
  const uint32_t gen = g_sock[fd].gen;          /* survives: tells instances apart */
  memset(&g_sock[fd], 0, sizeof g_sock[fd]);
  g_sock[fd].gen = gen;
  wake_locked();
}

void nxf_sock_forget(int fd) {
  if (fd < 0 || fd >= NXF_SOCK_MAX) return;
  nxn_lock(&g_lk);
  forget_locked(fd);
  nxn_unlock(&g_lk);
}

/* Forget fd only if it still holds the SAME socket a poll was issued for.
 * A POLLNVAL usually means close_fake already forgot it -- and the number may
 * belong to a brand-new socket by now, which must not be touched. */
static int forget_if_same(int fd, uint32_t gen, int hostfd) {
  int done = 0;
  nxn_lock(&g_lk);
  SockSlot *s = &g_sock[fd];
  if (s->live && s->gen == gen && s->hostfd == hostfd) { forget_locked(fd); done = 1; }
  nxn_unlock(&g_lk);
  return done;
}

/* ------------------------------------------------------------------ */
/* readiness of emulated fds (Linux epoll bits)                        */
/* ------------------------------------------------------------------ */
static uint32_t fake_level_locked(const Fake *f) {
  uint32_t lv = 0;
  switch (f->kind) {
    case K_STREAM:
      if (f->rx) {
        if (f->rx->len) lv |= G_EPOLLIN;
        if (f->rx->wfd < 0) lv |= G_EPOLLIN | G_EPOLLHUP | G_EPOLLRDHUP;  /* EOF */
      }
      if (f->tx) {
        if (f->tx->rfd < 0) lv |= G_EPOLLOUT | G_EPOLLERR;                /* EPIPE */
        else if (f->tx->len < f->tx->cap) lv |= G_EPOLLOUT;
      }
      break;
    case K_EVENTFD:
      if (f->counter) lv |= G_EPOLLIN;
      if (f->counter < UINT64_MAX - 1) lv |= G_EPOLLOUT;
      break;
    case K_URANDOM:
      lv = G_EPOLLIN | G_EPOLLOUT;
      break;
    default:
      break;                                   /* nested epoll: unsupported */
  }
  return lv;
}

/* ------------------------------------------------------------------ */
/* emulated fd constructors                                            */
/* ------------------------------------------------------------------ */
static Ring *ring_new(void) {
  Ring *r = (Ring *)calloc(1, sizeof *r);
  if (!r) return NULL;
  r->buf = (uint8_t *)malloc(RING_CAP);
  if (!r->buf) { free(r); return NULL; }
  r->cap = RING_CAP;
  r->rfd = r->wfd = -1;
  return r;
}
static void ring_free(Ring *r) { if (r) { free(r->buf); free(r); } }

int nxf_pipe(int fds[2]) {
  if (!fds) { errno = G_EFAULT; return -1; }
  Ring *r = ring_new();
  if (!r) { errno = G_ENOMEM; return -1; }
  nxn_lock(&g_lk);
  const int a = fk_alloc_locked(K_STREAM);
  const int b = a >= 0 ? fk_alloc_locked(K_STREAM) : -1;
  if (b < 0) {
    if (a >= 0) g_fake[a].kind = K_FREE;
    nxn_unlock(&g_lk);
    ring_free(r);
    errno = G_EMFILE;
    return -1;
  }
  g_fake[a].rx = r; r->rfd = a;
  g_fake[b].tx = r; r->wfd = b;
  nxn_unlock(&g_lk);
  fds[0] = NXF_BASE + a;
  fds[1] = NXF_BASE + b;
  return 0;
}

int nxf_socketpair(int sv[2], int nonblock) {
  if (!sv) { errno = G_EFAULT; return -1; }
  Ring *ab = ring_new(), *ba = ring_new();
  if (!ab || !ba) { ring_free(ab); ring_free(ba); errno = G_ENOMEM; return -1; }
  nxn_lock(&g_lk);
  const int a = fk_alloc_locked(K_STREAM);
  const int b = a >= 0 ? fk_alloc_locked(K_STREAM) : -1;
  if (b < 0) {
    if (a >= 0) g_fake[a].kind = K_FREE;
    nxn_unlock(&g_lk);
    ring_free(ab); ring_free(ba);
    errno = G_EMFILE;
    return -1;
  }
  g_fake[a].tx = ab; g_fake[a].rx = ba;
  g_fake[b].tx = ba; g_fake[b].rx = ab;
  ab->wfd = a; ab->rfd = b;
  ba->wfd = b; ba->rfd = a;
  g_fake[a].is_sockpair = g_fake[b].is_sockpair = 1;
  g_fake[a].nonblock = g_fake[b].nonblock = (uint8_t)(nonblock != 0);
  nxn_unlock(&g_lk);
  sv[0] = NXF_BASE + a;
  sv[1] = NXF_BASE + b;
  return 0;
}

int nxf_eventfd(unsigned initval, int flags) {
  if (flags & ~(G_O_NONBLOCK | G_O_CLOEXEC | G_EFD_SEMAPHORE)) { errno = G_EINVAL; return -1; }
  nxn_lock(&g_lk);
  const int i = fk_alloc_locked(K_EVENTFD);
  if (i >= 0) {
    g_fake[i].counter = initval;
    g_fake[i].sem = (uint8_t)((flags & G_EFD_SEMAPHORE) != 0);
    g_fake[i].nonblock = (uint8_t)((flags & G_O_NONBLOCK) != 0);
  }
  nxn_unlock(&g_lk);
  if (i < 0) { errno = G_EMFILE; return -1; }
  return NXF_BASE + i;
}

int nxf_open_urandom(int flags) {
  nxn_lock(&g_lk);
  const int i = fk_alloc_locked(K_URANDOM);
  if (i >= 0) g_fake[i].nonblock = (uint8_t)((flags & G_O_NONBLOCK) != 0);
  nxn_unlock(&g_lk);
  if (i < 0) { errno = G_EMFILE; return -1; }
  static int told;
  if (!told) { told = 1; nxn_log("[net] /dev/urandom -> libnx CSPRNG (OpenSSL RNG seed)\n"); }
  return NXF_BASE + i;
}

int nxf_epoll_create1(int flags) {
  if (flags & ~G_O_CLOEXEC) { errno = G_EINVAL; return -1; }
  Epoll *ep = (Epoll *)calloc(1, sizeof *ep);
  if (!ep) { errno = G_ENOMEM; return -1; }
  nxn_lock(&g_lk);
  const int i = fk_alloc_locked(K_EPOLL);
  if (i >= 0) g_fake[i].ep = ep;
  nxn_unlock(&g_lk);
  if (i < 0) { free(ep); errno = G_EMFILE; return -1; }
  static int told;
  if (!told) { told = 1; nxn_log("[net] epoll emulation active (first instance fd %d)\n", NXF_BASE + i); }
  return NXF_BASE + i;
}
int nxf_epoll_create(int size) {
  if (size <= 0) { errno = G_EINVAL; return -1; }
  return nxf_epoll_create1(0);
}

/* ------------------------------------------------------------------ */
/* close / fcntl / ioctl / fstat                                       */
/* ------------------------------------------------------------------ */
int nxf_close(int fd) {
  nxn_lock(&g_lk);
  Fake *f = fk_locked(fd);
  if (!f) { nxn_unlock(&g_lk); errno = G_EBADF; return -1; }
  const int me = fidx(fd);
  if (f->ep_refs) ep_drop_fd_everywhere_locked(fd);
  switch (f->kind) {
    case K_STREAM:
      if (f->rx) {                      /* we were the reader */
        Ring *r = f->rx; r->rfd = -1;
        if (r->wfd >= 0) touch_locked(r->wfd, ARM_OUT | ARM_ERR);
        else ring_free(r);
      }
      if (f->tx) {                      /* we were the writer */
        Ring *r = f->tx; r->wfd = -1;
        if (r->rfd >= 0) touch_locked(r->rfd, ARM_IN | ARM_HUP);
        else ring_free(r);
      }
      break;
    case K_EPOLL: {
      Epoll *ep = f->ep;
      for (int k = 0; k < ep->n; k++) {
        const int m = ep->regs[k].fd;
        Fake *mf = fk_locked(m);
        if (mf && mf->ep_refs) mf->ep_refs--;
        else if (m >= 0 && m < NXF_SOCK_MAX && g_sock[m].ep_refs) g_sock[m].ep_refs--;
      }
      free(ep->regs);
      free(ep);
      break;
    }
    default:
      break;
  }
  const uint32_t gen = g_fake[me].gen_id;
  memset(&g_fake[me], 0, sizeof g_fake[me]);
  g_fake[me].gen_id = gen;
  wake_locked();
  nxn_unlock(&g_lk);
  return 0;
}

int nxf_fcntl(int fd, int cmd, long arg) {
  nxn_lock(&g_lk);
  Fake *f = fk_locked(fd);
  int r = 0;
  if (!f) { nxn_unlock(&g_lk); errno = G_EBADF; return -1; }
  switch (cmd) {
    case G_F_GETFL: r = G_O_RDWR | (f->nonblock ? G_O_NONBLOCK : 0); break;
    case G_F_SETFL: f->nonblock = (uint8_t)((arg & G_O_NONBLOCK) != 0); break;
    case G_F_GETFD: case G_F_SETFD: r = 0; break;       /* CLOEXEC: no exec here */
    default: r = -1; errno = G_EINVAL; break;
  }
  nxn_unlock(&g_lk);
  return r;
}

int nxf_ioctl(int fd, int request, void *arg) {
  nxn_lock(&g_lk);
  Fake *f = fk_locked(fd);
  int r = 0;
  if (!f) { nxn_unlock(&g_lk); errno = G_EBADF; return -1; }
  if (request == G_FIONBIO) {
    f->nonblock = (uint8_t)(arg && *(int *)arg != 0);
  } else if (request == G_FIONREAD) {
    int n = 0;
    if (f->kind == K_STREAM && f->rx) n = (int)f->rx->len;
    else if (f->kind == K_EVENTFD) n = f->counter ? 8 : 0;
    if (arg) *(int *)arg = n;
  } else {
    r = -1; errno = G_ENOTTY;
  }
  nxn_unlock(&g_lk);
  return r;
}

int nxf_fstat_info(int fd, uint32_t *mode, uint64_t *ino, uint64_t *rdev) {
  nxn_lock(&g_lk);
  Fake *f = fk_locked(fd);
  if (!f) { nxn_unlock(&g_lk); return -1; }
  uint32_t m = 0600; uint64_t rd = 0;
  switch (f->kind) {
    case K_URANDOM: m = 0020000 | 0666; rd = (1u << 8) | 9u; break;   /* char 1:9 */
    case K_STREAM:  m = (f->is_sockpair ? 0140000 : 0010000) | 0600; break;
    default:        m = 0600; break;                                /* anon inode */
  }
  if (mode) *mode = m;
  if (ino)  *ino = 0x4e580000ull + (uint64_t)fidx(fd);
  if (rdev) *rdev = rd;
  nxn_unlock(&g_lk);
  return 0;
}

/* ------------------------------------------------------------------ */
/* read / write on emulated fds                                        */
/* ------------------------------------------------------------------ */
/* Waits drop the lock; the slot may be closed (and reused) meanwhile, which
 * gen_id detects. Waits are bounded so a stuck peer never wedges a thread. */
#define STILL(i, gen) (g_fake[i].kind != K_FREE && g_fake[i].gen_id == (gen))

long nxf_read(int fd, void *buf, size_t len, int dontwait) {
  const int i = fidx(fd);
  nxn_lock(&g_lk);
  Fake *f = fk_locked(fd);
  if (!f) { nxn_unlock(&g_lk); errno = G_EBADF; return -1; }
  const uint32_t gen = f->gen_id;
  long r = -1;
  switch (f->kind) {
    case K_URANDOM:
      nxn_unlock(&g_lk);
      if (len) nxn_random(buf, len);
      return (long)len;
    case K_EVENTFD:
      if (len < 8) { errno = G_EINVAL; break; }
      while (STILL(i, gen) && g_fake[i].counter == 0) {
        if (g_fake[i].nonblock || dontwait) break;
        nxn_wait_ms(&g_cv, &g_lk, 1000);
      }
      if (!STILL(i, gen)) { errno = G_EBADF; break; }
      if (g_fake[i].counter == 0) { errno = G_EAGAIN; break; }
      {
        uint64_t v;
        if (g_fake[i].sem) { v = 1; g_fake[i].counter--; }
        else { v = g_fake[i].counter; g_fake[i].counter = 0; }
        memcpy(buf, &v, 8);
        g_fake[i].arm |= ARM_OUT;
        wake_locked();
        r = 8;
      }
      break;
    case K_STREAM: {
      Ring *rx = f->rx;
      if (!rx) { errno = G_EBADF; break; }
      if (len == 0) { r = 0; break; }
      while (STILL(i, gen) && rx->len == 0 && rx->wfd >= 0) {
        if (g_fake[i].nonblock || dontwait) break;
        nxn_wait_ms(&g_cv, &g_lk, 1000);
      }
      if (!STILL(i, gen)) { errno = G_EBADF; break; }
      if (rx->len == 0) {
        if (rx->wfd < 0) { r = 0; break; }                 /* EOF */
        errno = G_EAGAIN;
        g_fake[i].arm |= ARM_IN;
        break;
      }
      size_t got = 0;
      uint8_t *dst = (uint8_t *)buf;
      while (got < len && rx->len) {
        const size_t run = rx->cap - rx->head < rx->len ? rx->cap - rx->head : rx->len;
        const size_t n = run < len - got ? run : len - got;
        memcpy(dst + got, rx->buf + rx->head, n);
        rx->head = (rx->head + n) % rx->cap; rx->len -= n; got += n;
      }
      if (rx->len == 0) g_fake[i].arm |= ARM_IN;           /* drained: next data is an edge */
      if (rx->wfd >= 0) touch_locked(rx->wfd, ARM_OUT);    /* space freed for the writer   */
      else wake_locked();
      r = (long)got;
      break;
    }
    default:
      errno = G_EINVAL;
      break;
  }
  nxn_unlock(&g_lk);
  return r;
}

long nxf_write(int fd, const void *buf, size_t len, int dontwait) {
  const int i = fidx(fd);
  nxn_lock(&g_lk);
  Fake *f = fk_locked(fd);
  if (!f) { nxn_unlock(&g_lk); errno = G_EBADF; return -1; }
  const uint32_t gen = f->gen_id;
  long r = -1;
  switch (f->kind) {
    case K_URANDOM:
      r = (long)len;                                       /* entropy "added" */
      break;
    case K_EVENTFD: {
      if (len < 8) { errno = G_EINVAL; break; }
      uint64_t v; memcpy(&v, buf, 8);
      if (v == UINT64_MAX) { errno = G_EINVAL; break; }
      while (STILL(i, gen) && g_fake[i].counter > UINT64_MAX - 1 - v) {
        if (g_fake[i].nonblock || dontwait) break;
        nxn_wait_ms(&g_cv, &g_lk, 1000);
      }
      if (!STILL(i, gen)) { errno = G_EBADF; break; }
      if (g_fake[i].counter > UINT64_MAX - 1 - v) { errno = G_EAGAIN; break; }
      g_fake[i].counter += v;
      touch_locked(i, ARM_IN);                             /* every write is an edge */
      r = 8;
      break;
    }
    case K_STREAM: {
      Ring *tx = f->tx;
      if (!tx) { errno = G_EBADF; break; }
      const uint8_t *src = (const uint8_t *)buf;
      size_t put = 0;
      while (put < len) {
        if (!STILL(i, gen)) { errno = G_EBADF; break; }
        if (tx->rfd < 0) { errno = G_EPIPE; break; }
        if (tx->len == tx->cap) {
          if (g_fake[i].nonblock || dontwait) {
            if (!put) errno = G_EAGAIN;
            g_fake[i].arm |= ARM_OUT;
            break;
          }
          nxn_wait_ms(&g_cv, &g_lk, 1000);
          continue;
        }
        const size_t tail = (tx->head + tx->len) % tx->cap;
        const size_t run = tail >= tx->head ? tx->cap - tail : tx->head - tail;
        const size_t n = run < len - put ? run : len - put;
        memcpy(tx->buf + tail, src + put, n);
        tx->len += n; put += n;
        touch_locked(tx->rfd, ARM_IN);
      }
      r = put ? (long)put : -1;
      break;
    }
    default:
      errno = G_EINVAL;
      break;
  }
  nxn_unlock(&g_lk);
  return r;
}

/* ------------------------------------------------------------------ */
/* epoll                                                               */
/* ------------------------------------------------------------------ */
static int ep_find(const Epoll *ep, int fd) {
  for (int k = 0; k < ep->n; k++) if (ep->regs[k].fd == fd) return k;
  return -1;
}

int nxf_epoll_ctl(int epfd, int op, int fd, void *event) {
  const GEpollEvent *ev = (const GEpollEvent *)event;
  nxn_lock(&g_lk);
  Fake *e = fk_locked(epfd);
  if (!e || e->kind != K_EPOLL) { nxn_unlock(&g_lk); errno = G_EBADF; return -1; }
  if (fd == epfd) { nxn_unlock(&g_lk); errno = G_EINVAL; return -1; }
  Fake *tf = fk_locked(fd);
  SockSlot *ts = tf ? NULL : ss(fd);
  if (!tf && !ts) {
    /* Linux answers EPERM for regular files. Anything else we do not track
     * (a socket made outside this layer, a closed fd) cannot be watched. */
    nxn_unlock(&g_lk);
    errno = (fd >= 0) ? G_EPERM : G_EBADF;
    return -1;
  }
  if (tf && tf->kind == K_EPOLL) { nxn_unlock(&g_lk); errno = G_EINVAL; return -1; }
  Epoll *ep = e->ep;
  const int at = ep_find(ep, fd);
  int rc = 0;
  switch (op) {
    case G_EPOLL_CTL_ADD:
      if (!ev) { errno = G_EFAULT; rc = -1; break; }
      if (at >= 0) { errno = G_EEXIST; rc = -1; break; }
      if (ep->n == ep->cap) {
        const int nc = ep->cap ? ep->cap * 2 : 16;
        EpReg *nr = (EpReg *)realloc(ep->regs, (size_t)nc * sizeof *nr);
        if (!nr) { errno = G_ENOMEM; rc = -1; break; }
        ep->regs = nr; ep->cap = nc;
      }
      ep->regs[ep->n].fd = fd;
      ep->regs[ep->n].events = ev->events;
      ep->regs[ep->n].data = ev->data;
      ep->regs[ep->n].serial = g_serial++;
      ep->regs[ep->n].off = 0;
      ep->n++;
      if (tf) { tf->ep_refs++; tf->arm |= ARM_ALL; }
      else    { ts->ep_refs++; __atomic_fetch_or(&ts->arm, ARM_ALL, __ATOMIC_ACQ_REL); }
      break;
    case G_EPOLL_CTL_MOD:
      if (!ev) { errno = G_EFAULT; rc = -1; break; }
      if (at < 0) { errno = G_ENOENT; rc = -1; break; }
      ep->regs[at].events = ev->events;
      ep->regs[at].data = ev->data;
      ep->regs[at].serial = g_serial++;
      ep->regs[at].off = 0;
      /* Linux re-evaluates readiness on MOD and raises a fresh edge if the fd
       * is ready. Asio's interrupt() is exactly this call. */
      if (tf) tf->arm |= ARM_ALL;
      else    __atomic_fetch_or(&ts->arm, ARM_ALL, __ATOMIC_ACQ_REL);
      break;
    case G_EPOLL_CTL_DEL:
      if (at < 0) { errno = G_ENOENT; rc = -1; break; }
      ep->regs[at] = ep->regs[--ep->n];
      if (tf && tf->ep_refs) tf->ep_refs--;
      if (ts && ts->ep_refs) ts->ep_refs--;
      break;
    default:
      errno = G_EINVAL; rc = -1;
      break;
  }
  if (rc == 0) wake_locked();
  nxn_unlock(&g_lk);
  return rc;
}

/* event-class helpers: an "arm class" covers the related epoll bits */
static uint32_t class_of(uint32_t bits) {
  uint32_t c = 0;
  if (bits & (G_EPOLLIN | G_EPOLLRDNORM | G_EPOLLRDBAND | G_EPOLLRDHUP)) c |= ARM_IN;
  if (bits & (G_EPOLLOUT | G_EPOLLWRNORM | G_EPOLLWRBAND)) c |= ARM_OUT;
  if (bits & G_EPOLLPRI) c |= ARM_PRI;
  if (bits & G_EPOLLERR) c |= ARM_ERR;
  if (bits & G_EPOLLHUP) c |= ARM_HUP;
  return c;
}
static uint32_t bits_of(uint32_t cls) {
  uint32_t b = 0;
  if (cls & ARM_IN)  b |= G_EPOLLIN | G_EPOLLRDNORM | G_EPOLLRDBAND | G_EPOLLRDHUP;
  if (cls & ARM_OUT) b |= G_EPOLLOUT | G_EPOLLWRNORM | G_EPOLLWRBAND;
  if (cls & ARM_PRI) b |= G_EPOLLPRI;
  if (cls & ARM_ERR) b |= G_EPOLLERR;
  if (cls & ARM_HUP) b |= G_EPOLLHUP;
  return b;
}

/* level: Linux epoll bits currently true. armp: the fd's arm word (atomic for
 * sockets, lock-guarded for emulated fds -- atomics are correct for both). */
static uint32_t ep_report(EpReg *r, uint32_t level, uint32_t *armp) {
  if (r->off) return 0;
  const uint32_t interest = (r->events & ~(G_EPOLLET | G_EPOLLONESHOT)) | G_EPOLLERR | G_EPOLLHUP;
  uint32_t lv = level;
  if (lv & G_EPOLLIN)  lv |= G_EPOLLRDNORM;
  if (lv & G_EPOLLOUT) lv |= G_EPOLLWRNORM;
  uint32_t rep = lv & interest;
  if (!rep) return 0;
  if (r->events & G_EPOLLET) {
    const uint32_t armed = __atomic_load_n(armp, __ATOMIC_ACQUIRE);
    const uint32_t fire = class_of(rep) & armed;
    if (!fire) return 0;
    __atomic_fetch_and(armp, ~fire, __ATOMIC_ACQ_REL);
    rep &= bits_of(fire);
  }
  if (r->events & G_EPOLLONESHOT) r->off = 1;
  return rep;
}

/* host poll bits <-> guest epoll bits for the native call */
static short host_events_for(uint32_t cls) {
  short e = 0;
  if (cls & ARM_IN)  e |= POLLIN;
  if (cls & ARM_OUT) e |= POLLOUT;
  if (cls & ARM_PRI) e |= POLLPRI;
  return e;
}
static uint32_t level_from_host(short re) {
  uint32_t lv = 0;
  if (re & (POLLIN | POLLRDNORM)) lv |= G_EPOLLIN;
  if (re & POLLOUT)  lv |= G_EPOLLOUT;
  if (re & POLLPRI)  lv |= G_EPOLLPRI;
  if (re & POLLERR)  lv |= G_EPOLLERR;
  if (re & POLLHUP)  lv |= G_EPOLLHUP;
  if (re & POLLNVAL) lv |= G_EPOLLERR | G_EPOLLHUP;   /* closed underneath us */
  return lv;
}

static int pick_slice(int timeout, uint64_t waited) {
  int s = waited < 250 ? 4 : 12;
  if (timeout >= 0) {
    const int left = timeout - (int)waited;
    if (left < s) s = left < 0 ? 0 : left;
  }
  return s;
}

/* How long to sleep when NO socket is involved: every change a waiter cares
 * about signals g_cv, so there is nothing to poll for -- wait for the whole
 * remaining time (bounded, as a safety net against a lost signal). */
static int idle_wait_ms(int timeout, uint64_t waited) {
  if (timeout < 0) return 1000;
  const int left = timeout - (int)waited;
  return left <= 0 ? 0 : (left > 1000 ? 1000 : left);
}

/* libnx's poll() fails the WHOLE call when any member is not a live socket
 * (_socketGetFd), where Linux reports POLLNVAL for that fd alone. Re-poll each
 * fd by itself, non-blocking, to separate the dead from the live. Live fds get
 * their own result in revents. Returns the number of dead fds. */
static int isolate_dead(struct pollfd *p, int n, uint8_t *dead) {
  int nd = 0;
  for (int k = 0; k < n; k++) {
    dead[k] = 0;
    if (p[k].fd < 0) { p[k].revents = 0; continue; }
    struct pollfd one = p[k];
    one.revents = 0;
    if (NXN_POLL(&one, 1, 0) < 0) { dead[k] = 1; p[k].revents = 0; nd++; }
    else p[k].revents = one.revents;
  }
  return nd;
}

typedef struct { int fd; uint32_t serial; uint32_t sgen; } Snap;

/* Turn poll() results into epoll events (caller holds g_lk). Registrations
 * changed by MOD/DEL while we were polling are skipped: MOD re-armed them and
 * the next pass re-evaluates. */
static int collect_locked(int ei, uint32_t egen, const struct pollfd *pfd, const Snap *snap, int npoll,
                          GEpollEvent *out, int nout, int maxevents, uint8_t *dead) {
  if (!STILL(ei, egen) || g_fake[ei].kind != K_EPOLL) return nout;
  Epoll *ep = g_fake[ei].ep;
  for (int k = 0; k < npoll && nout < maxevents; k++) {
    if (!pfd[k].revents) continue;
    if (pfd[k].revents & POLLNVAL) { dead[k] = 1; continue; }   /* closed: dropped below */
    const int at = ep_find(ep, snap[k].fd);
    if (at < 0 || ep->regs[at].serial != snap[k].serial) continue;
    SockSlot *s = ss(snap[k].fd);
    if (!s || s->gen != snap[k].sgen) continue;
    const uint32_t rep = ep_report(&ep->regs[at], level_from_host(pfd[k].revents), &s->arm);
    if (rep) { out[nout].events = rep; out[nout].data = ep->regs[at].data; nout++; }
  }
  return nout;
}

int nxf_epoll_wait(int epfd, void *events, int maxevents, int timeout) {
  if (!events || maxevents <= 0) { errno = G_EINVAL; return -1; }
  GEpollEvent *out = (GEpollEvent *)events;
  const uint64_t start = nxn_now_ms();

  Snap snap_stack[64];
  struct pollfd pfd_stack[64];
  uint8_t dead_stack[64];
  Snap *snap = snap_stack;
  struct pollfd *pfd = pfd_stack;
  uint8_t *dead = dead_stack;
  int cap = 64, result = 0;

  nxn_lock(&g_lk);
  Fake *e = fk_locked(epfd);
  if (!e || e->kind != K_EPOLL) { nxn_unlock(&g_lk); errno = G_EBADF; return -1; }
  const int ei = fidx(epfd);
  const uint32_t egen = e->gen_id;
  nxn_unlock(&g_lk);

  for (;;) {
    int nout = 0, npoll = 0;
    uint64_t gen0;

    /* 1. emulated fds are evaluated directly; sockets are queued for poll() */
    nxn_lock(&g_lk);
    if (!STILL(ei, egen) || g_fake[ei].kind != K_EPOLL) { nxn_unlock(&g_lk); errno = G_EBADF; result = -1; break; }
    Epoll *ep = g_fake[ei].ep;
    gen0 = g_gen;
    if (ep->n > cap) {
      Snap *ns = (Snap *)malloc((size_t)ep->n * sizeof *ns);
      struct pollfd *np = (struct pollfd *)malloc((size_t)ep->n * sizeof *np);
      uint8_t *nd = (uint8_t *)malloc((size_t)ep->n);
      if (!ns || !np || !nd) { free(ns); free(np); free(nd); nxn_unlock(&g_lk); errno = G_ENOMEM; result = -1; break; }
      if (snap != snap_stack) { free(snap); free(pfd); free(dead); }
      snap = ns; pfd = np; dead = nd; cap = ep->n;
    }
    for (int k = 0; k < ep->n; k++) {
      EpReg *r = &ep->regs[k];
      if (r->off) continue;
      Fake *tf = fk_locked(r->fd);
      if (tf) {
        if (nout >= maxevents) continue;
        const uint32_t rep = ep_report(r, fake_level_locked(tf), &tf->arm);
        if (rep) { out[nout].events = rep; out[nout].data = r->data; nout++; }
        continue;
      }
      SockSlot *s = ss(r->fd);
      if (!s) continue;                             /* closed: registration gone next */
      uint32_t cls = class_of((r->events & ~(G_EPOLLET | G_EPOLLONESHOT)) | G_EPOLLERR | G_EPOLLHUP);
      if (r->events & G_EPOLLET) {
        cls &= __atomic_load_n(&s->arm, __ATOMIC_ACQUIRE);
        if (!cls) continue;                         /* nothing that could fire: don't poll */
      }
      pfd[npoll].fd = s->hostfd;
      pfd[npoll].events = host_events_for(cls);     /* ERR/HUP are always reported */
      pfd[npoll].revents = 0;
      snap[npoll].fd = r->fd;
      snap[npoll].serial = r->serial;
      snap[npoll].sgen = s->gen;
      npoll++;
    }
    nxn_unlock(&g_lk);

    const uint64_t waited = nxn_now_ms() - start;
    const int slice = (nout || timeout == 0) ? 0 : pick_slice(timeout, waited);

    if (npoll) {
      /* 2. sockets */
      COUNT_WAIT();
      memset(dead, 0, (size_t)npoll);
      TEST_HOOK(nxt_before_poll);
      int pr = NXN_POLL(pfd, (nfds_t)npoll, slice);
      TEST_HOOK(nxt_after_poll);
      if (pr < 0) {
        /* Should not happen with bsd descriptors (a closed one is POLLNVAL),
         * but if the call fails as a whole, one bad member must not starve
         * every other socket in the set: find it. */
        const int nd = isolate_dead(pfd, npoll, dead);
        if (!nd && slice > 0) nxn_sleep_ms(1);      /* transient: don't spin */
        pr = 1;                                      /* live results are in pfd[] */
      }
      if (pr > 0) {
        nxn_lock(&g_lk);
        nout = collect_locked(ei, egen, pfd, snap, npoll, out, nout, maxevents, dead);
        nxn_unlock(&g_lk);
      }
      /* Linux drops a closed fd from epoll sets by itself. Normally close_fake
       * did that already; this covers a socket closed any other way. */
      for (int k = 0; k < npoll; k++)
        if (dead[k] && forget_if_same(snap[k].fd, snap[k].sgen, pfd[k].fd))
          nxn_log("[net] epoll: fd %d is no longer a live socket -- dropped from the set\n", snap[k].fd);
    } else if (!nout && timeout != 0) {
      /* 3. nothing to poll: sleep until something changes */
      const int w = idle_wait_ms(timeout, waited);
      if (w > 0) {
        COUNT_WAIT();
        nxn_lock(&g_lk);
        if (g_gen == gen0) nxn_wait_ms(&g_cv, &g_lk, w);
        nxn_unlock(&g_lk);
      }
    }

    if (nout) { result = nout; break; }
    if (timeout == 0) break;
    if (timeout > 0 && nxn_now_ms() - start >= (uint64_t)timeout) break;
  }
  if (snap != snap_stack) { free(snap); free(pfd); free(dead); }
  return result;
}

/* ------------------------------------------------------------------ */
/* poll / select                                                       */
/* ------------------------------------------------------------------ */
#define POLL_MAX 256

static short guest_to_host_poll(short ev) {
  short h = 0;
  if (ev & (G_POLLIN | G_POLLRDNORM))  h |= POLLIN;
  if (ev & (G_POLLOUT | G_POLLWRNORM)) h |= POLLOUT;
  if (ev & (G_POLLPRI | G_POLLRDBAND)) h |= POLLPRI;
  return h;      /* never pass POLLRDHUP (0x2000): libnx reads it as POLLINIGNEOF */
}
static short host_to_guest_poll(short re, short want) {
  short g = 0;
  if (re & (POLLIN | POLLRDNORM)) g |= G_POLLIN | G_POLLRDNORM;
  if (re & POLLOUT) g |= G_POLLOUT | G_POLLWRNORM;
  if (re & POLLPRI) g |= G_POLLPRI;
  if (re & POLLHUP) g |= G_POLLHUP;
  if (re & POLLERR) g |= G_POLLERR;
  if (re & POLLNVAL) g |= G_POLLNVAL;
  return (short)(g & (want | G_POLLERR | G_POLLHUP | G_POLLNVAL));
}
static short fake_poll_revents_locked(const Fake *f, short want) {
  const uint32_t lv = fake_level_locked(f);
  short g = 0;
  if (lv & G_EPOLLIN)  g |= G_POLLIN | G_POLLRDNORM;
  if (lv & G_EPOLLOUT) g |= G_POLLOUT | G_POLLWRNORM;
  if (lv & G_EPOLLHUP) g |= G_POLLHUP;
  if (lv & G_EPOLLERR) g |= G_POLLERR;
  if (lv & G_EPOLLRDHUP) g |= G_POLLRDHUP;
  return (short)(g & (want | G_POLLERR | G_POLLHUP | G_POLLNVAL));
}

/* bionic: int poll(struct pollfd*, nfds_t, int) with nfds_t = unsigned int.
 * The count arrives in a W register; its upper half is unspecified, so it
 * must be read as 32 bits. */
int nxf_poll(void *fdsv, unsigned int nfds, int timeout) {
  GPollfd *fds = (GPollfd *)fdsv;
  if (nfds && !fds) { errno = G_EFAULT; return -1; }
  if (nfds > POLL_MAX) { errno = G_EINVAL; return -1; }

  struct pollfd nat[POLL_MAX];
  int map[POLL_MAX];
  uint8_t dead[POLL_MAX];
  int nn = 0, nfake = 0, nother = 0;
  for (unsigned i = 0; i < nfds; i++) {
    fds[i].revents = 0;
    const int fd = fds[i].fd;
    if (fd < 0) continue;
    if (nxf_is_fake(fd)) { nfake++; continue; }
    const int hfd = nxf_sock_hostfd(fd);
    if (hfd >= 0) {
      nat[nn].fd = hfd; nat[nn].events = guest_to_host_poll(fds[i].events); nat[nn].revents = 0;
      map[nn++] = (int)i;
    } else {
      nother++;                    /* a regular file: POSIX says always ready */
    }
  }

  const uint64_t start = nxn_now_ms();
  for (;;) {
    uint64_t gen0;

    nxn_lock(&g_lk);
    gen0 = g_gen;
    for (unsigned i = 0; nfake && i < nfds; i++) {
      if (!nxf_is_fake(fds[i].fd)) continue;
      Fake *f = fk_locked(fds[i].fd);
      fds[i].revents = f ? fake_poll_revents_locked(f, fds[i].events) : G_POLLNVAL;
    }
    nxn_unlock(&g_lk);
    for (unsigned i = 0; nother && i < nfds; i++) {
      const int fd = fds[i].fd;
      if (fd < 0 || nxf_is_fake(fd) || nxf_sock_tracked(fd)) continue;
      fds[i].revents = (short)(fds[i].events & (G_POLLIN | G_POLLOUT | G_POLLRDNORM | G_POLLWRNORM));
    }
    /* Sockets closed (by another thread) since the last pass: POLLNVAL, like
     * Linux, and leave them out of the native call (fd -1 is ignored). */
    int live = 0;
    for (int k = 0; k < nn; k++) {
      if (nat[k].fd >= 0 && nxf_sock_hostfd(fds[map[k]].fd) != nat[k].fd) { nat[k].fd = -1; fds[map[k]].revents = G_POLLNVAL; }
      if (nat[k].fd >= 0) live++;
    }
    int ready = 0;
    for (unsigned i = 0; i < nfds; i++) if (fds[i].revents) ready++;

    const uint64_t waited = nxn_now_ms() - start;
    int slice;
    if (ready || timeout == 0) slice = 0;
    else if (nfake) slice = pick_slice(timeout, waited);
    else if (timeout < 0) slice = 500;             /* sockets only: long, but */
    else {                                         /* bounded so a closed fd  */
      const int left = timeout - (int)waited;      /* cannot pin us forever   */
      slice = left < 0 ? 0 : (left > 500 ? 500 : left);
    }

    if (live) {
      for (int k = 0; k < nn; k++) nat[k].revents = 0;
      COUNT_WAIT();
      if (NXN_POLL(nat, (nfds_t)nn, slice) < 0) {
        const int e = errno;
        const int nd = isolate_dead(nat, nn, dead);
        if (!nd) {
          /* nothing identifiably dead: a genuine failure of the call */
          if (slice > 0) nxn_sleep_ms(1);
          if (timeout == 0) { errno = nxs_guest_errno(e); return -1; }
        }
        for (int k = 0; k < nn; k++)
          if (dead[k]) { nat[k].fd = -1; fds[map[k]].revents = G_POLLNVAL; }
      }
      for (int k = 0; k < nn; k++) {
        if (nat[k].fd < 0) continue;
        GPollfd *g = &fds[map[k]];
        g->revents = host_to_guest_poll(nat[k].revents, g->events);
        /* observed NOT ready: whatever comes next is an edge for epoll */
        const uint32_t asked = class_of((uint32_t)(uint16_t)g->events) & (ARM_IN | ARM_OUT | ARM_PRI);
        const uint32_t got = class_of(level_from_host(nat[k].revents));
        if (asked & ~got) nxf_arm(g->fd, asked & ~got);
      }
      ready = 0;
      for (unsigned i = 0; i < nfds; i++) if (fds[i].revents) ready++;
    } else if (!ready && timeout != 0) {
      /* no live socket: fake fds signal g_cv on every change */
      const int w = nfake ? idle_wait_ms(timeout, waited) : slice;
      if (w > 0) {
        COUNT_WAIT();
        nxn_lock(&g_lk);
        if (g_gen == gen0) nxn_wait_ms(&g_cv, &g_lk, w);
        nxn_unlock(&g_lk);
      }
    }

    if (ready || timeout == 0) return ready;
    if (timeout > 0 && nxn_now_ms() - start >= (uint64_t)timeout) return 0;
  }
}

/* bionic fd_set: 1024 bits in unsigned long[16]; answered through nxf_poll */
static inline int gfd_isset(int fd, const unsigned long *s) {
  return (int)((s[fd / G_NFDBITS] >> (fd % G_NFDBITS)) & 1ul);
}
static inline void gfd_set(int fd, unsigned long *s) { s[fd / G_NFDBITS] |= 1ul << (fd % G_NFDBITS); }

int nxf_select(int nfds, void *rdv, void *wrv, void *exv, void *tov) {
  unsigned long *rd = (unsigned long *)rdv, *wr = (unsigned long *)wrv, *ex = (unsigned long *)exv;
  const GTimeval *tv = (const GTimeval *)tov;
  if (nfds < 0) { errno = G_EINVAL; return -1; }
  if (nfds > G_FD_SETSIZE) nfds = G_FD_SETSIZE;
  GPollfd pf[POLL_MAX];
  int k = 0;
  for (int fd = 0; fd < nfds; fd++) {
    const int r = rd && gfd_isset(fd, rd), w = wr && gfd_isset(fd, wr), x = ex && gfd_isset(fd, ex);
    if (!(r || w || x)) continue;
    if (k >= POLL_MAX) { errno = G_EINVAL; return -1; }
    pf[k].fd = fd;
    pf[k].events = (int16_t)((r ? G_POLLIN : 0) | (w ? G_POLLOUT : 0) | (x ? G_POLLPRI : 0));
    pf[k].revents = 0;
    k++;
  }
  int timeout = -1;
  if (tv) {
    if (tv->tv_sec < 0 || tv->tv_usec < 0) { errno = G_EINVAL; return -1; }
    const long long ms = (long long)tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
    timeout = ms > 0x7fffffff ? 0x7fffffff : (int)ms;
  }
  const int rc = nxf_poll(pf, (unsigned)k, timeout);
  if (rc < 0) return -1;
  const size_t words = (size_t)(G_FD_SETSIZE / G_NFDBITS);
  if (rd) memset(rd, 0, words * sizeof(unsigned long));
  if (wr) memset(wr, 0, words * sizeof(unsigned long));
  if (ex) memset(ex, 0, words * sizeof(unsigned long));
  int count = 0;
  for (int i = 0; i < k; i++) {
    const int16_t re = pf[i].revents, ev = pf[i].events;
    if (rd && (ev & G_POLLIN)  && (re & (G_POLLIN | G_POLLHUP | G_POLLERR | G_POLLNVAL))) { gfd_set(pf[i].fd, rd); count++; }
    if (wr && (ev & G_POLLOUT) && (re & (G_POLLOUT | G_POLLERR | G_POLLNVAL)))            { gfd_set(pf[i].fd, wr); count++; }
    if (ex && (ev & G_POLLPRI) && (re & G_POLLPRI))                                       { gfd_set(pf[i].fd, ex); count++; }
  }
  return count;
}

/* __FD_SET_chk(fd, set, setsize): bionic's fortified FD_SET. The previous
 * stub did nothing, so curl's curl_multi_fdset() + select() handed select an
 * empty set and HTTP requests never progressed. */
void nxf_fd_set_chk(int fd, void *set, size_t setsize) {
  if (!set || fd < 0 || fd >= G_FD_SETSIZE || setsize < (size_t)(G_FD_SETSIZE / 8)) {
    static int told;
    if (!told) { told = 1; nxn_log("[net] FD_SET(%d) out of range -- ignored\n", fd); }
    return;
  }
  gfd_set(fd, (unsigned long *)set);
}
