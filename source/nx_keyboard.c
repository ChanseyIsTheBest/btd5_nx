/* nx_keyboard.c -- see nx_keyboard.h.
 *
 * MIT license -- see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "nx_keyboard.h"
#include "nx_net.h"          /* nx_net_logf: btd5_net.log works in every build */

#define KBD_DEFAULT_MAX 64
#define KBD_ABS_MAX     500

static volatile int s_pending;
static volatile int s_input_type = -1;
static volatile int s_max_chars;
/* Reopen guard: after the keyboard closes, a new request is honoured only once
 * the player has tapped/clicked again. A screen that re-requests the keyboard
 * on its own (on "hidden", or every frame while its field has focus) can then
 * never trap the player in a loop of keyboards. */
static volatile unsigned s_taps, s_taps_at_close;
static volatile int s_closed_once;
static Mutex s_run_lock;               /* one applet at a time, ever */

void nxk_set_input_type(int type) {
  if (type != s_input_type)
    nx_net_logf("[kbd] input type %d\n", type);  /* NK's enum; logged for reference */
  s_input_type = type;
}

void nxk_set_max_chars(int max) { s_max_chars = max; }

void nxk_note_tap(void) { s_taps++; }

void nxk_request(int show) {
  if (!show) {                          /* engine hides: drop a request not yet run */
    if (s_pending) nx_net_logf("[kbd] hide requested before the keyboard opened -- dropped\n");
    s_pending = 0;
    return;
  }
  if (s_closed_once && s_taps == s_taps_at_close) {
    static int told;
    if (told++ < 3)
      nx_net_logf("[kbd] show requested again without a new tap -- ignored (reopen guard)\n");
    return;
  }
  s_pending = 1;
}

int nxk_pending(void) { return s_pending; }

int nxk_run(char *out, size_t cap) {
  if (!out || cap < 2) return 0;
  out[0] = 0;
  if (!mutexTryLock(&s_run_lock)) {     /* a second applet would take the console down */
    s_pending = 0;
    return 0;
  }
  s_pending = 0;

  int max = s_max_chars;
  if (max <= 0) max = KBD_DEFAULT_MAX;
  if (max > KBD_ABS_MAX) max = KBD_ABS_MAX;
  if ((size_t)max * 4 + 1 > cap) max = (int)((cap - 1) / 4);   /* UTF-8: up to 4 bytes/char */

  int ok = 0;
  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_SUCCEEDED(rc)) {
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetStringLenMax(&kbd, (u32)max);
    swkbdConfigSetBlurBackground(&kbd, true);
    rc = swkbdShow(&kbd, out, cap);     /* blocks: the system applet owns the screen */
    swkbdClose(&kbd);
    ok = R_SUCCEEDED(rc);
    if (!ok) out[0] = 0;
  } else {
    nx_net_logf("[kbd] swkbdCreate failed (0x%x)\n", rc);
  }

  s_taps_at_close = s_taps;
  s_closed_once = 1;
  mutexUnlock(&s_run_lock);
  nx_net_logf("[kbd] keyboard closed: %s (%d chars, max %d)\n",
              ok ? "confirmed" : "cancelled", ok ? (int)strlen(out) : 0, max);
  return ok;
}
