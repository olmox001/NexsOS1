/*
 * kernel/graphics/draw2d.c
 * 2D Graphics Primitives
 */
#include <kernel/graphics.h>
#include <kernel/types.h>

/*
 * Draw Line (Bresenham's Algorithm)
 */
/*
 * Draw Line (Bresenham's Algorithm)
 */
void graphics_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                        uint32_t color) {
  /* Use GL implementation which handles clipping */
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) == 0) {
    struct gl_surface *surf = &screen;
    gl_draw_line(surf, (int)x0, (int)y0, (int)x1, (int)y1, color);
  }
}

/*
 * Draw Circle (Midpoint Algorithm)
 */
void graphics_draw_circle(int cx, int cy, int r, uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) != 0)
    return;
  struct gl_surface *surf = &screen;

  int x = r;
  int y = 0;
  int err = 0;

  while (x >= y) {
    gl_draw_pixel(surf, cx + x, cy + y, color);
    gl_draw_pixel(surf, cx - x, cy + y, color);
    gl_draw_pixel(surf, cx + x, cy - y, color);
    gl_draw_pixel(surf, cx - x, cy - y, color);
    gl_draw_pixel(surf, cx + y, cy + x, color);
    gl_draw_pixel(surf, cx - y, cy + x, color);
    gl_draw_pixel(surf, cx + y, cy - x, color);
    gl_draw_pixel(surf, cx - y, cy - x, color);

    y++;
    err += 1 + 2 * y;
    if (2 * (err - x) + 1 > 0) {
      x--;
      err += 1 - 2 * x;
    }
  }
}

/*
 * Fill Circle
 */
void graphics_fill_circle(int cx, int cy, int r, uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) != 0)
    return;
  struct gl_surface *surf = &screen;

  int x = r;
  int y = 0;
  int err = 0;

  while (x >= y) {
    /* Draw horizontal lines for each octant pair with clipping */
    for (int i = cx - x; i <= cx + x; i++) {
      gl_draw_pixel(surf, i, cy + y, color);
      gl_draw_pixel(surf, i, cy - y, color);
    }
    for (int i = cx - y; i <= cx + y; i++) {
      gl_draw_pixel(surf, i, cy + x, color);
      gl_draw_pixel(surf, i, cy - x, color);
    }

    y++;
    err += 1 + 2 * y;
    if (2 * (err - x) + 1 > 0) {
      x--;
      err += 1 - 2 * x;
    }
  }
}

/*
 * Draw Triangle Outline
 */
void graphics_draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                            uint32_t color) {
  graphics_draw_line(x0, y0, x1, y1, color);
  graphics_draw_line(x1, y1, x2, y2, color);
  graphics_draw_line(x2, y2, x0, y0, color);
}

/*
 * Helper: Swap integers
 */
static inline void swap_int(int *a, int *b) {
  int t = *a;
  *a = *b;
  *b = t;
}

/*
 * Fill Triangle (Scanline Algorithm)
 */
void graphics_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                            uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) != 0)
    return;
  struct gl_surface *surf = &screen;

  /* Sort by y coordinate */
  if (y0 > y1) {
    swap_int(&y0, &y1);
    swap_int(&x0, &x1);
  }
  if (y1 > y2) {
    swap_int(&y1, &y2);
    swap_int(&x1, &x2);
  }
  if (y0 > y1) {
    swap_int(&y0, &y1);
    swap_int(&x0, &x1);
  }

  int total_height = y2 - y0;
  if (total_height == 0)
    return;

  for (int y = y0; y <= y2; y++) {
    int second_half = (y > y1) || (y1 == y0);
    int segment_height = second_half ? (y2 - y1) : (y1 - y0);
    if (segment_height == 0)
      continue;

    int alpha = (y - y0);
    int beta = second_half ? (y - y1) : (y - y0);

    int xa = x0 + (x2 - x0) * alpha / total_height;
    int xb = second_half ? (x1 + (x2 - x1) * beta / segment_height)
                         : (x0 + (x1 - x0) * beta / segment_height);

    if (xa > xb)
      swap_int(&xa, &xb);

    for (int x = xa; x <= xb; x++) {
      gl_draw_pixel(surf, x, y, color);
    }
  }
}

/*
 * Draw Rounded Rectangle
 */
void graphics_draw_rounded_rect(int x, int y, int w, int h, int r,
                                uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) != 0)
    return;
  struct gl_surface *surf = &screen;

  /* Top and bottom lines */
  graphics_draw_line(x + r, y, x + w - r, y, color);
  graphics_draw_line(x + r, y + h, x + w - r, y + h, color);

  /* Left and right lines */
  graphics_draw_line(x, y + r, x, y + h - r, color);
  graphics_draw_line(x + w, y + r, x + w, y + h - r, color);

  /* Corners (quarter circles using midpoint) */
  int px = r, py = 0, err = 0;

  while (px >= py) {
    /* Top-left */
    gl_draw_pixel(surf, x + r - px, y + r - py, color);
    gl_draw_pixel(surf, x + r - py, y + r - px, color);
    /* Top-right */
    gl_draw_pixel(surf, x + w - r + px, y + r - py, color);
    gl_draw_pixel(surf, x + w - r + py, y + r - px, color);
    /* Bottom-left */
    gl_draw_pixel(surf, x + r - px, y + h - r + py, color);
    gl_draw_pixel(surf, x + r - py, y + h - r + px, color);
    /* Bottom-right */
    gl_draw_pixel(surf, x + w - r + px, y + h - r + py, color);
    gl_draw_pixel(surf, x + w - r + py, y + h - r + px, color);

    py++;
    err += 1 + 2 * py;
    if (2 * (err - px) + 1 > 0) {
      px--;
      err += 1 - 2 * px;
    }
  }
}

/*
 * Alpha Blend Two Colors (ARGB8888)
 */
uint32_t graphics_blend(uint32_t fg, uint32_t bg) {
  uint32_t alpha = (fg >> 24) & 0xFF;
  if (alpha == 255) return fg;
  if (alpha == 0) return bg;

  uint32_t inv_alpha = 255 - alpha;
  uint32_t r = ((((fg >> 16) & 0xFF) * alpha) + (((bg >> 16) & 0xFF) * inv_alpha)) / 255;
  uint32_t g = ((((fg >> 8) & 0xFF) * alpha) + (((bg >> 8) & 0xFF) * inv_alpha)) / 255;
  uint32_t b = (((fg & 0xFF) * alpha) + ((bg & 0xFF) * inv_alpha)) / 255;

  return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/*
 * Draw Gradient Rectangle (Horizontal)
 */
void graphics_draw_gradient_h(int x, int y, int w, int h, uint32_t color_left,
                              uint32_t color_right) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) != 0)
    return;
  struct gl_surface *surf = &screen;

  for (int col = 0; col < w; col++) {
    uint32_t t = (col * 255) / (w > 1 ? w - 1 : 1);
    uint32_t inv_t = 255 - t;

    uint32_t r = ((((color_left >> 16) & 0xFF) * inv_t) + (((color_right >> 16) & 0xFF) * t)) / 255;
    uint32_t g = ((((color_left >> 8) & 0xFF) * inv_t) + (((color_right >> 8) & 0xFF) * t)) / 255;
    uint32_t b = (((color_left & 0xFF) * inv_t) + ((color_right & 0xFF) * t)) / 255;

    uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;

    for (int row = 0; row < h; row++) {
      gl_draw_pixel(surf, x + col, y + row, color);
    }
  }
}
