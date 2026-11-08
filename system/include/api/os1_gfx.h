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

/* Window chrome primitives shared by the userland renderer and input
 * service. Geometry is intentionally identical to the legacy kernel chrome
 * until its remaining rendering helpers have been migrated. */
struct os1_gfx_button_geometry {
  int size;
  int top;
  int close_cx;
  int bg_cx;
};

enum os1_gfx_button_hit {
  OS1_GFX_BUTTON_NONE = 0,
  OS1_GFX_BUTTON_CLOSE = 1,
  OS1_GFX_BUTTON_BACKGROUND = 2,
};

int os1_gfx_rrect_contains(int x, int y, int width, int height, int radius);
void os1_gfx_button_geometry(int frame_x, int frame_width, int titlebar_y,
                             int titlebar_height, int shape, int side,
                             struct os1_gfx_button_geometry *out);
int os1_gfx_button_hit(const struct os1_gfx_button_geometry *geometry,
                       int x, int y);
uint32_t os1_gfx_titlebar_tint(int shadow_type, int y, int titlebar_height,
                               uint32_t base);
uint32_t os1_gfx_button_pixel(const struct os1_gfx_button_geometry *geometry,
                             int shape, int x, int y, uint32_t close_argb,
                             uint32_t background_argb, int drop_shadow,
                             uint32_t base);

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
