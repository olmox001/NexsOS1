/* Userland window chrome primitives ported from kernel/graphics/chrome.c. */
#include <os1_gfx.h>
#include <limits.h>

#define CHROME_BUTTON_SIZE 16
#define CHROME_BUTTON_GAP 6

static uint32_t blend(uint32_t src, uint32_t dst) {
  uint32_t alpha = src >> 24;
  if (alpha == 255) return src;
  if (alpha == 0) return dst;
  uint32_t inverse = 255 - alpha;
  uint32_t red = (((src >> 16) & 255) * alpha +
                  ((dst >> 16) & 255) * inverse) / 255;
  uint32_t green = (((src >> 8) & 255) * alpha +
                    ((dst >> 8) & 255) * inverse) / 255;
  uint32_t blue = ((src & 255) * alpha + (dst & 255) * inverse) / 255;
  return 0xff000000u | (red << 16) | (green << 8) | blue;
}

static int int64_fits_int(int64_t value) {
  return value >= INT_MIN && value <= INT_MAX;
}

static int circle_contains(int64_t dx, int64_t dy, int radius,
                           int extra_radius_squared) {
  if (radius < 0 || dx < -radius - 1 || dx > radius + 1 ||
      dy < -radius - 1 || dy > radius + 1)
    return 0;
  int64_t distance_squared = dx * dx + dy * dy;
  return distance_squared <= (int64_t)radius * radius + extra_radius_squared;
}

uint32_t os1_gfx_titlebar_tint(int shadow_type, int y, int titlebar_height,
                               uint32_t base) {
  if (titlebar_height <= 0) return base;
  if (shadow_type == 1) {
    if (y == 0) base = blend(0x20ffffffu, base);
  } else if (shadow_type == 2) {
    if (y == 0) base = blend(0x40ffffffu, base);
    int64_t gradient = ((int64_t)y * 24) / titlebar_height;
    if (gradient > 0) {
      if (gradient > 255) gradient = 255;
      base = blend(((uint32_t)gradient << 24), base);
    }
  }
  return base;
}

int os1_gfx_rrect_contains(int x, int y, int width, int height, int radius) {
  if (radius <= 0) return 1;
  if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height)
    return 0;
  int cx, cy;
  if (x < radius && y < radius) {
    cx = radius - 1 - x; cy = radius - 1 - y;
  } else if (x >= width - radius && y < radius) {
    cx = x - (width - radius); cy = radius - 1 - y;
  } else if (x < radius && y >= height - radius) {
    cx = radius - 1 - x; cy = y - (height - radius);
  } else if (x >= width - radius && y >= height - radius) {
    cx = x - (width - radius); cy = y - (height - radius);
  } else {
    return 1;
  }
  return (int64_t)cx * cx + (int64_t)cy * cy <= (int64_t)radius * radius;
}

void os1_gfx_button_geometry(int frame_x, int frame_width, int titlebar_y,
                             int titlebar_height, int shape, int side,
                             struct os1_gfx_button_geometry *out) {
  if (!out) return;
  out->size = 0;
  out->top = 0;
  out->close_cx = 0;
  out->bg_cx = 0;
  if (frame_width <= 0 || titlebar_height < 0 || shape < 0 || shape > 2 ||
      side < 0 || side > 1)
    return;
  int size = CHROME_BUTTON_SIZE;
  if (shape == 1 || shape == 2) size -= 2;
  int64_t top = (int64_t)titlebar_y + (titlebar_height - size) / 2;
  int64_t close_cx, bg_cx;
  if (side == 0) {
    int64_t left = (int64_t)frame_x + 4;
    close_cx = left + size / 2;
    bg_cx = close_cx + size + CHROME_BUTTON_GAP;
  } else {
    int64_t right = (int64_t)frame_x + frame_width - 4;
    close_cx = right - size / 2;
    bg_cx = close_cx - size - CHROME_BUTTON_GAP;
  }
  if (!int64_fits_int(top) || !int64_fits_int(close_cx) ||
      !int64_fits_int(bg_cx))
    return;
  out->size = size;
  out->top = (int)top;
  out->close_cx = (int)close_cx;
  out->bg_cx = (int)bg_cx;
}

int os1_gfx_button_hit(const struct os1_gfx_button_geometry *g, int x, int y) {
  if (!g || g->size <= 0 || g->size > CHROME_BUTTON_SIZE ||
      (int64_t)y < g->top || (int64_t)y >= (int64_t)g->top + g->size)
    return OS1_GFX_BUTTON_NONE;
  int64_t close_x = (int64_t)g->close_cx - g->size / 2;
  int64_t bg_x = (int64_t)g->bg_cx - g->size / 2;
  if ((int64_t)x >= close_x && (int64_t)x < close_x + g->size)
    return OS1_GFX_BUTTON_CLOSE;
  if ((int64_t)x >= bg_x && (int64_t)x < bg_x + g->size)
    return OS1_GFX_BUTTON_BACKGROUND;
  return OS1_GFX_BUTTON_NONE;
}

uint32_t os1_gfx_button_pixel(const struct os1_gfx_button_geometry *g,
                             int shape, int x, int y, uint32_t close_argb,
                             uint32_t background_argb, int drop_shadow,
                             uint32_t base) {
  if (!g || g->size <= 0 || g->size > CHROME_BUTTON_SIZE || shape < 0 ||
      shape > 2) return base;
  int size = g->size;
  int64_t local_y = (int64_t)y - g->top;
  int64_t local_close = (int64_t)x - g->close_cx + size / 2;
  int64_t local_bg = (int64_t)x - g->bg_cx + size / 2;
  if (shape == 1) {
    if (local_close >= 0 && local_close < size && local_y >= 0 && local_y < size)
      return close_argb;
    if (local_bg >= 0 && local_bg < size && local_y >= 0 && local_y < size)
      return background_argb;
    if (drop_shadow && local_y == size) {
      if (local_close >= 0 && local_close < size) return blend(0x60000000, base);
      if (local_bg >= 0 && local_bg < size) return blend(0x60000000, base);
    }
    return base;
  }
  if (shape == 2) {
    if (local_close >= INT_MIN && local_close <= INT_MAX &&
        local_y >= INT_MIN && local_y <= INT_MAX &&
        os1_gfx_rrect_contains((int)local_close, (int)local_y, size, size, 6))
      return close_argb;
    if (local_bg >= INT_MIN && local_bg <= INT_MAX &&
        local_y >= INT_MIN && local_y <= INT_MAX &&
        os1_gfx_rrect_contains((int)local_bg, (int)local_y, size, size, 6))
      return background_argb;
    if (drop_shadow) {
      if (local_y > INT_MIN && local_y <= INT_MAX &&
          ((local_close >= INT_MIN && local_close <= INT_MAX &&
            os1_gfx_rrect_contains((int)local_close, (int)local_y - 1,
                                   size, size, 6)) ||
           (local_bg >= INT_MIN && local_bg <= INT_MAX &&
            os1_gfx_rrect_contains((int)local_bg, (int)local_y - 1,
                                   size, size, 6))))
        return blend(0x50000000, base);
    }
    return base;
  }
  int radius = size / 2 - 1;
  int64_t dy = (int64_t)y - ((int64_t)g->top + size / 2);
  int64_t dx_close = (int64_t)x - g->close_cx;
  int64_t dx_bg = (int64_t)x - g->bg_cx;
  if (circle_contains(dx_close, dy, radius, 0))
    return close_argb;
  if (circle_contains(dx_bg, dy, radius, 0))
    return background_argb;
  if (drop_shadow) {
    int64_t sy = dy - 1;
    if (circle_contains(dx_close, sy, radius, 2) ||
        circle_contains(dx_bg, sy, radius, 2))
      return blend(0x40000000, base);
  }
  return base;
}
