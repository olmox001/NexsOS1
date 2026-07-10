/* Kernel-internal surface contract implementation. */
#include <kernel/gfx_surface.h>

int gfx_surface_valid(const gfx_surface_t *surface) {
  return surface && surface->buffer && surface->width > 0 &&
         surface->height > 0 && surface->stride >= surface->width;
}

void gfx_surface_clear(gfx_surface_t *surface, uint32_t argb) {
  if (gfx_surface_valid(surface))
    gl_clear(surface, argb);
}

void gfx_surface_fill(gfx_surface_t *surface, const gfx_rect_t *rect,
                      uint32_t argb) {
  if (gfx_surface_valid(surface) && rect && rect->width > 0 &&
      rect->height > 0)
    gl_draw_rect_fill(surface, rect->x, rect->y, rect->width, rect->height,
                      argb);
}

void gfx_surface_composite_over(gfx_surface_t *destination,
                                gfx_surface_t *source, int x, int y) {
  if (gfx_surface_valid(destination) && gfx_surface_valid(source))
    gl_blit(destination, source, x, y);
}
