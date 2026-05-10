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

void gl_clear(struct gl_surface *surf, uint32_t color) {
  if (!surf || !surf->buffer)
    return;
  int size = surf->width * surf->height;
  /* Optimized clear for 32-bit */
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
  /* Clipping */
  if (x >= surf->width || y >= surf->height)
    return;
  if (x + w < 0 || y + h < 0)
    return;

  int cx = (x < 0) ? 0 : x;
  int cy = (y < 0) ? 0 : y;

  /* Re-calc real width/height after clipping */
  cx = clip(x, 0, surf->width - 1);
  cy = clip(y, 0, surf->height - 1);
  int x2 = clip(x + w, 0, surf->width);
  int y2 = clip(y + h, 0, surf->height);

  for (int j = cy; j < y2; j++) {
    for (int i = cx; i < x2; i++) {
      surf->buffer[j * surf->stride + i] = color;
    }
  }
}

void gl_blit(struct gl_surface *dst, struct gl_surface *src, int dx, int dy) {
  if (!dst || !src)
    return;

  for (int y = 0; y < src->height; y++) {
    for (int x = 0; x < src->width; x++) {
      uint32_t col = src->buffer[y * src->stride + x];
      /* Simple Alpha Test (0 alpha = transparent/skip) */
      if ((col & 0xFF000000) == 0)
        continue;
      gl_draw_pixel(dst, dx + x, dy + y, col);
    }
  }
}
