/*
 * OS1 OpenGL presentation seam.
 *
 * The surface an OSMesa-style software GL renders into: a CPU ARGB8888
 * top-down colour buffer bound to an OS1 window, swapped through the
 * os1_video_platform portability header.  The GL personality (Mesa's OSMesa
 * frontend on the nexsos-port-opengl branch, pinned at the last release
 * that ships it) binds this buffer with OSMesaMakeCurrent + OSMESA_Y_UP=0;
 * nothing here depends on Mesa headers, so the seam builds and ships ahead
 * of the library.  The backend field mirrors os1_video_backend: the CPU
 * buffer is the intentional swap-in point for the future ASTRA GPU-context
 * object, exactly like the D3D9 presentation chain.
 */
#ifndef OS1_GL_PLATFORM_H
#define OS1_GL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

struct os1_gl_surface_params {
  int x;
  int y;
  int width;
  int height;
  const char *title;
};

struct os1_gl_surface; /* opaque: OS1 window + CPU colour buffer */

/* Create/destroy a presentable GL surface on the platform's active backend.
 * Returns 0, -ENOSYS when only an unsupported backend is advertised, or a
 * negative OS1 error. */
int os1_gl_surface_create(const struct os1_gl_surface_params *params,
                          struct os1_gl_surface **out);
void os1_gl_surface_destroy(struct os1_gl_surface *surface);

/* The colour buffer a software GL binds (ARGB8888, top-down rows, tight
 * pitch).  Geometry is reported through the out parameters; the pointer
 * stays valid until destroy or resize. */
uint32_t *os1_gl_surface_buffer(struct os1_gl_surface *surface, int *width,
                                int *height, size_t *pitch_bytes);

/* SwapBuffers analogue: present the whole colour buffer and notify the
 * compositor. */
int os1_gl_surface_swap(struct os1_gl_surface *surface);

/* Adopt a new size (nxlauncher resize contract): kernel logical resize +
 * buffer reallocation.  The previously returned buffer pointer is invalid
 * after a successful resize; re-fetch it with os1_gl_surface_buffer. */
int os1_gl_surface_resize(struct os1_gl_surface *surface, int width,
                          int height);

/* The enum os1_video_backend this surface presents on. */
int os1_gl_surface_backend(const struct os1_gl_surface *surface);

#endif /* OS1_GL_PLATFORM_H */
