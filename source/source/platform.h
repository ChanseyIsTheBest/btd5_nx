/* platform.h -- Switch platform layer for the BTD5 port: heap split, GLES2/EGL
 * context, and input (handheld multitouch + docked virtual cursor), unified so
 * main.c stays a clean lifecycle driver.
 * MIT license -- see LICENSE. */
#ifndef __PLATFORM_H__
#define __PLATFORM_H__

#include <stddef.h>

/* ---- SO load zone (passed to so_load as the RW image buffer) ------------- */
void  *heap_so_base(void);      /* aligned buffer of heap_so_limit() bytes    */
size_t heap_so_limit(void);     /* == SO_ZONE_MB (config.h)                   */

/* ---- GLES2 / EGL --------------------------------------------------------- */
void egl_init_context(void);    /* create context+surface at current res     */
void egl_swap_buffers(void);
void egl_exit_context(void);

/* ---- per-frame input ----------------------------------------------------- */
void padUpdate_all(void);       /* padUpdate + sample touch/cursor once/frame */
int  handle_dock_change(int *w, int *h);  /* 1 if dock state (res) changed    */

/* Unified pointer events for this frame. Handheld = touchscreen fingers;
 * docked = a single stick-driven cursor with A as the "finger". */
enum { PTR_DOWN = 1, PTR_MOVE = 2, PTR_UP = 3 };
typedef struct { int id; float x, y; int phase; } PtrEvent;
int  platform_poll_pointers(PtrEvent *out, int max);


/* EGL tracing wrappers (see platform.c): the engine owns EGL; these log each
 * step + its error code so a silent failure (NO_SURFACE / failed MakeCurrent)
 * shows up in the log instead of just a black screen. */
#include <EGL/egl.h>
EGLDisplay eglGetDisplay_fake(EGLNativeDisplayType dpy);
EGLBoolean eglInitialize_fake(EGLDisplay d, EGLint *maj, EGLint *min);
EGLBoolean eglChooseConfig_fake(EGLDisplay d, const EGLint *attrib, EGLConfig *cfgs, EGLint n, EGLint *num);
EGLContext eglCreateContext_fake(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attrib);
EGLSurface eglCreateWindowSurface_fake(EGLDisplay d, EGLConfig c, EGLNativeWindowType win, const EGLint *attrib);
EGLBoolean eglMakeCurrent_fake(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean eglSwapBuffers_fake(EGLDisplay d, EGLSurface s);


/* GL frame tracing (platform.c): shows viewport, clear colour, draw-call count
 * and the bound FBO, so a black screen with a working EGL surface can be traced
 * to "no draws" / "degenerate viewport" / "rendering into an offscreen FBO". */
#include <GLES2/gl2.h>
void glViewport_fake(GLint x, GLint y, GLsizei w, GLsizei h);
void glClearColor_fake(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClear_fake(GLbitfield mask);
void glBindFramebuffer_fake(GLenum target, GLuint fb);
void glDrawArrays_fake(GLenum mode, GLint first, GLsizei count);
void glDrawElements_fake(GLenum mode, GLsizei count, GLenum type, const void *idx);

EGLBoolean eglQuerySurface_fake(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val);

/* Cursor overlay, drawn from eglSwapBuffers_fake. Implemented by the reusable
 * nx_pointer module -- see nx_pointer.h for the full control scheme. */
void cursor_draw(void);

#endif
