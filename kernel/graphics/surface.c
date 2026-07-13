/* Kernel-internal surface contract implementation. */
#include <kernel/gfx_surface.h>
#include <kernel/printk.h>

int gfx_surface_valid(const gfx_surface_t *surface) {
  return surface && surface->buffer && surface->width > 0 &&
         surface->height > 0 && surface->stride >= surface->width;
}

/* S-STAB invariant gate — see gl.h.  A surface whose geometry outruns its
 * backing allocation is a memory-corruption bug the instant anything draws to
 * it; fail loud and precise here rather than let an OOB write corrupt kernel
 * RAM and surface as an unrelated #PF/#GP later (the UTM panic signature). */
void gfx_surface_verify(const gfx_surface_t *surf, const char *where) {
  if (!surf)
    panic("gfx_surface_verify(%s): NULL surface", where ? where : "?");
  if (surf->buffer && surf->stride < surf->width)
    panic("gfx_surface_verify(%s): stride %d < width %d (buf=%p)",
          where ? where : "?", surf->stride, surf->width, (void *)surf->buffer);
  /* capacity==0 means the bind site is not yet instrumented — do not treat the
   * absence of a known allocation size as a violation. */
  if (surf->capacity == 0)
    return;
  size_t need = (size_t)(surf->height > 0 ? surf->height : 0) *
                (size_t)(surf->stride > 0 ? surf->stride : 0);
  if (need > surf->capacity)
    panic("gfx_surface_verify(%s): geometry %dx%d stride %d needs %lu px but "
          "buffer holds only %lu (buf=%p) — geometry/allocation desync",
          where ? where : "?", surf->width, surf->height, surf->stride,
          (unsigned long)need, (unsigned long)surf->capacity,
          (void *)surf->buffer);
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
