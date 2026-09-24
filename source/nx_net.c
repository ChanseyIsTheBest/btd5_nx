/* nx_net.c -- network bring-up for the Bloons TD 5 port. See nx_net.h.
 *
 * config.txt (in the game folder) has one setting:
 *
 *   online = on | off        off keeps the game off the network: DNS fails and
 *                            nothing but loopback can be reached. Sockets can
 *                            still be CREATED (Asio throws if they cannot).
 *
 * Everything else is fixed at the values the port was tested with: real
 * non-blocking connects, 10 bsd sessions (each concurrently BLOCKED socket
 * call holds one), no socket trace, a generated per-console device id, and
 * the player name "Player". A config.txt from an older build (with its extra
 * keys) is rewritten to the one-line form, keeping its online choice.
 *
 * DEVICE ID. MainActivity.getUniqueID feeds Ninja Kiwi's UDID / device_id
 * (NKUniqueDeviceId, nk_udid.cpp, the NK API's device_id field). It used to be
 * the constant "switch-btd5-local" for every Switch, so every console presented
 * the same identity to NK's services. It is now 16 random hex digits (the shape
 * of an Android ANDROID_ID), generated once and kept in device_id.txt next to
 * the save. Delete that file to get a new one.
 *
 * MIT license -- see LICENSE.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "config.h"
#include "nx_net.h"
#include "nx_paths.h"
#include "nx_config.h"
#include "util.h"

#define DEVICE_ID_FILE "device_id.txt"         /* in the game folder */

static int  s_inited;
static int  s_sock_ok, s_nifm_ok, s_sessions;
static int  s_enabled = 1;
static const int s_trace = 0, s_want_sessions = 10, s_connect_timeout = 0;  /* fixed; see top */
static char s_device_id[64];
static const char s_player_name[] = "Player";
static uint32_t s_bcast;

int nx_net_enabled(void)            { return s_enabled; }
int nx_net_sockets_ok(void)         { return s_sock_ok; }
int nx_net_trace(void)              { return s_trace; }
int nx_net_connect_timeout_ms(void) { return s_connect_timeout; }
uint32_t nx_net_subnet_broadcast(void) { return s_bcast; }
const char *nx_net_player_name(void) { return s_player_name; }

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

/* ------------------------------------------------------------------ */
/* device id                                                           */
/* ------------------------------------------------------------------ */
static int id_ok(const char *s) {
  size_t n = strlen(s);
  if (n < 8 || n > 48) return 0;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') return 0;
  return 1;
}

static void load_device_id(void) {
  char idpath[600];
  if (!nx_data_file(DEVICE_ID_FILE, idpath, sizeof idpath)) idpath[0] = 0;
  FILE *f = idpath[0] ? fopen(idpath, "r") : NULL;
  if (f) {
    char buf[64] = {0};
    if (fgets(buf, sizeof buf, f)) {
      char *t = trim(buf);
      if (id_ok(t)) snprintf(s_device_id, sizeof s_device_id, "%s", t);
    }
    fclose(f);
  }
  if (s_device_id[0]) {
    nx_net_logf("[net] device id: %s (device_id.txt)\n", s_device_id);
    return;
  }
  uint8_t r[8];
  randomGet(r, sizeof r);
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++) { s_device_id[2 * i] = hex[r[i] >> 4]; s_device_id[2 * i + 1] = hex[r[i] & 15]; }
  s_device_id[16] = 0;
  f = idpath[0] ? fopen(idpath, "w") : NULL;
  if (f) {
    fprintf(f, "%s\n", s_device_id);
    fclose(f);
  }
  nx_net_logf("[net] device id: %s (new, saved to %s%s)\n", s_device_id, idpath, f ? "" : " -- SAVE FAILED");
}

const char *nx_net_device_id(void) {
  if (!s_device_id[0]) load_device_id();     /* JNI can ask before nx_net_init */
  return s_device_id;
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */
static const char *fmt_ip(uint32_t be, char *b, size_t n) {
  const uint8_t *o = (const uint8_t *)&be;       /* nifm hands these over in network order */
  snprintf(b, n, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
  return b;
}

static void refresh_broadcast(void) {
  u32 a = 0, m = 0, g = 0, d1 = 0, d2 = 0;
  if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&a, &m, &g, &d1, &d2)) && a && m)
    s_bcast = (a & m) | ~m;                      /* network order in, network order out */
}

/* Online = the console has an IP address.
 *
 * NOT nifm's "Connected" alone: that state also requires Nintendo's
 * connection test to pass, and plenty of homebrew setups block Nintendo's
 * servers (DNS blocklists, 90DNS variants, firewalls) while the internet
 * works fine. Gating on it would refuse every lookup on exactly those
 * consoles. An address means a link; whether Ninja Kiwi is reachable is for
 * the request to find out. With no address at all, lookups fail at once. */
int nx_net_online(void) {
  if (!s_enabled || !s_sock_ok) return 0;
  if (!s_nifm_ok) return 1;                      /* cannot ask: let the request decide */
  static u64 s_last;
  static int s_state = -1;
  const u64 now = armGetSystemTick();
  if (s_state >= 0 && armTicksToNs(now - s_last) < 1000000000ull) return s_state;
  NifmInternetConnectionType type;
  NifmInternetConnectionStatus st;
  u32 strength = 0, ip = 0;
  const int internet = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &st)) &&
                       st == NifmInternetConnectionStatus_Connected;
  const int have_ip = R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0;
  const int state = internet || have_ip;
  if (state != s_state) {
    if (state) refresh_broadcast();              /* network may have changed (dock/Wi-Fi) */
    if (s_state >= 0 || !state || !internet)
      nx_net_logf("[net] network %s%s\n", state ? "UP" : "DOWN (no IP address)",
                  state && !internet ? " -- nifm reports Nintendo's connection test NOT passed "
                                       "(blocked Nintendo servers?); trying anyway" : "");
  }
  s_state = state;
  s_last = now;
  return state;
}

void nx_net_log_status(const char *why) {
  char ip[20] = "?", mask[20] = "?", gw[20] = "?";
  if (s_nifm_ok) {
    u32 a = 0, m = 0, g = 0, d1 = 0, d2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&a, &m, &g, &d1, &d2)) && a) {
      fmt_ip(a, ip, sizeof ip); fmt_ip(m, mask, sizeof mask); fmt_ip(g, gw, sizeof gw);
    }
  }
  nx_net_logf("[net] %s: %s, bsd %s (%d sessions), nifm %s, network %s, ip %s/%s gw %s, connect %s\n",
              why ? why : "status", s_enabled ? "online=1" : "online=0 (off the network)",
              s_sock_ok ? "up" : "DOWN", s_sessions, s_nifm_ok ? "up" : "unavailable",
              nx_net_online() ? "up" : "DOWN", ip, mask, gw,
              s_connect_timeout > 0 ? "bounded" : "async");
}

/* ------------------------------------------------------------------ */
/* bring-up                                                            */
/* ------------------------------------------------------------------ */
void nx_net_init(void) {
  if (s_inited) return;
  s_inited = 1;
  nx_data_dir_ensure();       /* DEBUG_LOG builds get here from userAppInit, before main() */
  s_enabled = nx_config_online();   /* config.txt: online = on | off */

  SocketInitConfig cfg = *socketGetDefaultInitConfig();
  cfg.num_bsd_sessions = (u32)s_want_sessions;
  Result rc = socketInitialize(&cfg);
  if (R_SUCCEEDED(rc)) {
    s_sessions = s_want_sessions;
  } else {
    /* a tuned config can be refused when memory is tight; the default one
     * still works, with less headroom for concurrent blocking calls */
    rc = socketInitializeDefault();
    s_sessions = (int)socketGetDefaultInitConfig()->num_bsd_sessions;
  }
  s_sock_ok = R_SUCCEEDED(rc);

  const Result nr = nifmInitialize(NifmServiceType_User);
  s_nifm_ok = R_SUCCEEDED(nr);
  if (s_nifm_ok) refresh_broadcast();
  if (!s_sock_ok) nx_net_logf("[net] socketInitialize FAILED 0x%x -- no networking this run\n", rc);
  if (!s_nifm_ok) nx_net_logf("[net] nifmInitialize failed 0x%x -- connection state unknown\n", nr);

  (void)nx_net_device_id();
  nx_net_log_status("init");
}

void nx_net_exit(void) {
  if (!s_inited) return;
  if (s_nifm_ok) { nifmExit(); s_nifm_ok = 0; }
  if (s_sock_ok) { socketExit(); s_sock_ok = 0; }
  s_inited = 0;
  nx_net_log_close();
}
