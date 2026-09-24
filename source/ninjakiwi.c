/* ninjakiwi.c -- the Java "shell" classes Bloons TD 5's engine calls up into.
 *
 * On Android these are real Java classes in classes.dex. Here we reimplement,
 * natively, just enough of each for the engine to boot and run offline. Every
 * up-call the engine can make (from tools/extract_jni.py / JNI_MAP.md) is
 * answered in the safe "offline, owned, no ads" shape.
 *
 * Entry point: nk_upcall(cls,name,sig,self,ap) -- jni_fake.c funnels all
 * Call*Method[V] here. We parse Java args from `ap` per `sig` into argv[], then
 * match on (class,name) and fill the return jvalue.
 *
 * Classes (see JNI_MAP.md): MainActivity; Store/GoogleStore/AmazonStore/NoStore
 * (+ $Order/$Product); LicenseChecker/GoogleLicenseChecker; IronSourceInterface;
 * PlayServicesInterface (+ $PlayerDetails); NKAttribution; Notifications; plus
 * android/os/StatFs and android/net/TrafficStats.
 *
 * MIT license -- see LICENSE.
 */

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "jni_fake.h"
#include "ninjakiwi.h"
#include "music.h"
#include "util.h"
#include "config.h"
#include "nx_net.h"
#include "nx_keyboard.h"
#include "nx_paths.h"

/* Native result callbacks resolved in main.c (push async answers up). */
extern void nk_report_licensed(void);   /* -> nativeLicenseResult(0,0)          */
extern void nk_report_no_ads(void);     /* -> nativeOnRewardedVideoAvailability */

#define CLS(x)  (!strcmp(cls, (x)))
#define M(x)    (!strcmp(name, (x)))
#define HAS(x)  (strstr(cls, (x)) != NULL)
#define MHAS(x) (strstr(name, (x)) != NULL)

/* Pull Java args out of the va_list per the JNI signature. Object types become
 * a pointer; Z/B/C/S/I widen to int (JNI vararg promotion); J is jlong; F is
 * promoted to double in the vararg form; D is double. Enough for every up-call
 * BTD5 actually makes. */
static int parse_args(const char *sig, va_list ap, jvalue *argv, int cap) {
  const char *p = sig;
  if (*p == '(') p++;
  int n = 0;
  while (*p && *p != ')' && n < cap) {
    switch (*p) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
        argv[n].i = va_arg(ap, int); p++; break;
      case 'J':
        argv[n].j = va_arg(ap, long long); p++; break;
      case 'F':
        argv[n].f = (float)va_arg(ap, double); p++; break;
      case 'D':
        argv[n].d = va_arg(ap, double); p++; break;
      case 'L':
        argv[n].l = va_arg(ap, void *);
        while (*p && *p != ';') p++;   /* skip to end of class name */
        if (*p == ';') p++;
        break;
      case '[':
        argv[n].l = va_arg(ap, void *);
        p++;                            /* skip '[' */
        if (*p == 'L') { while (*p && *p != ';') p++; if (*p == ';') p++; }
        else if (*p) p++;               /* primitive array element */
        break;
      default: p++; continue;           /* unknown: don't consume */
    }
    n++;
  }
  return n;
}

jvalue nk_upcall(const char *cls, const char *name, const char *sig,
                 jobject self, va_list ap) {
  (void)self;
  jvalue r; r.j = 0;
  jvalue argv[8]; for (int i = 0; i < 8; i++) argv[i].j = 0;
  parse_args(sig, ap, argv, 8);

  /* Storage-path getters can arrive with the receiver's class reported as
   * java/lang/Object (the engine caches the activity as a bare jobject), so
   * match these by method name regardless of class. All point at our writable
   * game dir. */
  if (M("getInternalStoragePath") || M("getExternalStoragePath") ||
      M("getCacheStoragePath")    || M("getStorageDirectory") ||
      M("getExternalFilesDir")    || M("getFilesDir") ||
      M("getSaveDirectory")       || M("getDataDirectory")) {
    r.l = jni_make_string(nx_data_dir());
    return r;
  }

  /* Device / environment getters -- also class-agnostic (same cached-jobject
   * reason). Returning NULL here makes the engine spin retrying (it needs a
   * unique id + language to finish profile/analytics init), so answer them. */
  if (M("getUniqueID") || M("getUniqueDeviceID") || M("getDeviceID") ||
      M("getAndroidID") || M("getInstallID")) {
    /* Feeds Ninja Kiwi's UDID / device_id: it must differ per console, or
     * every Switch presents the same identity online (nx_net.c). */
    r.l = jni_make_string(nx_net_device_id()); return r;
  }
  if (M("getLanguageCode") || M("getDeviceLanguage") || M("getLanguage")) {
    /* Seed the engine's initial locale from the Switch system language. The
     * in-game language menu overrides this afterwards. */
    r.l = jni_make_string(nx_system_language());
    return r;
  }
  if (M("getCountryCode") || M("getCountry")) { r.l = jni_make_string("US"); return r; }
  if (M("getDeviceModel") || M("getModel"))   { r.l = jni_make_string("Nintendo Switch"); return r; }
  if (M("getDeviceBootTime") || M("getBootTime") || M("getElapsedRealtime")) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    r.j = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return r;
  }
  /* Connectivity: the real state from nifm (always 0 with online=0 in
   * config.txt). The engine mostly learns this by trying, through the socket
   * layer in nx_socket.c; these answer any explicit query consistently. */
  if (M("isOnline") || M("isNetworkAvailable") || M("hasNetworkConnection") ||
      M("isConnected") || M("isWifiConnected") || M("hasInternet")) {
    r.i = nx_net_online(); r.z = (jboolean)r.i; return r;
  }

  /* ---- Software keyboard (CDroidKeyboard -> MainActivity) ---------------
   * Matched by name on any class: the engine calls these on its cached
   * activity jobject, which can be reported as java/lang/Object. The Switch
   * keyboard itself opens between frames -- see nx_keyboard.h. */
  if (M("ShowKeyboard"))             { nxk_request(argv[0].i != 0); return r; }   /* (Z)V */
  if (M("SetKeyboardInputType"))     { nxk_set_input_type(argv[0].i); return r; } /* (I)V */
  if (M("SetKeyboardMaxCharacters")) { nxk_set_max_chars(argv[0].i); return r; }  /* (I)V */

  /* ---- MainActivity: device / display / environment --------------------- */
  if (CLS("com/ninjakiwi/MainActivity")) {
    if (M("getDeviceLanguage")) { r.l = jni_make_string(nx_system_language()); return r; }
    if (M("getDeviceModel"))  { r.l = jni_make_string("Nintendo Switch"); return r; }
    if (M("getCountryCode"))  { r.l = jni_make_string("US");              return r; }
    if (M("getBundleName"))   { r.l = jni_make_string("com.ninjakiwi.bloonstd5"); return r; }
    if (M("getUniqueDeviceID") || M("getDeviceID")) {
      r.l = jni_make_string(nx_net_device_id()); return r; }
    if (M("getStorageDirectory") || M("getExternalFilesDir") || M("getSaveDirectory")) {
      r.l = jni_make_string(nx_data_dir()); return r; }
    if (M("isNetworkAvailable") || M("hasNetworkConnection")) { r.z = (jboolean)nx_net_online(); return r; }
    if (M("useImmersiveMode")) { r.z = 1; return r; }         /* (FF)Z */
    if (M("hasClipboardTextEntry")) { r.i = 0; return r; }    /* ()I    */
    if (M("openURL")) { r.i = 0; return r; }                  /* no browser */
    if (M("copyToClipboard") || M("setScreenCanTimeout") ||
        M("showMessageBox")  || M("startSendIntent")) { return r; }  /* void, ignore */
    if (M("showKeyboard") || M("showSoftKeyboard")) {
      nk_request_keyboard(argv[0].l); r.z = 1; return r; }
  }

  /* ---- Licensing: user owns the game -> LICENSED ------------------------ */
  if (HAS("LicenseChecker")) {
    if (M("checkAccess") || M("check") || M("isLicensed")) {
      nk_report_licensed();            /* async path */
      r.z = 1; return r;               /* sync path  */
    }
  }

  /* ---- Store / IAP: behave like NoStore (nothing purchasable) ----------- */
  if (HAS("Store")) {
    if (M("isBillingSupported"))                 { r.z = 0; return r; }
    if (M("queryProducts") || M("getProducts") ||
        M("requestProductInfo")) {
      /* Returns a product array/list in Java. NULL here = engine NULL-deref;
       * hand back an empty non-NULL object instead. */
      r.i = 0; r.l = jni_make_object(); return r; }
    if (M("requestPurchase") || M("purchase") || M("buy") ||
        M("restorePurchases") || M("consume") || M("consumeOrders")) {
      r.i = 0; r.z = 0; return r; }
    if (M("verifyPayload"))                      { r.i = 1; return r; } /* treat as valid */
    if (M("terminate"))                          { return r; }
    /* anything else in Store falls through to the typed default (never NULL) */
  }

  /* ---- Ads (IronSource): never available -------------------------------- */
  if (CLS("com/ninjakiwi/IronSourceInterface")) {
    if (MHAS("isReady") || MHAS("isAvailable")) { r.z = 0; return r; }
    nk_report_no_ads();
    /* init/show/load are void no-ops; anything returning an object drops to the
     * typed default below so we never hand back NULL. */
    if (strrchr(sig, ')') && strrchr(sig, ')')[1] == 'V') return r;
    r.z = 0; r.i = 0;
  }

  /* ---- Play services (achievements/leaderboards/cloud/friends) ----------
   * Offline build: report "not signed in" and hand back empty-but-valid data.
   * IMPORTANT: do NOT `return r;` blindly here -- that returns NULL for the
   * object-returning methods (getPlayerId/getPlayerName/getFriends/...), and
   * the engine dereferences them. Answer the known ones explicitly and let
   * anything else fall through to the signature-aware default below. */
  if (HAS("PlayServicesInterface")) {
    if (MHAS("isSignedIn") || MHAS("isConnected") || MHAS("isAvailable") ||
        MHAS("isSupported") || MHAS("isEnabled"))            { r.z = 0; return r; }
    if (MHAS("PlayerId") || MHAS("PlayerID") || MHAS("AccountId")) {
      r.l = jni_make_string("");  return r; }                /* non-NULL empty id */
    if (MHAS("PlayerName") || MHAS("DisplayName") || MHAS("Nickname")) {
      r.l = jni_make_string(nx_net_player_name()); return r; }
    if (MHAS("Token") || MHAS("AuthCode")) { r.l = jni_make_string(""); return r; }
    if (MHAS("Friends") || MHAS("Invites") || MHAS("Players")) {
      r.l = jni_make_object(); return r; }                   /* empty, non-NULL */
    /* login/logout/show../unlock../submit../load.. are fire-and-forget voids;
     * anything not matched drops to the typed default below (never NULL). */
  }

  /* ---- Attribution / notifications: stub -------------------------------- */
  if (CLS("com/ninjakiwi/NKAttribution") || CLS("com/ninjakiwi/Notifications")) {
    /* void methods return here; object-returning ones fall through to the typed
     * default so they get a non-NULL stub rather than NULL. */
    if (strrchr(sig, ')') && strrchr(sig, ')')[1] == 'V') return r;
  }

  /* ---- android/os/StatFs: report ~2 GB free ----------------------------- */
  if (CLS("android/os/StatFs")) {
    if (MHAS("AvailableBytes") || MHAS("FreeBytes"))
      { r.j = (long long)2 * 1024 * 1024 * 1024; return r; }
    if (MHAS("BlockSize")) { r.j = 4096; return r; }
    if (MHAS("Count") || MHAS("Blocks")) { r.j = 512 * 1024; return r; }
    return r;
  }
  /* ---- Music: DroidMusicManager -> CustomMediaPlayer -------------------
   * BTD5's music path. On Android these wrap android.media.MediaPlayer; here
   * they drive music.c (minimp3 decode + SDL mixing). The receiver class is
   * usually reported as java/lang/Object (the engine caches the player as a
   * bare jobject), so match by method NAME regardless of class.
   *
   *   loadMusic(String)            -> CustomMediaPlayer   (our Music* handle)
   *   playMusic(player)            -> void   (looping)
   *   playMusicNoLoop(player)      -> void
   *   pauseMusic(player)           -> void
   *   killMusic/unloadMusic(player)-> void
   *   setVolume(player, float)     -> void
   */
  if (M("loadMusic")) {
    const char *fn = jni_cstr(argv[0].l);
    r.l = fn ? music_load(fn) : NULL;
    if (!r.l) r.l = jni_make_object();   /* never hand back NULL */
    return r;
  }
  if (M("playMusic"))       { music_play(argv[0].l, 1); return r; }
  if (M("playMusicNoLoop")) { music_play(argv[0].l, 0); return r; }
  if (M("pauseMusic"))      { music_pause(argv[0].l, 1); return r; }
  if (M("resumeMusic"))     { music_pause(argv[0].l, 0); return r; }
  if (M("killMusic") || M("stopMusic")) { music_stop(argv[0].l); return r; }
  if (M("unloadMusic"))     { music_unload(argv[0].l); return r; }
  if (M("setVolume")) {
    /* setVolume(CustomMediaPlayer, float): the float is argv[1]. */
    music_set_volume(argv[0].l, argv[1].f);
    return r;
  }

  if (CLS("android/net/TrafficStats")) { r.j = 0; return r; }

  /* ---- Fall-through: return a TYPE-CORRECT value ------------------------
   * Previously every unhandled method returned a zeroed jvalue. For a method
   * whose return type is an object (Ljava/lang/String;, an array, or any class)
   * that means handing the engine a NULL reference -- which it promptly
   * dereferences. Java code practically never null-checks the result of its own
   * helper calls, so a single unhandled object-returning method is an instant
   * crash deep inside engine code (with no hint that JNI was the cause).
   *
   * Parse the return type out of the signature -- the part after ')' -- and
   * hand back something safe and non-NULL for reference types. */
  {
    const char *ret = strrchr(sig, ')');
    const char rt = ret ? ret[1] : 'V';

    switch (rt) {
      case 'V':                       /* void */
        break;
      case 'Z': case 'B': case 'C':   /* boolean / byte / char */
      case 'S': case 'I':             /* short / int */
        r.i = 0; break;
      case 'J': r.j = 0; break;       /* long   */
      case 'F': r.f = 0.0f; break;    /* float  */
      case 'D': r.d = 0.0; break;     /* double */

      case 'L':                       /* an object: NEVER return NULL */
        /* A String is by far the most common object return in this engine
         * (ids, names, tokens); give an empty string. Anything else gets a
         * generic non-NULL object so a method call or field read on it lands
         * back in our fake-JNI layer instead of faulting on address 0. */
        if (!strncmp(ret + 1, "Ljava/lang/String;", 18))
          r.l = jni_make_string("");
        else
          r.l = jni_make_object();
        break;

      case '[':                       /* an array: also never NULL */
        /* The engine iterates arrays after calling GetArrayLength(); our fake
         * JNI reports length 0 for a generic object, so an empty non-NULL array
         * makes the loop a clean no-op rather than a NULL deref. */
        r.l = jni_make_object();
        break;

      default:
        r.j = 0; break;
    }

    /* Rate-limit: the engine calls some of these every frame (e.g.
     * NKAttribution.hasInfo), which floods the log. Log each distinct
     * class.method a few times, then stay quiet. */
    {
      static const char *seen[64];
      static int hits[64];
      static int nseen = 0;
      int i, idx = -1;
      for (i = 0; i < nseen; i++)
        if (seen[i] == name) { idx = i; break; }      /* name ptrs are stable */
      if (idx < 0 && nseen < 64) { idx = nseen++; seen[idx] = name; hits[idx] = 0; }
      if (idx >= 0 && hits[idx]++ < 3)
        debugPrintf("JNI unhandled: %s.%s %s -> default '%c'%s\n",
                    cls, name, sig, rt,
                    (rt == 'L' || rt == '[') ? " (non-NULL stub)" : "");
    }
  }
  return r;
}

/* Software keyboard. editbox.c owns the swkbd; here we just kick it off. The
 * typed result is delivered back to the engine via nativeInputTextChanged
 * (wired in main.c). Full wiring is a bring-up item -- see README. */
void nk_request_keyboard(jobject prompt) {
  (void)prompt;
  nxk_request(1);                  /* same path as ShowKeyboard(true) */
}
