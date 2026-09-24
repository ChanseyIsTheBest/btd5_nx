/* nx_paths.c -- see nx_paths.h.
 *
 * No __thread and no engine dependencies: this runs from userAppInit, before
 * main() and before any engine code.
 *
 * MIT license -- see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config.h"
#include "nx_paths.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

extern int    __system_argc;             /* libnx argv.c, set before __appInit */
extern char **__system_argv;

static char        s_dir[512];
static const char *s_src = "";
static volatile int s_done;

int nx_path_normalize(const char *in, char *out, size_t cap) {
  if (!in || !out || cap < 2) return -1;
  size_t o = 0;
  const char *rest = in;
  const char *colon = strchr(in, ':');
  const char *slash = strchr(in, '/');
  if (colon && (!slash || colon < slash)) {       /* keep the "device:" prefix */
    const size_t d = (size_t)(colon - in) + 1;
    if (d + 1 >= cap) return -1;
    memcpy(out, in, d);
    o = d;
    rest = colon + 1;
  }
  const int absolute = (*rest == '/') || o > 0;
  size_t base = o;                                /* segments start after here */
  if (absolute) { out[o++] = '/'; base = o; }
  out[o] = 0;

  const char *p = rest;
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *e = p;
    while (*e && *e != '/') e++;
    const size_t len = (size_t)(e - p);
    if (len == 1 && p[0] == '.') {
      /* stay */
    } else if (len == 2 && p[0] == '.' && p[1] == '.') {
      if (o > base) {                            /* pop one segment */
        size_t k = o;
        while (k > base && out[k - 1] != '/') k--;
        o = (k > base) ? k - 1 : base;           /* drop the separator too */
        out[o] = 0;
      }                                          /* at the root: ".." is a no-op */
    } else {
      const int sep = (o > base) ? 1 : 0;
      if (o + sep + len + 1 > cap) return -1;
      if (sep) out[o++] = '/';
      memcpy(out + o, p, len);
      o += len;
      out[o] = 0;
    }
    p = e;
  }
  return 0;
}

static int has_component(const char *d) {        /* "dev:/x..." -- not a device root */
  const char *c = strchr(d, ':');
  if (!c || c == d || c[1] != '/') return 0;
  const char *p = c + 1;
  while (*p == '/') p++;
  return *p != 0;
}

void nx_paths_init(void) {
  if (s_done) return;
  char raw[512], norm[512];

  /* 1. The launch path: exactly what libnx's __libnx_init_cwd uses. */
  if (__system_argc > 0 && __system_argv && __system_argv[0] && __system_argv[0][0]) {
    snprintf(raw, sizeof raw, "%s%s", (__system_argv[0][0] == '/') ? "sdmc:" : "", __system_argv[0]);
    char *last = strrchr(raw, '/');
    if (last) {
      *last = 0;
      if (nx_path_normalize(raw, norm, sizeof norm) == 0 && has_component(norm)) {
        snprintf(s_dir, sizeof s_dir, "%s", norm);
        s_src = "launch path";
      }
    }
  }
  /* 2. The working directory (libnx sets it to the same folder when it can). */
  if (!s_dir[0] && getcwd(raw, sizeof raw) &&
      nx_path_normalize(raw, norm, sizeof norm) == 0 && has_component(norm)) {
    snprintf(s_dir, sizeof s_dir, "%s", norm);
    s_src = "working directory";
  }
  /* 3. Last resort. */
  if (!s_dir[0]) {
    const int had_argv = __system_argc > 0 && __system_argv && __system_argv[0] && __system_argv[0][0];
    snprintf(s_dir, sizeof s_dir, "%s", DATA_DIR_FALLBACK);
    s_src = had_argv ? "built-in default (the launch folder is the SD card root, which cannot hold files safely)"
                     : "built-in default (launch path unknown)";
  }
  size_t n = strlen(s_dir);
  while (n > 1 && s_dir[n - 1] == '/') s_dir[--n] = 0;
  s_done = 1;
}

const char *nx_data_dir(void)        { nx_paths_init(); return s_dir; }
const char *nx_data_dir_source(void) { nx_paths_init(); return s_src; }

char *nx_data_file(const char *name, char *buf, size_t cap) {
  if (!buf || !cap) return NULL;
  const int n = snprintf(buf, cap, "%s/%s", nx_data_dir(), name ? name : "");
  return (n < 0 || (size_t)n >= cap) ? NULL : buf;
}

char *nx_data_fs_path(const char *name, char *buf, size_t cap) {
  const char *d = nx_data_dir();
  if (strncasecmp(d, "sdmc:", 5) != 0 || !buf || !cap) return NULL;
  const int n = snprintf(buf, cap, "%s/%s", d + 5, name ? name : "");
  return (n < 0 || (size_t)n >= cap) ? NULL : buf;
}

int nx_path_inside_data(const char *normalized) {
  const char *d = nx_data_dir();
  const size_t n = strlen(d);
  if (!normalized || strncasecmp(normalized, d, n) != 0) return 0;
  return normalized[n] == 0 || normalized[n] == '/';
}

void nx_data_dir_ensure(void) {
#if defined(__SWITCH__)
  char fsp[512];
  if (!nx_data_fs_path("", fsp, sizeof fsp)) return;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (!fs) return;
  size_t n = strlen(fsp);
  if (n > 1 && fsp[n - 1] == '/') fsp[--n] = 0;
  for (size_t i = 1; i <= n; i++) {               /* "/switch", "/switch/btd5", ... */
    if (fsp[i] == '/' || fsp[i] == 0) {
      const char c = fsp[i];
      fsp[i] = 0;
      fsFsCreateDirectory(fs, fsp);                /* "already exists" is fine */
      fsp[i] = c;
    }
  }
#endif
}
