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

/* ===================== Input ============================================== */
static PadState s_pad;
static int      s_pad_ready = 0;

/* handheld multitouch tracking (by slot index) */
static int   s_prev_active[10];
static float s_prev_x[10], s_prev_y[10];

/* Virtual cursor -- available in BOTH handheld and docked.
 *   '+'  shows it
 *   '-'  hides it
 *   'A'  taps at the cursor
 * In handheld the touchscreen stays live at the same time, so you can use
 * either (or both). Docked has no touchscreen, so the cursor starts visible
 * there; in handheld it starts hidden since touch is the natural input. */
static float s_cur_x, s_cur_y;
static int   s_cur_down_prev;
static int   s_cursor_visible = -1;   /* -1 = not yet initialised */
static int   s_was_docked = -1;

/* Pointer ids: keep the cursor out of the touchscreen's id range so the two
 * never collide when both are active in handheld. */
#define CURSOR_PTR_ID   8
#define MAX_TOUCH_SLOTS 8

static void ensure_pad(void) {
  if (s_pad_ready) return;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&s_pad);
  hidInitializeTouchScreen();
  s_cur_x = screen_width  * 0.5f;
  s_cur_y = screen_height * 0.5f;
  s_pad_ready = 1;
}

static int is_docked(void) {
  return appletGetOperationMode() == AppletOperationMode_Console;
}

/* Collected once per frame by padUpdate_all(), drained by platform_poll_pointers. */
static PtrEvent s_events[16];
static int      s_nevents;

/* The Switch touch panel always reports in its native 1280x720 space, no matter
 * what resolution we render at. The engine works in render space (1920x1080), so
 * raw panel coordinates only ever reach the top-left 2/3 of the screen -- the
 * right edge and bottom edge (and hence the corners) are physically untouchable.
 * Scale panel -> render space. */
#define NX_TOUCH_PANEL_W 1280.0f
#define NX_TOUCH_PANEL_H 720.0f

/* Appends to s_events (does NOT reset it) so touch and cursor can coexist. */
static void collect_touch_events(void) {
  HidTouchScreenState ts = {0};
  hidGetTouchScreenStates(&ts, 1);
  int now[10] = {0};
  int count = ts.count > MAX_TOUCH_SLOTS ? MAX_TOUCH_SLOTS : ts.count;

  const float sx = (float)screen_width  / NX_TOUCH_PANEL_W;   /* 1920/1280 = 1.5 */
  const float sy = (float)screen_height / NX_TOUCH_PANEL_H;   /* 1080/720  = 1.5 */

  for (int i = 0; i < count; i++) {
    float x = (float)ts.touches[i].x * sx;
    float y = (float)ts.touches[i].y * sy;
    /* clamp so an edge touch maps exactly onto the last pixel */
    if (x < 0) x = 0; if (x > screen_width  - 1) x = (float)(screen_width  - 1);
    if (y < 0) y = 0; if (y > screen_height - 1) y = (float)(screen_height - 1);
    now[i] = 1;
    if (s_nevents < 16) {
      PtrEvent *e = &s_events[s_nevents++];
      e->id = i; e->x = x; e->y = y;
      e->phase = s_prev_active[i] ? PTR_MOVE : PTR_DOWN;
    }
    s_prev_x[i] = x; s_prev_y[i] = y;
  }
  for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
    if (s_prev_active[i] && !now[i] && s_nevents < 16) {
      PtrEvent *e = &s_events[s_nevents++];
      e->id = i; e->x = s_prev_x[i]; e->y = s_prev_y[i]; e->phase = PTR_UP;
    }
    s_prev_active[i] = now[i];
  }
}

/* Appends to s_events. Left stick moves the cursor, A is the tap. */
static void collect_cursor_events(void) {
  HidAnalogStickState ls = padGetStickPos(&s_pad, 0);
  const float SPEED = 14.0f;           /* px per frame at full deflection */
  s_cur_x += (ls.x / 32767.0f) * SPEED;
  s_cur_y -= (ls.y / 32767.0f) * SPEED;
  /* clamp to the last valid pixel, not one past it (screen_width would be an
   * off-screen column and the engine's hit-testing would miss the edge) */
  if (s_cur_x < 0) s_cur_x = 0;
  if (s_cur_x > screen_width  - 1) s_cur_x = (float)(screen_width  - 1);
  if (s_cur_y < 0) s_cur_y = 0;
  if (s_cur_y > screen_height - 1) s_cur_y = (float)(screen_height - 1);

  int down = (padGetButtons(&s_pad) & HidNpadButton_A) ? 1 : 0;
  int phase = 0;
  if (down && !s_cur_down_prev)      phase = PTR_DOWN;
  else if (down && s_cur_down_prev)  phase = PTR_MOVE;
  else if (!down && s_cur_down_prev) phase = PTR_UP;
  s_cur_down_prev = down;
  if (phase && s_nevents < 16) {
    PtrEvent *e = &s_events[s_nevents++];
    e->id = CURSOR_PTR_ID; e->x = s_cur_x; e->y = s_cur_y; e->phase = phase;
  }
}

void padUpdate_all(void) {
  ensure_pad();
  padUpdate(&s_pad);

  const int docked = is_docked();

  /* First run, and whenever we dock/undock: docked has no touchscreen, so make
   * sure the cursor is up there; handheld defaults to touch. */
  if (docked != s_was_docked) {
    if (s_cursor_visible < 0 || docked)
      s_cursor_visible = docked ? 1 : 0;
    s_was_docked = docked;
  }

  /* '+' shows the cursor, '-' hides it -- in both modes. */
  const u64 pressed = padGetButtonsDown(&s_pad);
  if (pressed & HidNpadButton_Plus)  s_cursor_visible = 1;
  if (pressed & HidNpadButton_Minus) s_cursor_visible = 0;

  s_nevents = 0;
  if (!docked)          collect_touch_events();   /* touchscreen: handheld only */
  if (s_cursor_visible) collect_cursor_events();  /* cursor: either mode */
}

/* Queried by the renderer (eglSwapBuffers_fake) to draw the cursor. */
int  cursor_is_visible(void) { return s_cursor_visible > 0; }
void cursor_get_pos(float *x, float *y) { if (x) *x = s_cur_x; if (y) *y = s_cur_y; }

int platform_poll_pointers(PtrEvent *out, int max) {
  int n = s_nevents < max ? s_nevents : max;
  memcpy(out, s_events, n * sizeof(PtrEvent));
  return n;
}

/* B is the Android BACK key. Plus used to be wired here too, but it now shows
 * the cursor -- leaving it as BACK would fire a back-press every time you
 * brought the cursor up. */
int back_edge_pressed(void) {
  return (padGetButtonsDown(&s_pad) & HidNpadButton_B) ? 1 : 0;
}
int back_edge_released(void) {
  return (padGetButtonsUp(&s_pad) & HidNpadButton_B) ? 1 : 0;
}

int should_quit(void) {
  /* HOME suspends/exits via the applet; also allow a deliberate combo. */
  u64 h = padGetButtons(&s_pad);
  return (h & HidNpadButton_Minus) && (h & HidNpadButton_StickL) &&
         (h & HidNpadButton_StickR);
}

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

/* ===================== On-screen cursor ==================================
 * The engine owns the GL context and presents via eglSwapBuffers, so we draw
 * the cursor from inside eglSwapBuffers_fake -- after the engine has rendered
 * its frame, just before it goes to the panel. That means it always sits on top.
 *
 * We use our own tiny shader + client-side vertex array, and save/restore every
 * piece of GL state we touch, so the engine's next frame is unaffected.
 * ======================================================================== */

static GLuint s_cur_prog = 0;
static GLint  s_loc_pos, s_loc_screen, s_loc_origin, s_loc_scale, s_loc_colour;
static int    s_cur_gl_failed = 0;

/* A classic arrow, in local units with the tip at (0,0), y down. Drawn as a
 * triangle fan from the tip (the shape is star-shaped about the tip). */
static const GLfloat s_arrow[] = {
   0.0f,  0.0f,
   0.0f, 16.0f,
   4.0f, 12.0f,
   7.0f, 18.0f,
  10.0f, 16.5f,
   7.0f, 10.5f,
  12.0f, 10.0f,
};
#define ARROW_VERTS (sizeof(s_arrow) / (2 * sizeof(GLfloat)))

static GLuint cursor_compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

static int cursor_init_gl(void) {
  if (s_cur_prog) return 1;
  if (s_cur_gl_failed) return 0;

  static const char *vs =
    "attribute vec2 aPos;\n"
    "uniform vec2 uScreen;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uScale;\n"
    "void main() {\n"
    "  vec2 p = uOrigin + aPos * uScale;\n"
    "  vec2 ndc = vec2((p.x / uScreen.x) * 2.0 - 1.0,\n"
    "                  1.0 - (p.y / uScreen.y) * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";
  static const char *fs =
    "precision mediump float;\n"
    "uniform vec4 uColour;\n"
    "void main() { gl_FragColor = uColour; }\n";

  GLuint v = cursor_compile(GL_VERTEX_SHADER, vs);
  GLuint f = cursor_compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) { s_cur_gl_failed = 1; debugPrintf("cursor: shader compile failed\n"); return 0; }

  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);

  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(p); s_cur_gl_failed = 1; debugPrintf("cursor: link failed\n"); return 0; }

  s_cur_prog   = p;
  s_loc_pos    = 0;
  s_loc_screen = glGetUniformLocation(p, "uScreen");
  s_loc_origin = glGetUniformLocation(p, "uOrigin");
  s_loc_scale  = glGetUniformLocation(p, "uScale");
  s_loc_colour = glGetUniformLocation(p, "uColour");
  debugPrintf("cursor: gl ready (prog=%u)\n", p);
  return 1;
}

void cursor_draw(void) {
  if (!cursor_is_visible()) return;
  if (!cursor_init_gl())    return;

  float cx, cy;
  cursor_get_pos(&cx, &cy);

  /* --- save every bit of state we are about to change --- */
  GLint  prev_prog = 0, prev_buf = 0;
  GLint  bs_rgb = 0, bd_rgb = 0, bs_a = 0, bd_a = 0;
  GLint  attr0_on = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
  glGetIntegerv(GL_BLEND_SRC_RGB, &bs_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bd_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bs_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bd_a);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr0_on);
  const GLboolean was_blend   = glIsEnabled(GL_BLEND);
  const GLboolean was_depth   = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean was_cull    = glIsEnabled(GL_CULL_FACE);
  const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

  /* --- draw --- */
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);          /* the engine may have clipped to a sub-rect */
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(s_cur_prog);
  glBindBuffer(GL_ARRAY_BUFFER, 0);    /* client-side array (legal in GLES2) */
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, s_arrow);

  glUniform2f(s_loc_screen, (GLfloat)screen_width, (GLfloat)screen_height);
  glUniform2f(s_loc_origin, cx, cy);

  const GLfloat scale = 2.4f;          /* ~40px tall on a 1080p screen */

  /* black outline first (same shape, scaled up from the tip), then white fill */
  glUniform1f(s_loc_scale, scale * 1.22f);
  glUniform4f(s_loc_colour, 0.0f, 0.0f, 0.0f, 0.85f);
  glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)ARROW_VERTS);

  glUniform1f(s_loc_scale, scale);
  glUniform4f(s_loc_colour, 1.0f, 1.0f, 1.0f, 1.0f);
  glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)ARROW_VERTS);

  /* --- restore --- */
  if (!attr0_on) glDisableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
  glUseProgram((GLuint)prev_prog);
  glBlendFuncSeparate((GLenum)bs_rgb, (GLenum)bd_rgb, (GLenum)bs_a, (GLenum)bd_a);
  if (!was_blend)  glDisable(GL_BLEND);      else glEnable(GL_BLEND);
  if (was_depth)   glEnable(GL_DEPTH_TEST);
  if (was_cull)    glEnable(GL_CULL_FACE);
  if (was_scissor) glEnable(GL_SCISSOR_TEST);
}
