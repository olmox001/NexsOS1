#ifndef _GRAPHICS_GL_H
#define _GRAPHICS_GL_H

#include <stddef.h>
#include <stdint.h>

/*
 * TinyGL: A simple software rasterization library.
 * Operates on 32-bit ARGB buffers.
 */

struct gl_surface {
  int width;
  int height;
  int stride; /* in pixels, usually width */
  uint32_t *buffer;
  uint8_t *alpha_mask;
};

/* Exact round-to-nearest divide-by-255 for x in [0, ~65790] (classic identity,
 * no integer division).  Replaces the ">>8" (÷256) approximation that dimmed
 * blended pixels — GFX-DYN-01 #121.4. */
static inline uint32_t gl_div255(uint32_t x) {
  x += 128;
  return (x + (x >> 8)) >> 8;
}

/* Alpha-blend a source ARGB pixel over a destination ARGB pixel (straight
 * over operator, opaque result).  Shared by the compositor and the terminal
 * emulator so both use one definition. */
static inline uint32_t gl_blend_pixel(uint32_t fg, uint32_t bg) {
  uint32_t alpha = (fg >> 24) & 0xFF;
  if (alpha == 255)
    return fg;
  if (alpha == 0)
    return bg;
  uint32_t inv_alpha = 255 - alpha;
  uint32_t r =
      gl_div255(((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * inv_alpha);
  uint32_t g =
      gl_div255(((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * inv_alpha);
  uint32_t b = gl_div255((fg & 0xFF) * alpha + (bg & 0xFF) * inv_alpha);
  return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* Primitives */
void gl_clear(struct gl_surface *surf, uint32_t color);
void gl_draw_pixel(struct gl_surface *surf, int x, int y, uint32_t color);
void gl_draw_line(struct gl_surface *surf, int x0, int y0, int x1, int y1,
                  uint32_t color);
void gl_draw_rect(struct gl_surface *surf, int x, int y, int w, int h,
                  uint32_t color);
void gl_draw_rect_fill(struct gl_surface *surf, int x, int y, int w, int h,
                       uint32_t color);
void gl_blit(struct gl_surface *dst, struct gl_surface *src, int dx, int dy);
void gl_draw_char(struct gl_surface *surf, int x, int y, uint32_t codepoint,
                  uint32_t color);
void gl_draw_string(struct gl_surface *surf, int x, int y, const char *str,
                    uint32_t color);
/* Clipped variant: identical to gl_draw_string but writes only pixels inside
 * [cx1,cx2) x [cy1,cy2).  Used by the damage-clipped compositor so title text
 * never paints outside the per-frame damage box. */
void gl_draw_string_clipped(struct gl_surface *surf, int x, int y,
                            const char *str, uint32_t color, int cx1, int cy1,
                            int cx2, int cy2);
void gl_swizzle_bgr(
    struct gl_surface *surf); /* Convert ABGR to ARGB if needed */

#endif
