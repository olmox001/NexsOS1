/*
 * Kernel-internal surface contract.
 *
 * This facade defines the semantic unit shared by compositor chrome, the
 * software presentation path and future GPU providers. It is deliberately
 * independent of OpenGL/D3D names: those APIs remain userland personalities.
 */
#ifndef KERNEL_GFX_SURFACE_H
#define KERNEL_GFX_SURFACE_H

#include <graphics/gl.h>
#include <stdint.h>

typedef struct gl_surface gfx_surface_t;

typedef struct gfx_rect {
  int x;
  int y;
  int width;
  int height;
} gfx_rect_t;

enum gfx_surface_format { GFX_SURFACE_ARGB8888 = 1 };

int gfx_surface_valid(const gfx_surface_t *surface);
void gfx_surface_clear(gfx_surface_t *surface, uint32_t argb);
void gfx_surface_fill(gfx_surface_t *surface, const gfx_rect_t *rect,
                      uint32_t argb);
void gfx_surface_composite_over(gfx_surface_t *destination,
                                gfx_surface_t *source, int x, int y);

#endif /* KERNEL_GFX_SURFACE_H */
