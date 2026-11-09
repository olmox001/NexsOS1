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
  return os1_gfx_rrect_coverage(x, y, width, height, radius) >= 128U;
}

void os1_gfx_button_geometry(int frame_x, int frame_width, int titlebar_y,
                             int titlebar_height, int shape, int side,
                             struct os1_gfx_button_geometry *out) {
  if (!out) return;
  out->size = 0;
  out->top = 0;
  out->close_cx = 0;
  out->bg_cx = 0;
  if (frame_width <= 0 || titlebar_height < 8 || shape < 0 || shape > 2 ||
      side < 0 || side > 1)
    return;
  int size = CHROME_BUTTON_SIZE;
  if (shape == 1 || shape == 2) size -= 2;
  if (size > titlebar_height - 4) size = titlebar_height - 4;
  if (frame_width < 2 * size + CHROME_BUTTON_GAP + 8) return;
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
  int hit = os1_gfx_button_hit(g, x, y);
  if (hit == OS1_GFX_BUTTON_NONE) return base;
  int cx = hit == OS1_GFX_BUTTON_CLOSE ? g->close_cx : g->bg_cx;
  int lx = (int)((int64_t)x - cx + g->size / 2);
  int ly = (int)((int64_t)y - g->top);
  int radius = shape == 0 ? g->size / 2 : (shape == 2 ? 4 : 0);
  unsigned coverage = os1_gfx_rrect_coverage(lx, ly, g->size, g->size, radius);
  uint32_t accent = hit == OS1_GFX_BUTTON_CLOSE ? close_argb : background_argb;
  /* Inset controls, a fine accent rim and explicit high-contrast glyphs.
   * Shape and hit geometry remain shared with the input service. */
  unsigned inner = os1_gfx_rrect_coverage(lx - 1, ly - 1,
      g->size - 2, g->size - 2, radius > 0 ? radius - 1 : 0);
  unsigned rim = coverage > inner ? coverage - inner : 0;
  unsigned opacity = (48U * coverage + 72U * rim) / 255U;
  if (drop_shadow && ly == g->size - 1) opacity += coverage / 8U;
  uint32_t result = blend((accent & 0x00ffffffU) | (opacity << 24), base);
  int dx = 2 * lx - (g->size - 1), dy = 2 * ly - (g->size - 1);
  int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
  int diagonal = ax - ay;
  if (diagonal < 0) diagonal = -diagonal;
  int glyph = hit == OS1_GFX_BUTTON_CLOSE ?
      (ax <= 7 && ay <= 7 && diagonal <= 2) : (ax <= 7 && ay <= 1);
  if (glyph) {
    unsigned luminance = ((base >> 16) & 255U) * 54U +
                         ((base >> 8) & 255U) * 183U + (base & 255U) * 19U;
    uint32_t ink = luminance < 32768U ? 0xfff4f5f7U : 0xff252830U;
    result = blend((ink & 0x00ffffffU) | (coverage << 24), result);
  }
  return result;
}
