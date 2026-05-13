#include <graphics/gl.h>
#include <kernel/string.h>
#include <kernel/types.h>

/* Helper for clipping */
static inline int clip(int val, int min, int max) {
  if (val < min)
    return min;
  if (val > max)
    return max;
  return val;
}

/*
 * Alpha blend: composite src (ARGB8888) over opaque dst.
 * Standard Porter-Duff "src over" for pre-multiplied-free ARGB:
 *   out = (src * a + dst * (255 - a)) / 255
 * Inline so the compiler can hoist it into the blit hot loop.
 */
static inline uint32_t blend_over(uint32_t src, uint32_t dst) {
  uint32_t a = (src >> 24) & 0xFF;
  uint32_t ia = 255 - a;

  uint32_t r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
  uint32_t g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
  uint32_t b = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;

  return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void gl_clear(struct gl_surface *surf, uint32_t color) {
  if (!surf || !surf->buffer)
    return;

  /*
   * BUG FIX (minor): original used width*height, ignoring stride.
   * If stride > width (e.g. pitch-aligned framebuffer) the padding columns
   * between rows were never cleared, leaving stale data visible.
   * Use stride*height to cover the full allocation.
   */
  int size = surf->stride * surf->height;
  for (int i = 0; i < size; i++)
    surf->buffer[i] = color;
}

void gl_draw_pixel(struct gl_surface *surf, int x, int y, uint32_t color) {
  if (!surf || !surf->buffer)
    return;
  if (x < 0 || x >= surf->width || y < 0 || y >= surf->height)
    return;
  surf->buffer[y * surf->stride + x] = color;
}

void gl_draw_line(struct gl_surface *surf, int x0, int y0, int x1, int y1,
                  uint32_t color) {
  if (!surf)
    return;
  int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = (dx > dy ? dx : -dy) / 2;
  int e2;

  for (;;) {
    gl_draw_pixel(surf, x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    e2 = err;
    if (e2 > -dx) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dy) {
      err += dx;
      y0 += sy;
    }
  }
}

void gl_draw_rect_fill(struct gl_surface *surf, int x, int y, int w, int h,
                       uint32_t color) {
  if (!surf)
    return;
  if (x >= surf->width || y >= surf->height)
    return;
  if (x + w < 0 || y + h < 0)
    return;

  int cx = clip(x, 0, surf->width - 1);
  int cy = clip(y, 0, surf->height - 1);
  int x2 = clip(x + w, 0, surf->width);
  int y2 = clip(y + h, 0, surf->height);

  for (int j = cy; j < y2; j++)
    for (int i = cx; i < x2; i++)
      surf->buffer[j * surf->stride + i] = color;
}

/*
 * gl_blit — composite src surface onto dst at (dx, dy).
 *
 * BUG FIX (critical): The original implementation performed only an alpha
 * TEST: any pixel with alpha == 0 was skipped, and every other pixel —
 * regardless of its actual alpha value — was written at full opacity with
 * gl_draw_pixel().  Semi-transparent pixels (0 < alpha < 255) were therefore
 * composited as if they were completely opaque, corrupting the destination
 * with raw source colours instead of a proper blend.  This is the root cause
 * of the multicoloured artefact visible in the compositor output.
 *
 * Fix: three-way path keyed on the source alpha:
 *   alpha == 0   → skip (fully transparent, nothing to draw)
 *   alpha == 255 → fast copy (fully opaque, no arithmetic needed)
 *   otherwise    → Porter-Duff "src over" blend with the destination pixel
 */
void gl_blit(struct gl_surface *dst, struct gl_surface *src, int dx, int dy) {
  if (!dst || !src || !dst->buffer || !src->buffer)
    return;

  for (int y = 0; y < src->height; y++) {
    int dsty = dy + y;
    if (dsty < 0 || dsty >= dst->height)
      continue;

    for (int x = 0; x < src->width; x++) {
      int dstx = dx + x;
      if (dstx < 0 || dstx >= dst->width)
        continue;

      uint32_t col = src->buffer[y * src->stride + x];
      uint32_t alpha = col >> 24;

      if (alpha == 0)
        continue; /* fully transparent — nothing to do */

      uint32_t *dp = &dst->buffer[dsty * dst->stride + dstx];

      if (alpha == 255) {
        *dp = col; /* fully opaque — fast path, no blend math */
      } else {
        *dp = blend_over(col, *dp); /* semi-transparent — proper alpha blend */
      }
    }
  }
}