/* libc_shim.c -- bionic-compatible libc wrappers for the 2.1.131 libs
 *
 * libGame.so and libc++_shared.so are linked against bionic. Where the
 * bionic and newlib ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches
 * is passed straight through from imports.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"
#include "obb.h"

// Redirect the engine's root-relative data files (it opens absolute paths like
// "/fusion.registry", "/highscores.lua") into the game directory so saves live
// with the game instead of cluttering the SD-card root. Defined below, next to
// the asset base it uses. Real pseudo-paths (/proc, /dev, /sys) are left alone.
static const char *remap_data_path(const char *path, char *buf, size_t bufsz);
static int path_would_crash_newlib(const char *path);

/* ---------------------------------------------------------------------------
 * newlib file-table lock
 *
 * devkitPro's newlib is NOT safe against concurrent open/close from multiple
 * threads: _open_r/_close_r dispatch through devoptab_list[dev] and the fd
 * table without holding a lock, so two engine worker threads opening assets at
 * the same time can observe devoptab_list[dev] == NULL and fault dereferencing
 * ->open_r (+0x10) / ->stat_r (+0x40) / ->write_r (+0x20). That is exactly the
 * Data Abort (Fault Address 0x0, offset 0x10/0x20/0x40) this port kept hitting
 * during nativeLoad, and it became reliably reproducible once the condvar fix
 * let the worker threads actually run in parallel.
 *
 * Serialising the fd-table-mutating entry points (open/close family) removes the
 * race. Reads/writes on an already-open FILE are left unlocked -- newlib does
 * lock those per-stream, and locking them here would serialise all asset I/O.
 * Recursive so a shim that calls another shim can't self-deadlock.
 * ------------------------------------------------------------------------- */
static RMutex g_file_lock;
#define FILE_LOCK()   rmutexLock(&g_file_lock)
#define FILE_UNLOCK() rmutexUnlock(&g_file_lock)

/* Locked wrappers around the newlib entry points that resolve a path to a
 * device (FindDevice) or mutate the fd table. Serialising these removes the
 * concurrent-access corruption in newlib's device layer (devoptab_list[dev]
 * transiently NULL -> Data Abort in _open_r/_stat_r). Reads/writes on an
 * already-open FILE are deliberately NOT wrapped -- newlib locks those per
 * stream and serialising them would throttle all asset I/O. */
static inline FILE *fopen_L(const char *p, const char *m) {
  FILE_LOCK(); FILE *f = fopen(p, m); FILE_UNLOCK(); return f;
}
static inline int open_L(const char *p, int fl, int mode) {
  FILE_LOCK(); int fd = open(p, fl, mode); FILE_UNLOCK(); return fd;
}
static inline int stat_L(const char *p, struct stat *s) {
  FILE_LOCK(); int r = stat(p, s); FILE_UNLOCK(); return r;
}
static inline int lstat_L(const char *p, struct stat *s) {
  FILE_LOCK(); int r = lstat(p, s); FILE_UNLOCK(); return r;
}
static inline int access_L(const char *p, int m) {
  FILE_LOCK(); int r = access(p, m); FILE_UNLOCK(); return r;
}
static inline int remove_L(const char *p) {
  FILE_LOCK(); int r = remove(p); FILE_UNLOCK(); return r;
}
static inline int unlink_L(const char *p) {
  FILE_LOCK(); int r = unlink(p); FILE_UNLOCK(); return r;
}
static inline int fclose_L(FILE *f) {
  FILE_LOCK(); int r = fclose(f); FILE_UNLOCK(); return r;
}

/* CRITICAL: every function that ALLOCATES or RELEASES a newlib file handle must
 * hold the same lock. devkitPro's __alloc_handle()/__release_handle() are NOT
 * thread-safe: alloc scans the handle table for a free slot while release
 * mutates it. The engine calls close()/opendir()/closedir() from worker threads,
 * so an unlocked close racing a locked open let a handle be read mid-mutation ->
 * handle->device is garbage -> devoptab_list[dev] == NULL -> Data Abort
 * dereferencing ->open_r (+0x10) / ->write_r (+0x20) / ->seek_r (+0x30) /
 * ->stat_r (+0x40). That is the family of crashes this port kept hitting.
 * Reads/writes on an ALREADY-open handle are left unlocked: they only touch
 * their own slot, and newlib locks per-stream. */
int close_fake(int fd) {
  FILE_LOCK(); int r = close(fd); FILE_UNLOCK(); return r;
}
FILE *fdopen_fake(int fd, const char *mode) {
  FILE_LOCK(); FILE *f = fdopen(fd, mode); FILE_UNLOCK(); return f;
}
FILE *freopen_fake(const char *path, const char *mode, FILE *f) {
  char rbuf[600];
  if (path) {
    path = remap_data_path(path, rbuf, sizeof rbuf);
    if (path_would_crash_newlib(path)) return NULL;
  }
  FILE_LOCK(); FILE *r = freopen(path, mode, f); FILE_UNLOCK(); return r;
}
FILE *tmpfile_fake(void) {
  FILE_LOCK(); FILE *f = tmpfile(); FILE_UNLOCK(); return f;
}
int mkstemp_fake(char *tmpl) {
  FILE_LOCK(); int fd = mkstemp(tmpl); FILE_UNLOCK(); return fd;
}



// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}

char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}

char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}

size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen;
  return strlen(s);
}

char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncat(dst, src, n);
}

char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncpy(dst, src, n);
}

char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) {
  (void)dstlen; (void)srclen;
  return strncpy(dst, src, n);
}

int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsnprintf(s, maxlen, fmt, va);
}

int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsprintf(s, fmt, va);
}

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  value[0] = '\0';
  return 0;
}

unsigned long getauxval_fake(unsigned long type) {
  (void)type;
  return 0;
}

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178
#define ARM64_SYS_FUTEX  98

// futex(2) emulation over libnx mutex+condvar.
//
// THE DEADLOCK: bionic implements C++ std::mutex / std::condition_variable /
// atomic waits on top of syscall(SYS_futex). We returned ENOSYS, so every futex
// WAIT failed instead of blocking and every futex WAKE failed to wake anyone --
// waiters parked forever and the engine's threading never made progress. That is
// the "reaches STAGE 14/15 then freezes with no crash report" hang.
//
// Wait queues are hashed by uaddr into buckets; FUTEX_WAKE wakes the whole bucket
// (waiters re-check *uaddr, so over-broad wakes are harmless). The bucket mutex
// serializes compare-and-sleep against wakers so no wake is lost. Infinite waits
// are capped at 16ms and return as if woken: under load a wake can be missed, and
// a bounded re-poll recovers safely since the waiter re-checks *uaddr anyway.
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK    0x7f  // strip FUTEX_PRIVATE_FLAG(128)/CLOCK_REALTIME(256)
#define FUTEX_BUCKETS     256

static Mutex   futex_lock[FUTEX_BUCKETS];   // libnx Mutex/CondVar are u32; 0 == ready
static CondVar futex_cond[FUTEX_BUCKETS];

static long futex_impl(volatile int32_t *uaddr, int op, int val, const struct timespec *to) {
  if (!uaddr) { errno = EINVAL; return -1; }
  const int cmd = op & FUTEX_CMD_MASK;
  const unsigned h = (unsigned)(((uintptr_t)uaddr >> 4) & (FUTEX_BUCKETS - 1));

  if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
    long ret = 0;
    mutexLock(&futex_lock[h]);
    if (*uaddr != val) {
      errno = EAGAIN; ret = -1;          // value already changed: don't sleep
    } else if (to) {
      const u64 ns = (u64)to->tv_sec * 1000000000ULL + (u64)to->tv_nsec;
      if (R_FAILED(condvarWaitTimeout(&futex_cond[h], &futex_lock[h], ns))) {
        errno = BIONIC_ETIMEDOUT; ret = -1;
      }
    } else {
#if COND_WAIT_CAP_MS <= 0
      // No cap: block until FUTEX_WAKE actually signals us. Zero idle cpu.
      // (UINT64_MAX == wait forever; same call we already use, so no API risk.)
      condvarWaitTimeout(&futex_cond[h], &futex_lock[h], UINT64_MAX);
#else
      // capped "infinite" wait: caller re-checks *uaddr, so a bounded re-poll
      // is safe and recovers any missed wake instead of hanging forever.
      condvarWaitTimeout(&futex_cond[h], &futex_lock[h],
                         (u64)COND_WAIT_CAP_MS * 1000000ULL);
#endif
    }
    mutexUnlock(&futex_lock[h]);
    return ret;
  }

  if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
    mutexLock(&futex_lock[h]);
    condvarWakeAll(&futex_cond[h]);
    mutexUnlock(&futex_lock[h]);
    return val > 0 ? val : 0;            // approximate count woken
  }

  errno = BIONIC_ENOSYS;
  return -1;
}

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID:
      return gettid_fake();
    case ARM64_SYS_FUTEX: {
      va_list va; va_start(va, number);
      volatile int32_t *uaddr    = va_arg(va, volatile int32_t *);
      const int op               = va_arg(va, int);
      const int val              = va_arg(va, int);
      const struct timespec *to  = va_arg(va, const struct timespec *);
      va_end(va);
      return futex_impl(uaddr, op, val, to);
    }
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = BIONIC_ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

int sched_get_priority_max_fake(int policy) {
  (void)policy;
  return 0;
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("abort message: %s\n", msg ? msg : "(null)");
}

size_t __ctype_get_mb_cur_max_fake(void) {
  return 1;
}

int __register_atfork_fake(void) {
  return 0;
}

int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) {
  // threads never exit cleanly in this port; leak instead of running dtors
  (void)fn; (void)arg; (void)dso;
  return 0;
}

// bionic sysconf constants
#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
      return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:
      return 3;
    case BIONIC_SC_PHYS_PAGES:
      return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default:
      debugPrintf("libc: sysconf(%d) -> -1\n", name);
      return -1;
  }
}

long pathconf_fake(const char *path, int name) {
  (void)path; (void)name;
  return -1;
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000

static int convert_open_flags(int flags) {
  int out = flags & 3; // O_RDONLY/O_WRONLY/O_RDWR match
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

int open_fake(const char *path, int flags, ...) {
  char rbuf[600];
  path = remap_data_path(path, rbuf, sizeof rbuf);
  if (path_would_crash_newlib(path)) {
    debugPrintf("open(%s) -> refused (unsafe device-root path)\n", path ? path : "(null)");
    return -1;
  }
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  int fd = open_L(path, convert_open_flags(flags), mode);
  debugPrintf("open(%s, 0x%x) -> %d\n", path, flags, fd);
  return fd;
}

int openat_fake(int dirfd, const char *path, int flags, ...) {
  (void)dirfd; // assume AT_FDCWD or absolute paths
  char rbuf[600];
  path = remap_data_path(path, rbuf, sizeof rbuf);
  if (path_would_crash_newlib(path)) return -1;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  return open_L(path, convert_open_flags(flags), mode);
}

int unlinkat_fake(int dirfd, const char *path, int flags) {
  (void)dirfd; (void)flags;
  char rbuf[600];
  path = remap_data_path(path, rbuf, sizeof rbuf);
  if (path_would_crash_newlib(path)) return -1;
  return unlink(path);
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  char rbuf[600];
  path = remap_data_path(path, rbuf, sizeof rbuf);
  if (path_would_crash_newlib(path)) { errno = ENOENT; return -1; }
  struct stat real;
  const int ret = stat_L(path, &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

int lstat_fake(const char *path, struct bionic_stat *st) {
  return stat_fake(path, st);
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

/* Each DIR gets its own dirent buffer (see readdir_fake for why not __thread).
 * FakeDir is what opendir_fake hands the engine; dir_ok() validates it so a
 * stale/wild DIR* fails safe instead of faulting. */
#define FAKEDIR_MAGIC 0xD1E2C7A9u

typedef struct {
  uint32_t magic;
  DIR *d;
  struct bionic_dirent ent;
} FakeDir;

static FakeDir *dir_ok(void *p) {
  FakeDir *f = p;
  return (f && f->magic == FAKEDIR_MAGIC && f->d) ? f : NULL;
}

void *opendir_fake(const char *path) {
  char rbuf[600];
  path = remap_data_path(path, rbuf, sizeof rbuf);
  if (path_would_crash_newlib(path)) return NULL;

  FILE_LOCK();
  DIR *d = opendir(path);            /* allocates a handle: must hold the lock */
  FILE_UNLOCK();
  if (!d) return NULL;

  FakeDir *f = calloc(1, sizeof(*f));
  if (!f) { FILE_LOCK(); closedir(d); FILE_UNLOCK(); return NULL; }
  f->magic = FAKEDIR_MAGIC;
  f->d = d;
  return f;
}

int closedir_fake(void *p) {
  FakeDir *f = dir_ok(p);
  if (!f) return -1;
  FILE_LOCK();
  int r = closedir(f->d);            /* releases a handle: must hold the lock */
  FILE_UNLOCK();
  f->d = NULL;
  f->magic = 0;                      /* poison: a later use fails dir_ok() */
  free(f);
  return r;
}


void *readdir_fake(void *dirp) {
  /* Per-DIR storage, NOT a shared static and NOT __thread.
   *
   * The original used one `static struct bionic_dirent` for all callers; the
   * engine calls readdir() from worker threads, so concurrent callers clobbered
   * each other's result. __thread is NOT an option here either: we hijack
   * TPIDR_EL0 for the engine's bionic TLS block, which is the same register the
   * AArch64 C TLS model uses -- a __thread variable would read/write into that
   * 0x400-byte block and corrupt the stack guard. So each DIR carries its own
   * dirent (see FakeDir), which is what POSIX per-DIR semantics want anyway. */
  FakeDir *fd = dir_ok(dirp);
  if (!fd) return NULL;

  FILE_LOCK();
  struct dirent *e = readdir(fd->d);     /* device layer: same lock as open/close */
  FILE_UNLOCK();

  if (!e)
    return NULL;
  memset(&fd->ent, 0, sizeof(fd->ent));
  fd->ent.d_ino = e->d_ino;
  fd->ent.d_reclen = sizeof(fd->ent);
  fd->ent.d_type = e->d_type;
  snprintf(fd->ent.d_name, sizeof(fd->ent.d_name), "%s", e->d_name);
  return &fd->ent;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base;
  return (void *)1;
}

void freelocale_fake(void *loc) {
  (void)loc;
}

void *uselocale_fake(void *loc) {
  (void)loc;
  return (void *)1;
}

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha)
WRAP_ISW_L(iswblank)
WRAP_ISW_L(iswcntrl)
WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower)
WRAP_ISW_L(iswprint)
WRAP_ISW_L(iswpunct)
WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper)
WRAP_ISW_L(iswxdigit)
WRAP_ISW_L(towlower)
WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) {
  (void)loc;
  return strcoll(a, b);
}

size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) {
  (void)loc;
  return strxfrm(dst, src, n);
}

size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) {
  (void)loc;
  return strftime(s, max, fmt, (const struct tm *)tm);
}

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc;
  return strtold(s, end);
}

long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoll(s, end, base);
}

unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoull(s, end, base);
}

int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) {
  (void)loc;
  return wcscoll(a, b);
}

size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) {
  (void)loc;
  return wcsxfrm(dst, src, n);
}

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  // ascii-ish naive conversion
  size_t i = 0;
  const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (unsigned char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!resolved)
    resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}

int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

int statvfs_fake(const char *path, void *buf) {
  (void)path;
  memset(buf, 0, 0x70);
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr)
// libc++_shared initializes std::cout/cerr against &__sF[1]/&__sF[2];
// these wrappers absorb accesses to those fake FILEs and forward the rest
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c too

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total);
    buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}

// --- I/O timing instrumentation (temporary, for the audio-stall hunt) --------
static double mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

// libnx's gettimeofday resolves to the time service at ~1-second granularity
// (sub-second often 0). The engine measures its per-frame delta with it, so a
// coarse clock makes motion freeze then lurch once per second. Back it with the
// fine-grained monotonic clock, seeded once from wall-clock so the absolute
// value stays approximately correct.
static int64_t g_tod_wall_base_us = 0;
static int64_t g_tod_mono_base_ns = 0;

int gettimeofday_fake(struct timeval *tv, void *tz) {
  (void)tz;
  if (!tv)
    return 0;
  struct timespec mono;
  clock_gettime(CLOCK_MONOTONIC, &mono);
  int64_t mono_ns = (int64_t)mono.tv_sec * 1000000000LL + mono.tv_nsec;
  if (g_tod_wall_base_us == 0) {
    struct timespec real;
    clock_gettime(CLOCK_REALTIME, &real);
    g_tod_wall_base_us = (int64_t)real.tv_sec * 1000000LL + real.tv_nsec / 1000;
    g_tod_mono_base_ns = mono_ns;
  }
  int64_t now_us = g_tod_wall_base_us + (mono_ns - g_tod_mono_base_ns) / 1000;
  tv->tv_sec  = (time_t)(now_us / 1000000);
  tv->tv_usec = (long)(now_us % 1000000);
  return 0;
}

// Android/bionic and newlib disagree on CLOCK_* ids: bionic CLOCK_MONOTONIC == 1
// but newlib's 1 is CLOCK_REALTIME (newlib CLOCK_MONOTONIC == 4). The engine and
// BASS were built against bionic and pass bionic ids, so forwarding them raw
// handed them the coarse REALTIME clock when they asked for the fine-grained
// MONOTONIC one -- quantizing the engine's per-frame delta (juddery pacing) and
// scrambling BASS's update-thread timing. Translate ids here.
int clock_gettime_fake(int bionic_clk, struct timespec *ts) {
  clockid_t clk;
  switch (bionic_clk) {
    case 0: clk = CLOCK_REALTIME;  break; // bionic CLOCK_REALTIME
    case 1: clk = CLOCK_MONOTONIC; break; // bionic CLOCK_MONOTONIC
#ifdef CLOCK_PROCESS_CPUTIME_ID
    case 2: clk = CLOCK_PROCESS_CPUTIME_ID; break;
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
    case 3: clk = CLOCK_THREAD_CPUTIME_ID; break;
#endif
    case 4: clk = CLOCK_MONOTONIC; break; // MONOTONIC_RAW -> MONOTONIC
    case 5: clk = CLOCK_REALTIME;  break; // REALTIME_COARSE
    case 6: clk = CLOCK_MONOTONIC; break; // MONOTONIC_COARSE
    case 7: clk = CLOCK_MONOTONIC; break; // BOOTTIME ~= MONOTONIC
    default: clk = CLOCK_MONOTONIC; break;
  }
  return clock_gettime(clk, ts);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return 0;
  const double t0 = mono_ms();
  size_t r = fread(ptr, size, n, f);
  const double dt = mono_ms() - t0;
  if (dt > 40.0)
    debugPrintf("io: SLOW fread %zu bytes took %.1f ms\n", (size_t)(size * n), dt);
  return r;
}

int fputc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return c;
  return fputc(c, f);
}

int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f)) {
    debugPrintf("stdio: %s", s);
    return 0;
  }
  return fputs(s, f);
}

/* ---------------------------------------------------------------------------
 * The engine's libc++ wires std::cout/cerr/clog to &__sF[n] -- which for us is
 * `fake_sF`, a plain byte buffer, NOT a newlib FILE. Any libc function handed
 * one of those would parse our buffer as a real FILE and dereference garbage
 * (the _write function pointer, the fd, the buffer pointers). These were still
 * mapped straight to newlib:
 *
 *   vfprintf setvbuf rewind getc fseeko ftello clearerr fileno
 *
 * fileno was the nastiest: it would return a garbage fd read out of our buffer,
 * and a subsequent close(fd) on that value corrupts newlib's handle table --
 * the very failure this port has been chasing. Route them all through the
 * is_fake_file() check; real FILEs still get the real implementation.
 * ------------------------------------------------------------------------- */

int setvbuf_fake(FILE *f, char *buf, int mode, size_t size) {
  if (is_fake_file(f)) return 0;          /* nothing to buffer */
  return setvbuf(f, buf, mode, size);
}

void rewind_fake(FILE *f) {
  if (is_fake_file(f)) return;
  rewind(f);
}

int fseeko_fake(FILE *f, off_t off, int whence) {
  if (is_fake_file(f)) return -1;
  return fseeko(f, off, whence);
}

off_t ftello_fake(FILE *f) {
  if (is_fake_file(f)) return -1;
  return ftello(f);
}

void clearerr_fake(FILE *f) {
  if (is_fake_file(f)) return;
  clearerr(f);
}


int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL)
    return 0;
  return fflush(f);
}

int fclose_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fclose_L(f);
}

int ferror_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return ferror(f);
}

int fileno_fake(FILE *f) {
  if (is_fake_file(f))
    return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    char buf[0x400];   /* was `static` -- shared across engine worker threads */
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}

int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f))
    return -1;
  return fseek(f, off, whence);
}

int getc_fake(FILE *f) {
  if (is_fake_file(f))
    return -1; // EOF
  return getc(f);
}

int ungetc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return -1;
  return ungetc(c, f);
}

void setbuf_fake(FILE *f, char *buf) {
  if (is_fake_file(f))
    return;
  setbuf(f, buf);
}

// Burger Shop's PopCap engine reads small config/score files through these.
// fopen_fake hands back a real newlib FILE*, so the only special case is the
// trio of fake std streams, which never carry readable content.
long ftell_fake(FILE *f) {
  if (is_fake_file(f))
    return -1;
  return ftell(f);
}

int feof_fake(FILE *f) {
  if (is_fake_file(f))
    return 1; // a fake stream is always "at end" for read loops
  return feof(f);
}

int fgetc_fake(FILE *f) {
  if (is_fake_file(f))
    return -1; // EOF
  return fgetc(f);
}

char *fgets_fake(char *s, int n, FILE *f) {
  if (is_fake_file(f))
    return NULL;
  return fgets(s, n, f);
}

int fscanf_fake(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int ret;
  if (is_fake_file(f))
    ret = -1; // EOF: nothing to parse from a std stream
  else
    ret = vfscanf(f, fmt, va);
  va_end(va);
  return ret;
}

// ---------------------------------------------------------------------------
// AAsset emulation: serve "APK assets" from the encrypted OBB, falling back
// to loose files in the game directory.
// ---------------------------------------------------------------------------

#define ASSET_MAGIC 0xA55E7A11u   /* "ASSETALL" -- validates AAsset* handles */

typedef struct {
  uint32_t magic;  // ASSET_MAGIC while live; anything else => wild/stale pointer
  uint8_t *mem;    // OBB-decoded buffer (NULL for a loose-file asset)
  size_t size;
  size_t pos;
  FILE *f;         // loose-file backing (NULL for a memory asset)
} Asset;

/* Validate an AAsset* handed back by the engine. Returns NULL if the pointer is
 * not one of ours (wild/garbage), so every AAsset_* entry point can fail safe
 * instead of dereferencing junk. */
static Asset *asset_ok(void *p) {
  Asset *a = p;
  return (a && a->magic == ASSET_MAGIC) ? a : NULL;
}

void *AAssetManager_fromJava_fake(void *env, void *mgr) {
  (void)env; (void)mgr;
  return (void *)1; // any non-NULL token
}

// --- asset path resolution -------------------------------------------------
// The engine asks for paths relative to the Android assets/ root, e.g.
// "data/shaders/gles2/x.fx". We map those onto the extracted files under the
// game directory, trying each ASSET_ROOTS prefix ("assets" first, since that's
// where an unpacked APK puts them). Using an absolute base makes this immune to
// the process working directory.

/* MUST be an absolute device path. Every path we hand newlib is anchored to
 * this, and a colon-less path makes newlib's FindDevice() fall back to an unset
 * default device -> devoptab_list[dev] == NULL -> Data Abort in _open_r. */
static char g_asset_base[512] = DATA_DIR;   /* "sdmc:/switch/btd5" */

void set_asset_base(const char *dir) {
  if (!dir || !*dir) return;
  strncpy(g_asset_base, dir, sizeof(g_asset_base) - 1);
  g_asset_base[sizeof(g_asset_base) - 1] = 0;
  size_t n = strlen(g_asset_base);
  while (n > 1 && g_asset_base[n - 1] == '/') g_asset_base[--n] = 0; // drop trailing '/'
  debugPrintf("asset base = %s\n", g_asset_base);
}

static int file_exists(const char *p) {
  /* MUST guard: this is called in a loop from resolve_asset_path() on paths
   * built from the asset roots. A path that resolves to a bare device root
   * ("sdmc:", "sdmc:/switch") makes newlib's _stat_r null-deref its devoptab
   * (Data Abort at devoptab->stat, +0x40) instead of returning an error. */
  if (path_would_crash_newlib(p)) return 0;
  struct stat st;
  return stat_L(p, &st) == 0 && S_ISREG(st.st_mode);
}

int resolve_asset_path(const char *rel, char *out, size_t out_size) {
  if (!rel || !out || out_size == 0) return 0;
  while (rel[0] == '.' && rel[1] == '/') rel += 2;          // strip leading "./"

  if (rel[0] == '/' || strchr(rel, ':')) {                  // already a host path
    if (file_exists(rel)) { snprintf(out, out_size, "%s", rel); return 1; }
    return 0;
  }

  static const char *roots[] = ASSET_ROOTS;
  const int nroots = (int)(sizeof(roots) / sizeof(roots[0]));
  for (int i = 0; i < nroots; i++) {
    if (strcmp(roots[i], ".") == 0)
      snprintf(out, out_size, "%s/%s", g_asset_base, rel);
    else
      snprintf(out, out_size, "%s/%s/%s", g_asset_base, roots[i], rel);
    if (file_exists(out)) return 1;
  }
  return 0;
}

// Redirect an absolute, root-relative data path (e.g. "/fusion.registry") into
// the game directory. Leaves alone: non-absolute paths, anything already
// carrying a device ("sdmc:/..."), and real pseudo-paths (/proc, /dev, /sys)
// which must keep their original meaning.
static const char *remap_data_path(const char *path, char *buf, size_t bufsz) {
  if (!path) return path;

  /* Pseudo-filesystems the engine probes: leave alone (handled/failed above). */
  if (!strncmp(path, "/proc", 5) || !strncmp(path, "/dev", 4) ||
      !strncmp(path, "/sys", 4))
    return path;

  /* Already has a device prefix ("sdmc:/...", "romfs:/..."): use as-is. */
  if (strchr(path, ':'))
    return path;

  /* CRITICAL: never hand newlib a path with NO device prefix.
   * newlib's FindDevice() falls back to the DEFAULT device for a colon-less
   * path; in this environment that slot is unset, so devoptab_list[dev] is NULL
   * and _open_r faults dereferencing ->open_r (+0x10) instead of returning
   * ENOENT. The engine opens plenty of bare relative names ("events.data",
   * "newslocal_btd6", "com.ninjakiwi.link/cache.files"). Anchor every one of
   * them under the game directory so the path always carries "sdmc:". */
  if (path[0] == '/')
    snprintf(buf, bufsz, "%s%s", g_asset_base, path);        /* "/foo" -> base + "/foo" */
  else
    snprintf(buf, bufsz, "%s/%s", g_asset_base, path);       /* "foo"  -> base + "/foo" */
  return buf;
}

int rename_fake(const char *oldp, const char *newp) {
  char a[600], b[600];
  const char *rold = remap_data_path(oldp, a, sizeof a);
  const char *rnew = remap_data_path(newp, b, sizeof b);
  if (path_would_crash_newlib(rold) || path_would_crash_newlib(rnew)) return -1;
  FILE_LOCK();                       /* device-layer op: same lock as open/close */
  int rc = rename(rold, rnew);
  if (rc != 0) {
    // Switch fsdev (FAT32/exFAT) rename fails if the destination already
    // exists, unlike POSIX which atomically replaces it. The game's saves rely
    // on write-tmp-then-rename-over-the-old-file, so without this every save
    // after the first silently fails. Remove the old file and retry.
    remove(rnew);
    rc = rename(rold, rnew);
  }
  FILE_UNLOCK();
  debugPrintf("rename(%s -> %s) -> %d\n", rold, rnew, rc);
  return rc;
}
int remove_fake(const char *path) {
  char a[600];
  const char *p = remap_data_path(path, a, sizeof a);
  if (path_would_crash_newlib(p)) return -1;
  return remove_L(p);
}
int unlink_fake(const char *path) {
  char a[600];
  const char *p = remap_data_path(path, a, sizeof a);
  if (path_would_crash_newlib(p)) return -1;
  return unlink_L(p);
}
int access_fake(const char *path, int mode) {
  char a[600];
  const char *p = remap_data_path(path, a, sizeof a);
  if (path_would_crash_newlib(p)) return -1;
  return access_L(p, mode);
}

// fopen with a large stream buffer for the big game archives: the engine's
// LZW streams issue many small reads/seeks, and fsdev round trips dominate
/* newlib's _open_r/_mkdir_r/_stat_r etc. DEREFERENCE A NULL DEVOPTAB (crash,
 * Data Abort at devoptab->fn) when handed a path that resolves to a bare device
 * root or a single top-level component -- e.g. "sdmc:", "sdmc:/", "sdmc:/switch"
 * (a "device:" prefix whose in-device path is empty or has no '/'). Instead of
 * returning an error like a sane libc, it faults. The engine hits exactly such
 * a path during nativeLoad. Reject these BEFORE calling newlib. Returns 1 if the
 * path is unsafe to hand to newlib. (Discovered via the colorsheep/cr3 ports,
 * which guard the same newlib bug.) */
static int path_would_crash_newlib(const char *path) {
  if (!path || !*path) return 1;
  const char *colon = strchr(path, ':');
  if (colon) {                       // has a "device:" prefix
    const char *in = colon + 1;      // path inside the device
    while (*in == '/') in++;
    if (!*in) return 1;              // "sdmc:" / "sdmc:/"  -> device root
    if (!strchr(in, '/')) return 1;  // "sdmc:/switch"     -> single top-level component
    return 0;
  }
  /* NO device prefix at all. remap_data_path() deliberately passes /proc, /dev
   * and /sys through unremapped (they're Linux-only pseudo-filesystems the
   * engine probes). newlib then resolves them against the DEFAULT device, whose
   * devoptab slot is NULL in this process -> _open_r/_stat_r do
   *     ldr x1,[devoptab_list + idx*8] ; ldr x1,[x1,#0x10]  -> Data Abort at 0x0
   * i.e. it crashes instead of returning ENOENT. The engine's fopen("/proc/...")
   * during nativeLoad is exactly this. Refuse device-less absolute paths so the
   * caller gets a clean NULL/-1 (the engine already handles a failed open here). */
  if (path[0] == '/') return 1;
  return 0;                          // relative path: newlib resolves via cwd, safe
}

FILE *fopen_fake(const char *path, const char *mode) {
  // The Switch has no /proc/meminfo. The engine reads MemTotal from it to size
  // its asset tier; when the open fails it ends up seeing 0MB and drops to
  // LowRes graphics. Serve a synthetic meminfo with a healthy MemTotal so its
  // normal detection runs and it loads the full-resolution asset set.
  if (path && strcmp(path, "/proc/meminfo") == 0) {
    static const char meminfo[] =
        "MemTotal:       4194304 kB\n"
        "MemFree:        2097152 kB\n"
        "MemAvailable:   3145728 kB\n";
    FILE *m = fmemopen((void *)meminfo, sizeof(meminfo) - 1, "r");
    if (m) return m;
  }

  // redirect root-relative save/data files into the game directory
  char rbuf[600];
  path = remap_data_path(path, rbuf, sizeof rbuf);

  // reject paths that would null-deref newlib's devoptab (see helper above)
  if (path_would_crash_newlib(path)) {
    debugPrintf("fopen(%s, %s) -> refused (unsafe device-root path)\n", path ? path : "(null)", mode);
    return NULL;
  }

  FILE *f = fopen_L(path, mode);
  // If a relative READ misses, retry via the asset roots -- some engine code
  // opens assets with fopen directly rather than through AAssetManager.
  if (!f && path && mode && strchr(mode, 'r') &&
      path[0] != '/' && !strchr(path, ':')) {
    char resolved[600];
    if (resolve_asset_path(path, resolved, sizeof(resolved)))
      f = fopen_L(resolved, mode);
  }
  if (!f) debugPrintf("fopen(%s, %s) -> FAIL\n", path, mode); // skip the per-frame save.bin spam
  // trace save writes: the game persists settings/progress by writing these
  if (path && mode && (strchr(mode, 'w') || strchr(mode, 'a')))
    debugPrintf("fopen(%s, %s) [WRITE] -> %s\n", path, mode, f ? "ok" : "FAIL");
  // trace save reads (.dat/.bin) so we can see the load path on relaunch
  if (path && mode && strchr(mode, 'r')) {
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".dat") == 0 || strcasecmp(ext, ".bin") == 0))
      debugPrintf("fopen(%s, %s) [READ] -> %s\n", path, mode, f ? "ok" : "FAIL");
  }
  if (f && strchr(mode, 'r')) {
    const char *ext = strrchr(path, '.');
    if (ext && (strcasecmp(ext, ".ras") == 0 || strcasecmp(ext, ".msf") == 0))
      setvbuf(f, NULL, _IOFBF, 128 * 1024);
  }
  return f;
}

void *AAssetManager_open_fake(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  Asset *a = calloc(1, sizeof(*a));
  if (!a)
    return NULL;
  a->magic = ASSET_MAGIC;

  size_t size = 0;
  void *mem = obb_read(path, &size);
  if (mem) {
    a->mem = mem;
    a->size = size;
    debugPrintf("AAsset: open(%s) -> mem, %zu bytes\n", path, size);
    return a;
  }

  // fall back to a loose file: map the Android-relative asset path onto the
  // extracted files under the game directory (tries assets/, ., data/).
  char resolved[600];
  const char *fp = resolve_asset_path(path, resolved, sizeof(resolved)) ? resolved : path;
  // resolve_asset_path may fail and leave fp == path (the raw engine string);
  // guard before newlib fopen so a device-root/malformed path can't null-deref
  // the devoptab (the crash reached via libnative -> AAssetManager_open).
  FILE *f = path_would_crash_newlib(fp) ? NULL : fopen_L(fp, "rb");
  if (!f) {
    free(a);
    debugPrintf("AAsset: open(%s) MISSING (tried %s)\n", path, fp);
    return NULL;
  }
  /* Read the whole asset into memory, then CLOSE the FILE immediately.
   *
   * Why: the engine keeps many assets open at once (78+ during nativeLoad) and
   * seeks/reads them later. Holding that many newlib FILEs exhausts its small fd
   * table; once exhausted, a FILE can come back with an invalid fd, and the next
   * fseek faults resolving devoptab_list[fd->device] == NULL (Data Abort at
   * ->seek_r, +0x30). Every crash we chased through AAsset_seek was this.
   *
   * Serving assets from memory removes newlib FILE from the asset path entirely:
   * no fds are held, seek/read become pure pointer math on the existing a->mem
   * path, and there is nothing left for newlib to corrupt or run out of. The
   * assets are small (largest ~1.8 MB) so this is a good trade. */
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len < 0) len = 0;

  uint8_t *buf = malloc((size_t)len ? (size_t)len : 1);
  if (!buf) {
    fclose_L(f);
    free(a);                        /* never handed to the engine -> safe to free */
    debugPrintf("AAsset: open(%s) OOM (%ld bytes)\n", path, len);
    return NULL;
  }
  const size_t got = fread(buf, 1, (size_t)len, f);
  fclose_L(f);                       /* fd released right away */

  if (got != (size_t)len)
    debugPrintf("AAsset: open(%s) short read %zu/%ld\n", path, got, len);

  a->mem  = buf;                     /* memory-backed from here on */
  a->size = got;
  a->pos  = 0;
  a->f    = NULL;                    /* no FILE held: seek/read never touch newlib */

  debugPrintf("AAsset: open(%s) -> mem, %ld bytes\n", path, (long)a->size);
  return a;
}

void AAsset_close_fake(void *asset) {
  Asset *a = asset_ok(asset);
  if (!a)
    return;

  /* USE-AFTER-FREE GUARD.
   * The engine calls AAsset_seek/AAsset_read on assets it has already closed.
   * On Android that is (accidentally) survivable; here, free()ing the struct
   * meant the next seek read a dangling a->f -- a non-NULL garbage FILE* --
   * and newlib faulted resolving its fd's device (devoptab_list[dev] == NULL,
   * Data Abort at ->seek_r +0x30). That is the crash we kept hitting.
   *
   * So release the *resources* but deliberately KEEP the Asset struct alive and
   * mark it closed. A later seek/read then sees mem == NULL and f == NULL and
   * returns -1 harmlessly instead of faulting. The struct is ~32 bytes; leaking
   * one per asset is a trivial cost for turning a hard crash into a no-op. */
  if (a->f) { fclose_L(a->f); a->f = NULL; }
  free(a->mem);
  a->mem  = NULL;
  a->size = 0;
  a->pos  = 0;
  /* intentionally NOT free(a) */
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  Asset *a = asset_ok(asset);
  if (!a)
    return -1;
  if (a->mem) {
    if (a->pos >= a->size) return 0;          /* EOF; also stops the underflow below */
    size_t avail = a->size - a->pos;          /* safe: pos < size */
    if (count > avail)
      count = avail;
    memcpy(buf, a->mem + a->pos, count);
    a->pos += count;
    return (int)count;
  }
  if (!a->f) return -1;          /* no memory buffer and no open FILE */
  const double t0 = mono_ms();
  int r = (int)fread(buf, 1, count, a->f);
  const double dt = mono_ms() - t0;
  if (dt > 40.0)
    debugPrintf("io: SLOW AAsset_read %zu bytes took %.1f ms\n", count, dt);
  return r;
}

long AAsset_seek_fake(void *asset, long off, int whence) {
  Asset *a = asset_ok(asset);
  if (!a)
    return -1;
  if (a->mem) {
    long base = (whence == SEEK_CUR) ? (long)a->pos : (whence == SEEK_END) ? (long)a->size : 0;
    long np = base + off;
    if (np < 0 || (size_t)np > a->size)
      return -1;
    a->pos = (size_t)np;
    return (long)a->pos;
  }
  if (!a->f) return -1;          /* no memory buffer and no open FILE */
  if (fseek(a->f, off, whence) < 0)
    return -1;
  return ftell(a->f);
}

int64_t AAsset_seek64_fake(void *asset, int64_t off, int whence) {
  return AAsset_seek_fake(asset, (long)off, whence);
}

long AAsset_getLength_fake(void *asset) {
  Asset *a = asset_ok(asset);
  return a ? (long)a->size : 0;
}

int64_t AAsset_getLength64_fake(void *asset) {
  Asset *a = asset_ok(asset);
  return a ? (int64_t)a->size : 0;
}

// AAsset_getBuffer: Android maps the whole asset and returns a pointer to it.
// For a memory-backed asset we already have the buffer; for a loose file we
// slurp the whole thing once and cache it on the Asset (freed in AAsset_close).
// The Fusion engine uses this to load textures/data in one shot.
const void *AAsset_getBuffer_fake(void *asset) {
  Asset *a = asset_ok(asset);
  if (!a) return NULL;
  if (a->mem) return a->mem;
  if (!a->f) return NULL;
  uint8_t *buf = malloc(a->size ? a->size : 1);
  if (!buf) return NULL;
  long cur = ftell(a->f);
  fseek(a->f, 0, SEEK_SET);
  size_t rd = fread(buf, 1, a->size, a->f);
  fseek(a->f, cur < 0 ? 0 : cur, SEEK_SET);
  if (rd != a->size) debugPrintf("AAsset_getBuffer: short read %zu/%zu\n", rd, a->size);
  a->mem = buf;          // cache; AAsset_close frees a->mem
  /* The asset is now memory-backed, so subsequent read/seek take the a->mem
   * branch, which is driven by a->pos. Sync a->pos to where the FILE actually
   * is, otherwise those calls would silently restart from offset 0 and hand the
   * engine the wrong bytes. */
  a->pos = (size_t)(cur < 0 ? 0 : cur);
  return a->mem;
}

long AAsset_getRemainingLength_fake(void *asset) {
  Asset *a = asset;
  if (!a)
    return 0;
  size_t pos = a->mem ? a->pos : (size_t)ftell(a->f);
  return (long)(a->size - pos);
}

int64_t AAsset_getRemainingLength64_fake(void *asset) {
  return AAsset_getRemainingLength_fake(asset);
}

// ---------------------------------------------------------------------------
// ANativeWindow -> NWindow mapping
// ---------------------------------------------------------------------------

void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env; (void)surface;
  NWindow *win = nwindowGetDefault();
  /* Match the working colorsheep/cr3 ports: set dimensions AND an explicit crop
   * plus identity transform. nwindowSetDimensions may allocate a width-aligned
   * swapchain buffer; without a matching crop the compositor can scan extra
   * uninitialized columns. Leaving the transform unset likewise lets a stale
   * value drive the compositor. Both are cheap property writes. */
  nwindowSetDimensions(win, screen_width, screen_height);
  nwindowSetCrop(win, 0, 0, screen_width, screen_height);
  nwindowSetTransform(win, 0u);   /* identity: landscape buffer -> landscape panel */
  debugPrintf("ANativeWindow_fromSurface -> %p (%dx%d, crop+identity)\n",
              win, screen_width, screen_height);
  return win;
}

int ANativeWindow_getWidth_fake(void *win) {
  (void)win;
  return screen_width;
}

int ANativeWindow_getHeight_fake(void *win) {
  (void)win;
  return screen_height;
}

void ANativeWindow_release_fake(void *win) {
  (void)win;
}

int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)format;
  debugPrintf("ANativeWindow_setBuffersGeometry(%d, %d)\n", w, h);
  if (w > 0 && h > 0)
    nwindowSetDimensions((NWindow *)win, w, h);
  return 0;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks and semaphores via pointer indirection
// (bionic types are plain structs the game allocates; we stash a pointer
// to the real object in their first bytes, like the mutex fakes)
// ---------------------------------------------------------------------------

typedef struct {
  RwLock lock;
} FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) {
    FakeRwLock *l = calloc(1, sizeof(*l));
    rwlockInit(&l->lock);
    *storage = l;
  }
  return *storage;
}

int pthread_rwlock_rdlock_fake(void **rw) {
  rwlockReadLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_wrlock_fake(void **rw) {
  rwlockWriteLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  // libnx needs to know which way it was locked
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock))
    rwlockWriteUnlock(&l->lock);
  else
    rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct {
  Semaphore sem;
} FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}

int sem_destroy_fake(void **s) {
  if (s && *s) {
    free(*s);
    *s = NULL;
  }
  return 0;
}

int sem_post_fake(void **s) {
  if (s && *s)
    semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_wait_fake(void **s) {
  if (s && *s)
    semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem))
    return 0;
  errno = EAGAIN;
  return -1;
}

int sem_getvalue_fake(void **s, int *val) {
  if (s && *s)
    *val = (int)((FakeSem *)*s)->sem.count;
  else
    *val = 0;
  return 0;
}

int pthread_attr_getstacksize_fake(const void *attr, size_t *size) {
  (void)attr;
  *size = 512 * 1024;
  return 0;
}

int pthread_attr_getschedparam_fake(const void *attr, void *param) {
  (void)attr;
  memset(param, 0, 8);
  return 0;
}
