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
};

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
void gl_draw_char(struct gl_surface *surf, int x, int y, char c,
                  uint32_t color);
void gl_draw_string(struct gl_surface *surf, int x, int y, const char *str,
                    uint32_t color);
void gl_swizzle_bgr(
    struct gl_surface *surf); /* Convert ABGR to ARGB if needed */

#endif
