/* nx_net_log.c -- btd5_net.log in the game folder
 *
 * Built only when NET_LOG is 1 (config.h); release builds have it off, and then
 * no file is created at all. It records bring-up state, every host looked up,
 * the first connection, keyboard activity, and every failure (bounded), and is
 * rewritten each launch.
 *
 * Written through the fs service directly (fsFileWrite), NOT newlib, for the
 * reason util.c gives: newlib's fd table races with the engine's asset
 * threads. Lines are also mirrored to debugPrintf for DEBUG_LOG builds.
 *
 * MIT license -- see LICENSE.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "nx_net.h"
#include "nx_paths.h"
#include "util.h"

#define NET_LOG_FILE "btd5_net.log"                 /* in the game folder */
#define NET_LOG_MAX  (512 * 1024)

#if NET_LOG
static Mutex  s_mx;
static FsFile s_file;
static int    s_state;           /* 0 unopened, 1 open, -1 failed/full */
static s64    s_off;
static unsigned s_lines;

static void open_locked(void) {
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  char path[600];
  if (!fs || !nx_data_fs_path(NET_LOG_FILE, path, sizeof path)) { s_state = -1; return; }
  nx_data_dir_ensure();                             /* may run before main() */
  fsFsCreateFile(fs, path, 0, 0);                   /* no-op if it exists */
  if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &s_file))) {
    s_state = -1;
    return;
  }
  fsFileSetSize(&s_file, 0);                        /* fresh each launch */
  s_off = 0;
  s_state = 1;
}
#endif /* NET_LOG */

int nx_net_logf(const char *fmt, ...) {
#if !NET_LOG && !DEBUG_LOG
  (void)fmt;                     /* release build: no log at all, no file created */
  return 0;
#else
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n <= 0) return 0;
  if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;

  debugPrintf("%s", buf);                           /* main log, DEBUG_LOG builds */
#if !NET_LOG
  return n;                                         /* debug log only */
#else
  mutexLock(&s_mx);
  if (s_state == 0) open_locked();
  if (s_state == 1) {
    if (s_off + n > NET_LOG_MAX) {
      static const char full[] = "[net] log size cap reached\n";
      const s64 fl = (s64)sizeof full - 1;
      if (R_SUCCEEDED(fsFileSetSize(&s_file, s_off + fl)))
        fsFileWrite(&s_file, s_off, full, (u64)fl, FsWriteOption_Flush);
      fsFileClose(&s_file);
      s_state = -1;
    } else if (R_SUCCEEDED(fsFileSetSize(&s_file, s_off + n))) {
      /* Grow EXACTLY to what is written. Pre-extending in chunks left the
       * slack filled with whatever was on the SD card before (FAT does not
       * zero new clusters), and a game closed from the HOME menu never gets
       * to trim it -- that is how a log ended in 30 KB of binary junk. */
      const u32 opt = (s_lines < 64 || (s_lines & 31) == 0) ? FsWriteOption_Flush : FsWriteOption_None;
      if (R_SUCCEEDED(fsFileWrite(&s_file, s_off, buf, (u64)n, opt))) s_off += n;
      else fsFileSetSize(&s_file, s_off);            /* keep the file exactly its content */
      s_lines++;
    }
  }
  mutexUnlock(&s_mx);
  return n;
#endif /* NET_LOG */
#endif /* NET_LOG || DEBUG_LOG */
}

void nx_net_log_close(void) {
#if NET_LOG
  mutexLock(&s_mx);
  if (s_state == 1) {
    fsFileFlush(&s_file);
    fsFileClose(&s_file);
    s_state = -1;
  }
  mutexUnlock(&s_mx);
#endif
}
