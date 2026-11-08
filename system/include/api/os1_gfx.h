/*
 * NexsOS userland software graphics primitives.
 *
 * The surface is a caller-owned linear ARGB8888 buffer.  Raster operations
 * never call into the kernel; presentation and buffer lifetime are provided
 * separately by OS1's window/surface control ABI.
 */
#ifndef OS1_GFX_H
#define OS1_GFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct os1_gfx_surface {
  uint32_t *pixels;
  size_t width;
  size_t height;
  size_t stride;   /* pixels per row */
  size_t capacity; /* total allocated pixels */
};

struct os1_gfx_rect {
  int x;
  int y;
  int width;
  int height;
};

int os1_gfx_surface_valid(const struct os1_gfx_surface *surface);
void os1_gfx_clear(struct os1_gfx_surface *surface, uint32_t argb);
void os1_gfx_fill_rect(struct os1_gfx_surface *surface,
                       const struct os1_gfx_rect *rect, uint32_t argb);
void os1_gfx_composite_over(struct os1_gfx_surface *destination,
                            const struct os1_gfx_surface *source, int x,
                            int y);
void os1_gfx_composite_scaled(struct os1_gfx_surface *destination,
                              const struct os1_gfx_surface *source,
                              int x, int y, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* OS1_GFX_H */
