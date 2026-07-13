/* main.c -- Bloons TD 5 Switch port: loader + lifecycle + input + frame loop.
 *
 * BRING-UP SCAFFOLD. The lifecycle below mirrors the callback set the engine
 * registered via RegisterNatives (extracted from libnative.so):
 *
 *   load:     nativeLoad -> nativeSurfaceCreated -> nativeResize -> nativeResume
 *   frame:    nativeTick   (updates AND renders in this engine; swap after)
 *   input:    nativeTouchStarted / nativeTouchHeld / nativeTouchEnded /
 *             nativeTouchCancelled ; nativeBackPressed for the Back key
 *   suspend:  nativePause / nativeResume ; nativeLostAudioFocus/Gained
 *   teardown: nativeSurfaceDestroyed -> nativeUnload
 *
 * The exact argument shapes (does nativeResize take (env,thiz,w,h)? does
 * nativeTouch* take a pointer id + float or int coords?) are confirmed on
 * device from the DEBUG_LOG trace -- the two most likely variants are noted at
 * each call and are one-line switches.
 *
 * MIT license -- see LICENSE.
 */

#include <switch.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "so_util.h"
#include "jni_fake.h"
#include "ninjakiwi.h"
#include "opensles.h"
#include "util.h"
#include "error.h"
#include "platform.h"
#include "imports.h"

int screen_width  = 0;
int screen_height = 0;
Config config;

static so_module game_mod;

/* ---- resolved engine entry points -- signatures are the GROUND TRUTH from
 * tools/extract_jni.py (RegisterNatives table 0/1), not guesses:
 *   nativeLoad            (Lcom/ninjakiwi/MainActivity;Ljava/lang/Object;)V
 *   nativeSurfaceCreated  (Landroid/view/Surface;II)V
 *   nativeResize          (II)V
 *   nativeTick            ()V
 *   nativeTouchStarted    (FFI)V   -> (x, y, pointerId)     [NOTE: x,y FIRST]
 *   nativeTouchEnded      (FFI)V   -> (x, y, pointerId)
 *   nativeTouchCancelled  (FFI)V   -> (x, y, pointerId)
 *   nativeTouchHeld       (FFIZ)V  -> (x, y, pointerId, moved)  [extra bool]
 *   nativeInputKeyDown/Up (II)V    -> (keycode, meta)  [Back = AKEYCODE_BACK=4]
 *   nativeLicenseResult   (II)V    -> (result, reason)
 * There is NO nativeBackPressed callback -- Back is a key event. */
typedef void (*fn_v)    (JNIEnv env, void *thiz);
typedef void (*fn_wh)   (JNIEnv env, void *thiz, int w, int h);
typedef void (*fn_surf) (JNIEnv env, void *thiz, void *surface, int w, int h);
typedef void (*fn_load) (JNIEnv env, void *thiz, void *activity, void *obj);
typedef void (*fn_tch)  (JNIEnv env, void *thiz, float x, float y, int id);
typedef void (*fn_tchh) (JNIEnv env, void *thiz, float x, float y, int id, unsigned char moved);
typedef void (*fn_key)  (JNIEnv env, void *thiz, int keycode, int meta);
typedef void (*fn_lic)  (JNIEnv env, void *thiz, int result, int reason);

static fn_load e_nativeLoad;
static fn_v    e_nativeUnload;
static fn_surf e_nativeSurfaceCreated;
static fn_v    e_nativeSurfaceDestroyed;
static fn_wh   e_nativeResize;
static fn_v    e_nativeTick;
static fn_v    e_nativePause, e_nativeResume;
static fn_tch  e_nativeTouchStarted, e_nativeTouchEnded, e_nativeTouchCancelled;
static fn_tchh e_nativeTouchHeld;
static fn_key  e_nativeInputKeyDown, e_nativeInputKeyUp;
static fn_v    e_nativeGainedAudioFocus, e_nativeLostAudioFocus;
static fn_lic  e_nativeLicenseResult;
static void  (*e_nativeRVAvailability)(JNIEnv env, void *thiz, unsigned char available);

#define AKEYCODE_BACK 4

/* fake_env / fake_vm are the globals jni_init() sets up (defined in jni_fake.c).
 * We pass fake_env as the JNIEnv* to every engine callback. */
extern JNIEnv fake_env;
extern JavaVM fake_vm;
static void *thiz;   /* stand-in MainActivity object the engine holds */

static void resolve_entry_points(void)
{
  /* Resolve by the name the engine passed to RegisterNatives (captured in
   * jf_RegisterNatives). This binds obfuscated/static callbacks too -- e.g.
   * nativeLicenseResult, whose .dynsym symbol is 'ox94jnabared'. */
  #define REQ(v, name) do { v = (void*)jni_registered(name); } while (0)
  #define OPT(v, name) v = (void*)jni_registered(name)

  /* Registered names (Java side) -- see JNI_MAP.md tables 0/1/3. */
  REQ(e_nativeLoad,            "nativeLoad");
  REQ(e_nativeSurfaceCreated,  "nativeSurfaceCreated");
  REQ(e_nativeResize,          "nativeResize");
  REQ(e_nativeTick,            "nativeTick");
  OPT(e_nativeUnload,          "nativeUnload");
  OPT(e_nativeSurfaceDestroyed,"nativeSurfaceDestroyed");
  OPT(e_nativePause,           "nativePause");
  OPT(e_nativeResume,          "nativeResume");
  OPT(e_nativeTouchStarted,    "nativeTouchStarted");
  OPT(e_nativeTouchHeld,       "nativeTouchHeld");
  OPT(e_nativeTouchEnded,      "nativeTouchEnded");
  OPT(e_nativeTouchCancelled,  "nativeTouchCancelled");
  OPT(e_nativeInputKeyDown,    "nativeInputKeyDown");
  OPT(e_nativeInputKeyUp,      "nativeInputKeyUp");
  OPT(e_nativeGainedAudioFocus,"nativeGainedAudioFocus");
  OPT(e_nativeLostAudioFocus,  "nativeLostAudioFocus");
  OPT(e_nativeLicenseResult,   "nativeLicenseResult");
  OPT(e_nativeRVAvailability,  "nativeOnRewardedVideoAvailabilityChanged");

  if (!e_nativeTick || !e_nativeLoad)
    fatal_error("Could not resolve engine entry points -- see log.\n"
                "The RegisterNatives names may differ in your build; check the "
                "DEBUG_LOG trace for the registered method table.");
}

/* Called from ninjakiwi.c to push async results back into the engine.
 * nativeLicenseResult(result, reason) maps result -> an internal tri-state
 * (disasm of +0x7a2aa4): result==-1 -> 0 (error), result==1 -> 2 (denied),
 * anything else (incl. 0) -> 1. State 1 is the "allow" value (Android LVL
 * convention: 0 == LICENSED), so we send (0, 0). */
void nk_report_licensed(void) { if (e_nativeLicenseResult) e_nativeLicenseResult(fake_env, thiz, 0, 0); }
void nk_report_no_ads(void)   { if (e_nativeRVAvailability) e_nativeRVAvailability(fake_env, thiz, 0); }

/* ---- input: platform pointer events -> BTD5 touch callbacks -------------- *
 * platform.c unifies handheld touchscreen fingers and the docked stick-cursor
 * into PtrEvents. We translate each to the right callback. Signature order is
 * (x, y, pointerId) with x,y FIRST, and nativeTouchHeld takes a trailing
 * 'moved' bool -- both confirmed by extract_jni. */
static void pump_input(void)
{
  PtrEvent ev[16];
  int n = platform_poll_pointers(ev, 16);
  for (int i = 0; i < n; i++) {
    float x = ev[i].x, y = ev[i].y; int id = ev[i].id;
    switch (ev[i].phase) {
      case PTR_DOWN: if (e_nativeTouchStarted) e_nativeTouchStarted(fake_env,thiz,x,y,id); break;
      case PTR_MOVE: if (e_nativeTouchHeld)    e_nativeTouchHeld(fake_env,thiz,x,y,id,1);  break;
      case PTR_UP:   if (e_nativeTouchEnded)   e_nativeTouchEnded(fake_env,thiz,x,y,id);   break;
    }
  }
  /* B / + -> Android Back key (there is no nativeBackPressed callback). */
  if (e_nativeInputKeyDown && back_edge_pressed())  e_nativeInputKeyDown(fake_env,thiz,AKEYCODE_BACK,0);
  if (e_nativeInputKeyUp   && back_edge_released()) e_nativeInputKeyUp(fake_env,thiz,AKEYCODE_BACK,0);
}

/* ---------------------------------------------------------------------------
 * The platform layer (heap split, GLES2/EGL context, and input -- handheld
 * touch + docked stick cursor) lives in platform.c behind platform.h. main()
 * below is just the loader + JNI + lifecycle driver.
 * ------------------------------------------------------------------------- */

/* Tiny startup breadcrumb: overwrite a one-line file at each major step so a
 * crash that leaves no clean backtrace still tells us exactly how far we got.
 * fd-based, no stdio locks. Reads back as e.g. "5 so_resolve". */
static void stage(const char *s) {
  /* Log via debugPrintf only (libnx fsFileWrite path). The previous version
   * also did its own open/write/close of btd5_stage.txt with newlib -- a second
   * unlocked fd-table user on the main thread that raced the engine's asset
   * opens on worker threads. debugPrintf already records every stage. */
  debugPrintf(">>> STAGE %s\n", s);
}

static unsigned long long g_frame_count = 0;

int main(int argc, char *argv[])
{
  (void)argc; (void)argv;
  stage("0 enter main");
  mkdir("sdmc:/switch", 0777);
  mkdir(DATA_DIR, 0777);              /* so debugPrintf/crash log can be written */
  /* Surface the previous run's crash dump in the main log for convenience. */
  {
    FILE *pf = fopen(DATA_DIR "/btd5_crash.log", "rb");
    if (pf) {
      char cb[256]; size_t cn;
      debugPrintf("=== PREVIOUS RUN btd5_crash.log ===\n");
      while ((cn = fread(cb, 1, sizeof cb - 1, pf)) > 0) { cb[cn] = 0; debugPrintf("%s", cb); }
      debugPrintf("\n=== END PREVIOUS CRASH ===\n");
      fclose(pf);
    }
  }
  extern void crash_log_open(void);
  crash_log_open();                  /* (re)open + truncate the dedicated crash file */
  cpu_boost(1);                       /* run at full clocks */
  tls_setup_guard();                  /* TLS slot the .so's TLS access expects */
  pin_current_thread();               /* SINGLE_CORE: main thread on the same core */
  read_config(CONFIG_NAME);
  stage("1 config read");

  /* 1. map + relocate + resolve the game library against our shim table */
  stage("2 so_load: heap alloc + open libnative.so");
  if (so_load(&game_mod, SO_NAME, heap_so_base(), heap_so_limit()) < 0)
    fatal_error("Couldn't load " SO_NAME ".\nPut it (and the Assets/ folder) "
                "from YOUR OWN copy of Bloons TD 5 next to this .nro.");
  stage("3 so_relocate");
  so_relocate(&game_mod);
  stage("4 so_resolve");
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);
  /* CRITICAL: map the image as executable (svcMapProcessCodeMemory + set the
   * code pages Perm_Rx) and flush caches, or the first jump into the .so
   * (JNI_OnLoad / init_array) faults with an instruction abort. */
  stage("4b so_finalize (map code RX)");
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  /* 2. GLES2/EGL context. Audio needs no init here: the engine creates its own
   *    OpenSL ES device through the imported slCreateEngine (imports.c). */
  stage("5 egl_init_context");
  egl_init_context();                /* 1080p docked / 720p handheld */

  /* 3. run the engine's C++ constructors, then set up the fake JNIEnv/JavaVM and
   *    call JNI_OnLoad so it RegisterNatives. init_array must run before
   *    JNI_OnLoad (globals it touches must be constructed first). */
  stage("6a so_execute_init_array");
  so_execute_init_array(&game_mod);
  stage("6b jni_init + JNI_OnLoad");
  jni_init();                         /* fills globals fake_env + fake_vm */
  thiz = jni_make_activity();
  jint (*jni_onload)(JavaVM vm, void *reserved) =
      (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");
  if (jni_onload) jni_onload(fake_vm, NULL);
  stage("7 resolve_entry_points");
  resolve_entry_points();

  /* 4. lifecycle: load -> surface -> resize -> resume
   *   nativeLoad(activity, obj): the engine reads the AssetManager / context off
   *     these. We pass the fake activity for both; if it dereferences fields,
   *     the DEBUG_LOG + crash handler will show which -- build them in jni_fake.
   *   nativeSurfaceCreated(surface, w, h): 'surface' feeds ANativeWindow_fromSurface
   *     (shimmed in libc_shim.c to hand back our EGL-backed window). */
  void *surface  = jni_make_surface();     /* android.view.Surface stand-in   */
  void *assetmgr = jni_make_object();      /* android AssetManager stand-in   */
  /* nativeLoad(activity, assetManager): arg3 is NewGlobalRef'd and kept as the
   * MainActivity receiver; arg4 is passed to AAssetManager_fromJava. (Confirmed
   * by disassembly -- tools/disasm_fn.py 0x77f2bc.) */
  stage("9 nativeLoad");
  e_nativeLoad(fake_env, thiz, thiz, assetmgr);
  stage("10 nativeSurfaceCreated");
  if (e_nativeSurfaceCreated)
    e_nativeSurfaceCreated(fake_env, thiz, surface, screen_width, screen_height);
  stage("11 nativeResize");
  e_nativeResize(fake_env, thiz, screen_width, screen_height);
  if (e_nativeResume) e_nativeResume(fake_env, thiz);
  if (e_nativeGainedAudioFocus) e_nativeGainedAudioFocus(fake_env, thiz);
  stage("12 entering frame loop");

  /* 5. frame loop */
  int first_tick = 1;
  const u64 t0 = armGetSystemTick();
  /* Watchdog disabled: it never produced a usable dump and adds a thread (and a
   * forced core affinity) that we don't want while chasing a system-level crash.
   * Re-enable only if we need a stall dump again. */
  while (appletMainLoop()) {
    if (first_tick) stage("12a before padUpdate");
    padUpdate_all();
    if (first_tick) stage("12b after padUpdate");
    if (should_quit()) break;
    if (handle_dock_change(&screen_width, &screen_height)) {
      stage("13 dock nativeResize");
      e_nativeResize(fake_env, thiz, screen_width, screen_height);
    }
    /* Don't feed input to the engine until it has rendered its first frame:
     * on Android touch/key events only arrive after the first frame, and the
     * engine's input handlers dereference state that the first nativeTick sets
     * up. Sending a touch on frame 0 faults. So we skip pump_input on the very
     * first iteration and dispatch normally (before the tick) thereafter. */
    if (!first_tick) {
      pump_input();
    }
    if (first_tick) stage("14 first nativeTick");
    e_nativeTick(fake_env, thiz);    /* engine updates, renders, and presents */
    if (first_tick) { stage("15 running"); first_tick = 0; }

    /* Heartbeat: the loop is otherwise silent after stage 15, so a log ending at
     * "15 running" can't tell a healthy 60fps game from a frame-2 hang. Print a
     * line every 60 frames with the frame count so the log shows whether the loop is
     * actually turning (and at what rate). */
    if ((++g_frame_count % 60) == 0) {
      const u64 now = armGetSystemTick();
      const double secs = (double)(now - t0) / 19200000.0;   /* NX tick = 19.2MHz */
      debugPrintf("[hb] frame %llu  t=%.1fs  fps=%.1f\n",
                  (unsigned long long)g_frame_count, secs,
                  secs > 0.0 ? (double)g_frame_count / secs : 0.0);
      /* Flush once a second so a crash still leaves the recent log on disk,
       * without paying an SD sync on every single line. */
      debugLogFlush();
    }

    egl_swap_buffers();              /* no-op: engine presents itself */
  }

  /* 6. teardown */
  if (e_nativePause) e_nativePause(fake_env, thiz);
  if (e_nativeSurfaceDestroyed) e_nativeSurfaceDestroyed(fake_env, thiz);
  if (e_nativeUnload) e_nativeUnload(fake_env, thiz);
  opensles_shutdown();
  debugLogFlush();                 /* push the buffered log to the SD card */
  egl_exit_context();
  cpu_boost(0);
  return 0;
}
