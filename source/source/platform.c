/* platform.c -- Switch platform layer for the BTD5 port.
 *
 * Implements the pieces main.c factors out: the SO load buffer, the GLES2/EGL
 * context (recreated on dock/undock so docked runs 1080p and handheld 720p),
 * and input -- handheld touchscreen fingers plus a docked left-stick cursor
 * with A as the tap "finger", both delivered as unified PtrEvents.
 *
 * MIT license -- see LICENSE.
 */

#include <switch.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "platform.h"
#include "nx_pointer.h"
#include "libc_shim.h"   /* fopen_fake/fclose_fake: locked newlib file I/O */
#include "util.h"

/* ===================== SO load zone ======================================= *
 * so_load() uses this as the RW buffer it assembles the mapped image into
 * (then maps to executable virtmem itself). It must be >= the .so's aligned
 * load size and persist for the process lifetime. A page-aligned allocation
 * from the (large) applet heap is all that's needed; if you ever run out of
 * memory for textures, add a __libnx_initheap override that claims full RAM. */
static void  *g_so_base = NULL;
static size_t g_so_size = 0;

void *heap_so_base(void) {
  if (!g_so_base) {
    g_so_size = (size_t)SO_ZONE_MB * 1024 * 1024;
    g_so_base = memalign(0x1000, g_so_size);
    if (!g_so_base) debugPrintf("heap_so_base: memalign(%zu) failed\n", g_so_size);
  }
  return g_so_base;
}
size_t heap_so_limit(void) { heap_so_base(); return g_so_size; }

/* ===================== GLES2 / EGL ======================================== *
 * BTD5's engine owns EGL: it imports the full creation set (eglGetDisplay/
 * Initialize/ChooseConfig/CreateContext/CreateWindowSurface/MakeCurrent/
 * SwapBuffers) and creates its own context + window surface on our NWindow
 * inside nativeSurfaceCreated (via ANativeWindow_fromSurface). So the wrapper
 * must NOT create or touch EGL -- doing so (and especially destroying/recreating
 * the surface on dock change) corrupts the engine's live GL state. These are
 * therefore no-ops; the engine renders and presents (eglSwapBuffers) itself
 * inside nativeTick. */
void egl_init_context(void) {
  debugPrintf("egl: engine-owned (wrapper does not create a context)\n");
}
void egl_swap_buffers(void) { /* engine swaps inside nativeTick */ }
void egl_exit_context(void) { /* engine tears down its own EGL */ }

/* ===================== Input =============================================
 * All pointer input (touchscreen, USB mouse, stick cursor) and the on-screen
 * cursor overlay now live in the reusable nx_pointer module, so the same
 * control scheme can be dropped straight into other Switch/Android ports.
 * platform.c just adapts it to this port's existing API.
 * ======================================================================== */

static void nxp_logger(const char *msg) { debugPrintf("%s", (char *)msg); }

static void ensure_pointer(void) {
  static int done = 0;
  if (done) return;
  done = 1;
  NxpConfig cfg = {0};
  cfg.screen_w  = screen_width;
  cfg.screen_h  = screen_height;
  cfg.panel_w   = 1280;            /* Switch touch panel is always 1280x720 */
  cfg.panel_h   = 720;
  cfg.data_dir  = DATA_DIR;        /* cursor.png is read from here */
  cfg.cursor_id = 8;               /* keep clear of touch ids 0..7 */
  cfg.max_touch_slots = 8;
  cfg.log       = nxp_logger;
  /* Give the module our LOCKED file wrappers. It saves pointer.cfg at runtime,
   * while the engine's worker threads are doing their own file I/O -- an
   * unlocked open/close from here would race devkitPro's (non-thread-safe)
   * handle table and corrupt it. */
  cfg.fopen_fn  = fopen_fake;
  cfg.fclose_fn = fclose_fake;
  nxp_init(&cfg);
}

void padUpdate_all(void) {
  ensure_pointer();
  nxp_update();
}

/* Draw the cursor on top of the engine's finished frame (called from
 * eglSwapBuffers_fake, which runs with the GL context current). */
void cursor_draw(void) { nxp_draw(); }

int platform_poll_pointers(PtrEvent *out, int max) {
  /* NxpEvent and PtrEvent are the same {int id; float x,y; int phase;} layout. */
  return nxp_poll((NxpEvent *)out, max);
}


/* Back key and the quit combo were removed: BTD5 has no use for an Android Back
 * event, and HOME already exits the port cleanly, so a button combo to quit was
 * redundant. (ZL/ZR/A all confirm; see nx_pointer.) */

int handle_dock_change(int *w, int *h) {
  /* Fixed 1080p in every mode -- never signal a resolution change, so the
   * frame loop never re-resizes the engine's surface. */
  (void)w; (void)h;
  return 0;
}

/* ---------------------------------------------------------------------------
 * EGL tracing wrappers.
 *
 * The engine owns EGL: it calls eglGetDisplay/eglInitialize/eglChooseConfig/
 * eglCreateContext/eglCreateWindowSurface/eglMakeCurrent/eglSwapBuffers itself.
 * The frame loop runs at a steady 60fps and the engine compiles shaders and
 * uploads textures -- yet the screen stays black, which means something in this
 * chain is failing silently (the engine does not check every return). These
 * wrappers log each step and its EGL error code so we can see exactly which one
 * breaks, instead of guessing.
 * ------------------------------------------------------------------------- */

EGLDisplay eglGetDisplay_fake(EGLNativeDisplayType dpy) {
  EGLDisplay d = eglGetDisplay(dpy);
  debugPrintf("egl: eglGetDisplay(%p) -> %p  err=0x%x\n",
              (void *)dpy, (void *)d, eglGetError());
  return d;
}

EGLBoolean eglInitialize_fake(EGLDisplay d, EGLint *maj, EGLint *min) {
  EGLBoolean r = eglInitialize(d, maj, min);
  debugPrintf("egl: eglInitialize(%p) -> %d  v%d.%d  err=0x%x\n",
              (void *)d, (int)r, maj ? *maj : -1, min ? *min : -1, eglGetError());
  return r;
}

EGLBoolean eglChooseConfig_fake(EGLDisplay d, const EGLint *attrib,
                                EGLConfig *cfgs, EGLint n, EGLint *num) {
  EGLBoolean r = eglChooseConfig(d, attrib, cfgs, n, num);
  debugPrintf("egl: eglChooseConfig -> %d  got %d config(s)  err=0x%x\n",
              (int)r, num ? *num : -1, eglGetError());
  return r;
}

EGLContext eglCreateContext_fake(EGLDisplay d, EGLConfig c,
                                 EGLContext share, const EGLint *attrib) {
  EGLContext ctx = eglCreateContext(d, c, share, attrib);
  debugPrintf("egl: eglCreateContext -> %p  err=0x%x%s\n",
              (void *)ctx, eglGetError(),
              ctx == EGL_NO_CONTEXT ? "   *** NO CONTEXT ***" : "");
  return ctx;
}

EGLSurface eglCreateWindowSurface_fake(EGLDisplay d, EGLConfig c,
                                       EGLNativeWindowType win, const EGLint *attrib) {
  EGLSurface s = eglCreateWindowSurface(d, c, win, attrib);
  debugPrintf("egl: eglCreateWindowSurface(win=%p) -> %p  err=0x%x%s\n",
              (void *)win, (void *)s, eglGetError(),
              s == EGL_NO_SURFACE ? "   *** NO SURFACE -> BLACK SCREEN ***" : "");
  return s;
}

EGLBoolean eglMakeCurrent_fake(EGLDisplay d, EGLSurface draw,
                               EGLSurface read, EGLContext ctx) {
  EGLBoolean r = eglMakeCurrent(d, draw, read, ctx);
  debugPrintf("egl: eglMakeCurrent(draw=%p ctx=%p) -> %d  err=0x%x%s\n",
              (void *)draw, (void *)ctx, (int)r, eglGetError(),
              r ? "" : "   *** FAILED -> GL calls go nowhere ***");
  return r;
}

void gl_frame_report(void);   /* defined below */
void cursor_draw(void);       /* defined below */

EGLBoolean eglSwapBuffers_fake(EGLDisplay d, EGLSurface s) {
  gl_frame_report();          /* what did the engine actually draw this frame? */
  cursor_draw();              /* overlay the cursor on the finished frame */
  EGLBoolean r = eglSwapBuffers(d, s);
  static unsigned n = 0;
  /* log the first few and then once a second, so we can see it is presenting */
  if (n < 5 || (n % 300) == 0) {
    const GLenum gl = glGetError();
    debugPrintf("egl: eglSwapBuffers #%u(surf=%p) -> %d  eglerr=0x%x  glerr=0x%x%s\n",
                n, (void *)s, (int)r, eglGetError(), gl,
                r ? "" : "   *** SWAP FAILED -> nothing presented ***");
  }
  n++;
  return r;
}

/* ---------------------------------------------------------------------------
 * GL frame tracing.
 *
 * EGL is proven good (valid surface/context, MakeCurrent OK, SwapBuffers
 * succeeding every frame with glerr=0) and the game logic is alive (it loads
 * and plays menu music). So the engine is drawing into a working surface and we
 * still see black. These wrappers answer the remaining questions:
 *
 *   - is the viewport the full 1920x1080, or degenerate/0-sized?
 *   - what colour is it clearing to?
 *   - is it issuing any draw calls at all, and how many per frame?
 *   - is it rendering into FBO 0 (the screen) or into an offscreen FBO it
 *     never blits back?
 * ------------------------------------------------------------------------- */

static unsigned g_draws, g_clears, g_frame;
static GLuint   g_fbo_bound;

void glViewport_fake(GLint x, GLint y, GLsizei w, GLsizei h) {
  /* SAFETY NET: the engine was setting a 0x0 viewport (it queried the surface
   * size and got 0), which clips every triangle -> black screen despite 26 draw
   * calls a frame. A zero-area viewport is never legitimate here, so clamp it to
   * the real screen. eglQuerySurface_fake fixes the source; this guarantees the
   * viewport regardless. */
  if (w <= 0 || h <= 0) {
    static int warned = 0;
    if (!warned) {
      warned = 1;
      debugPrintf("gl: glViewport(%d,%d,%d,%d) DEGENERATE -> forcing %dx%d\n",
                  x, y, w, h, screen_width, screen_height);
    }
    x = 0; y = 0; w = screen_width; h = screen_height;
  }
  static GLint lx = -1, ly = -1; static GLsizei lw = -1, lh = -1;
  if (x != lx || y != ly || w != lw || h != lh) {
    debugPrintf("gl: glViewport(%d,%d,%d,%d)\n", x, y, w, h);
    lx = x; ly = y; lw = w; lh = h;
  }
  glViewport(x, y, w, h);
}

void glClearColor_fake(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  static GLfloat lr = -1, lg = -1, lb = -1, la = -1;
  if (r != lr || g != lg || b != lb || a != la) {
    debugPrintf("gl: glClearColor(%.2f, %.2f, %.2f, %.2f)\n", r, g, b, a);
    lr = r; lg = g; lb = b; la = a;
  }
  glClearColor(r, g, b, a);
}

void glClear_fake(GLbitfield mask) {
  g_clears++;
  glClear(mask);
}

void glBindFramebuffer_fake(GLenum target, GLuint fb) {
  g_fbo_bound = fb;
  glBindFramebuffer(target, fb);
}

void glDrawArrays_fake(GLenum mode, GLint first, GLsizei count) {
  g_draws++;
  glDrawArrays(mode, first, count);
}

void glDrawElements_fake(GLenum mode, GLsizei count, GLenum type, const void *idx) {
  g_draws++;
  glDrawElements(mode, count, type, idx);
}

/* Called from eglSwapBuffers_fake once per presented frame. */
void gl_frame_report(void) {
  if (g_frame < 5 || (g_frame % 120) == 0) {
    debugPrintf("gl: frame %u -- %u draw call(s), %u clear(s), fbo=%u%s\n",
                g_frame, g_draws, g_clears, g_fbo_bound,
                g_draws == 0 ? "   *** NO DRAW CALLS -> engine is drawing nothing ***"
                             : (g_fbo_bound != 0 ? "   *** rendering into an FBO, not the screen ***" : ""));
  }
  g_draws = 0;
  g_clears = 0;
  g_frame++;
}

/* eglQuerySurface: the engine asks EGL for the surface size and uses it for
 * glViewport. It ended up with 0x0 -- everything it drew was clipped away
 * (26 draw calls/frame into FBO 0, black screen). Log what mesa actually
 * reports, and if it hands back a zero/failed size, substitute the real
 * screen size so the viewport can never be degenerate. */
EGLBoolean eglQuerySurface_fake(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val) {
  EGLBoolean r = eglQuerySurface(d, s, attr, val);
  if (attr == EGL_WIDTH || attr == EGL_HEIGHT) {
    const int want = (attr == EGL_WIDTH) ? screen_width : screen_height;
    static int logged = 0;
    if (logged < 6) {
      logged++;
      debugPrintf("egl: eglQuerySurface(%s) -> %d (ret=%d, err=0x%x)%s\n",
                  attr == EGL_WIDTH ? "EGL_WIDTH" : "EGL_HEIGHT",
                  val ? *val : -1, (int)r, eglGetError(),
                  (!r || !val || *val <= 0) ? "   *** BAD -> substituting real size ***" : "");
    }
    if (val && (!r || *val <= 0)) {   /* mesa gave us nothing usable */
      *val = want;
      r = EGL_TRUE;
    }
  }
  return r;
}
