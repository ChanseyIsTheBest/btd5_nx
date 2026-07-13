/* config.h -- build-time constants for the Bloons TD 5 Switch port
 *
 * Bloons TD 5 (com.ninjakiwi.bloonstd5) runs on Ninja Kiwi's in-house native
 * C++ engine (class prefixes like CBloonsTD5Game / CBloonStore; analytics under
 * "DG..."). Determined by static analysis of lib/arm64-v8a/libnative.so:
 *
 *   - App model:  Java GLSurfaceView + JNI (NOT a NativeActivity). The Java
 *                 shell (com/ninjakiwi/MainActivity) creates the GL surface,
 *                 passes it down via ANativeWindow_fromSurface, and drives the
 *                 engine through nativeLoad / nativeSurfaceCreated / nativeResize
 *                 / nativeTick / nativeTouch* callbacks it registers via
 *                 RegisterNatives inside JNI_OnLoad.
 *   - Renderer:   GLES2 + EGL (NEEDED: libGLESv2, libEGL).
 *   - Audio:      OpenSL ES (NEEDED: libOpenSLES) -> emulated in opensles.c.
 *   - Assets:     Android AssetManager (AAssetManager_open) over a packed
 *                 archive "Assets/BTD5.jet" plus loose Assets/JSON, Assets/Audio,
 *                 Assets/Fonts trees.
 *   - C++ runtime is STATICALLY linked (only 2 external C++ symbols), so unlike
 *                 the Chaos Rings III donor there is NO separate libc++_shared.so
 *                 to ship.
 *   - Network / ads / IAP / license (IronSource, AppLovin, Crashlytics, Google
 *                 Play Licensing) are stubbed to fail-safe offline -- see
 *                 ninjakiwi.c. You must own the game; supply libnative.so and the
 *                 Assets/ tree from YOUR OWN copy.
 *
 * MIT license -- see LICENSE.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// The single native module from the APK's lib/arm64-v8a/.
#define SO_NAME  "libnative.so"

#define DATA_DIR "sdmc:/switch/btd5"
#define LOG_NAME DATA_DIR "/btd5_nx.log"

// Address-space split (see __libnx_initheap in main.c). libnative.so is ~12 MB
// of code+data; reserve a fixed zone for it + relocation scratch and give the
// rest to newlib's heap (textures, audio, the parsed JSON/jet game state).
#define SO_ZONE_MB 160

// Logging on during bring-up. Set 0 once stable.
#define DEBUG_LOG 0

/* Pin EVERY thread (main + all engine workers + audio) to one CPU core.
 *
 * This does not remove races outright -- Horizon still preempts -- but it
 * removes true parallelism, so two threads can no longer be executing inside the
 * same critical section on two CPUs simultaneously. That closes most race
 * windows. Diagnostic value:
 *   - if the crashes/hangs STOP with this on  -> the bug is a data race
 *   - if they persist identically             -> it is NOT a race; look elsewhere
 * Costs performance (all engine work on one core), so turn off once stable. */
/* Upper bound on a blocking condvar/futex wait, in ms.  0 = NO CAP.
 *
 *   0   -> real, indefinite blocking waits. Idle engine threads sleep until they
 *          are actually signalled and use ZERO cpu. This is how a normal pthread
 *          implementation behaves, and it is what we want.
 *   >0  -> every wait wakes at least this often, whether signalled or not. This
 *          was a defence against a suspected lost wakeup -- but that theory was
 *          wrong. The real bug was returning newlib's ETIMEDOUT (116) where the
 *          engine's boost expected bionic's (110), which made every timed wait
 *          throw an uncaught exception. With that fixed, the cap does nothing
 *          except wake every idle thread 62x/second (at 16ms) and burn cpu.
 *
 * Set to 0 now that the errno bug is fixed. If the game ever hangs on a lost
 * wakeup, put a value back here (100 is a good re-poll rate) -- that instantly
 * restores the old safety net without any other change. */
#define COND_WAIT_CAP_MS 0

#define SINGLE_CORE 0
#define SINGLE_CORE_ID 0

// Asset lookup roots, tried in order under the game dir. On Android,
// AAssetManager_open("Assets/x") means assets/Assets/x, so "assets" comes first.
// BTD5's engine asks for paths that already begin with "Assets/", so "." also
// resolves them if you drop the Assets/ folder directly next to the .nro.
#define ASSET_ROOTS { ".", "assets", "romfs:" }

// Packed archive the engine memory-maps first; loose files override.
#define JET_ARCHIVE "Assets/BTD5.jet"

extern int screen_width;
extern int screen_height;

#define CONFIG_NAME DATA_DIR "/config.txt"

typedef struct {
  int screen_width;    // 0 = automatic (per dock state)
  int screen_height;   // 0 = automatic
  int docked_width;    // default 1920
  int docked_height;   // default 1080
  char language[8];    // "auto" or 2-letter code
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
