/* Userland decoration rasterizer. No kernel calls, allocation or global state.
 * Coverage work is restricted to corner/edge pixels; rectangle interiors use
 * the ordinary raster path. All externally supplied geometry is clipped. */
#include <os1_gfx.h>
#include <stdint.h>

#define DECORATION_MAX_DIMENSION 16384
#define DECORATION_MAX_RADIUS 64
#define DECORATION_MAX_SHADOW 24

static uint32_t over_coverage(uint32_t color, uint32_t base, unsigned coverage) {
  unsigned alpha = (color >> 24) * coverage / 255U;
  if (alpha == 255U) return color;
  if (alpha == 0U) return base;
  uint32_t covered = (color & 0x00ffffffU) | (alpha << 24);
  return os1_gfx_argb_over(covered, base);
}

unsigned os1_gfx_rrect_coverage(int x, int y, int width, int height, int radius) {
  if (width <= 0 || height <= 0 || width > DECORATION_MAX_DIMENSION ||
      height > DECORATION_MAX_DIMENSION || x < 0 || y < 0 ||
      x >= width || y >= height) return 0;
  if (radius <= 0) return 255;
  if (radius > DECORATION_MAX_RADIUS) radius = DECORATION_MAX_RADIUS;
  if (radius > width / 2) radius = width / 2;
  if (radius > height / 2) radius = height / 2;
  if ((x >= radius && x < width - radius) ||
      (y >= radius && y < height - radius)) return 255;
  int cx = x < radius ? radius : width - radius;
  int cy = y < radius ? radius : height - radius;
  unsigned inside = 0;
  for (int sy = 1; sy <= 3; sy += 2) {
    for (int sx = 1; sx <= 3; sx += 2) {
      int dx = 4 * (x - cx) + sx, dy = 4 * (y - cy) + sy;
      inside += (unsigned)(dx * dx + dy * dy <= 16 * radius * radius);
    }
  }
  return inside * 255U / 4U;
}

static void decoration_pixel(struct os1_gfx_surface *frame, int64_t x, int64_t y,
                             uint32_t color, unsigned coverage) {
  if (x < 0 || y < 0 || x >= (int64_t)frame->width ||
      y >= (int64_t)frame->height || coverage == 0U) return;
  uint32_t *pixel = frame->pixels + (size_t)y * frame->stride + (size_t)x;
  *pixel = over_coverage(color, *pixel, coverage);
}

static int bounded_radius(const os1_compositor_style_t *style) {
  int radius = style->rounded_corners ? style->border_radius : 0;
  if (radius < 0) return 0;
  return radius > DECORATION_MAX_RADIUS ? DECORATION_MAX_RADIUS : radius;
}

static void decoration_shadow(struct os1_gfx_surface *frame,
    const struct os1_gfx_rect *box, int radius, int size) {
  if (size <= 0) return;
  if (size > DECORATION_MAX_SHADOW) size = DECORATION_MAX_SHADOW;
  for (int y = -size; y < box->height + size; ++y) {
    for (int x = -size; x < box->width + size; ++x) {
      int inset = y >= radius && y < box->height - radius ? 0 : radius;
      if (y >= 0 && y < box->height && x == inset &&
          box->width - inset > x) x = box->width - inset;
      if (x >= box->width + size) break;
      unsigned inner = os1_gfx_rrect_coverage(x, y, box->width, box->height, radius);
      if (inner == 255U) continue;
      unsigned alpha = 0;
      for (int layer = 1; layer <= size; ++layer) {
        alpha += os1_gfx_rrect_coverage(x + layer, y + layer - 1,
            box->width + 2 * layer, box->height + 2 * layer, radius + layer);
      }
      alpha = alpha * (255U - inner) / (255U * (unsigned)size * 4U);
      decoration_pixel(frame, (int64_t)box->x + x, (int64_t)box->y + y,
                        0xff000000U, alpha);
    }
  }
}

static void decoration_title(struct os1_gfx_surface *frame,
    const struct os1_gfx_rect *box, int height, int radius, int tint_style, uint32_t color) {
  for (int y = 0; y < height; ++y) {
    uint32_t tint = os1_gfx_titlebar_tint(tint_style, y, height, color);
    for (int x = 0; x < box->width; ++x)
      decoration_pixel(frame, (int64_t)box->x + x, (int64_t)box->y + y,
          tint, os1_gfx_rrect_coverage(x, y, box->width, box->height, radius));
  }
}

static void decoration_border(struct os1_gfx_surface *frame,
    const struct os1_gfx_rect *box, int radius, uint32_t color) {
  for (int y = 0; y < box->height; ++y) {
    for (int x = 0; x < box->width; ++x) {
      int inset = y > radius && y < box->height - radius - 1 ? 1 : radius + 1;
      if (y > 0 && y < box->height - 1 && x == inset &&
          box->width - inset > x) x = box->width - inset;
      unsigned outer = os1_gfx_rrect_coverage(x, y, box->width, box->height, radius);
      unsigned inner = os1_gfx_rrect_coverage(x - 1, y - 1,
          box->width - 2, box->height - 2, radius > 0 ? radius - 1 : 0);
      if (outer > inner)
        decoration_pixel(frame, (int64_t)box->x + x, (int64_t)box->y + y,
                          color, outer - inner);
    }
  }
}

void os1_gfx_window_decoration(struct os1_gfx_surface *frame,
    const struct os1_gfx_rect *content, const os1_compositor_style_t *style,
    const os1_compositor_theme_t *theme, int focused, int protected_window) {
  if (!os1_gfx_surface_valid(frame) || !content || !style || !theme ||
      content->width <= 0 || content->height <= 0 ||
      content->width > 8192 || content->height > 8192 ||
      style->titlebar_height < 0 || style->titlebar_height > 64) return;
  int height = style->titlebar ? style->titlebar_height : 0;
  if ((int64_t)content->y - height < INT32_MIN) return;
  struct os1_gfx_rect box = {content->x, content->y - height,
                             content->width, content->height + height};
  int radius = bounded_radius(style);
  if (style->shadows) decoration_shadow(frame, &box, radius, style->shadow_size);
  decoration_title(frame, &box, height, radius, style->shadow_type,
                     focused ? theme->title_active : theme->title_inactive);
  if (style->window_borders) decoration_border(frame, &box, radius, theme->border);
  if (!height || protected_window) return;
  struct os1_gfx_button_geometry buttons;
  os1_gfx_button_geometry(box.x, box.width, box.y, height,
                           style->button_shape, style->button_side, &buttons);
  for (int button = 0; button < 2; ++button) {
    int cx = button == 0 ? buttons.close_cx : buttons.bg_cx;
    for (int y = 0; y < buttons.size; ++y) {
      for (int x = 0; x < buttons.size; ++x) {
        int64_t px = (int64_t)cx - buttons.size / 2 + x;
        int64_t py = (int64_t)buttons.top + y;
        if (px < 0 || py < 0 || px >= (int64_t)frame->width ||
            py >= (int64_t)frame->height) continue;
        uint32_t *pixel = frame->pixels + (size_t)py * frame->stride + (size_t)px;
        *pixel = os1_gfx_button_pixel(&buttons, style->button_shape, (int)px,
            (int)py, theme->close_btn, 0xffe8b949U, 0, *pixel);
      }
    }
  }
}

void os1_gfx_composite_rounded(struct os1_gfx_surface *destination,
    const struct os1_gfx_surface *source, const struct os1_gfx_rect *content,
    int title_height, int radius) {
  if (!os1_gfx_surface_valid(destination) || !os1_gfx_surface_valid(source) ||
      !content || content->width <= 0 || content->height <= 0 ||
      content->width > 8192 || content->height > 8192 ||
      title_height < 0 || title_height > 64) return;
  if (radius <= 0) {
    os1_gfx_composite_scaled(destination, source, content->x, content->y,
                              content->width, content->height);
    return;
  }
  int same_size = source->width == (size_t)content->width &&
                  source->height == (size_t)content->height;
  for (int y = 0; y < content->height; ++y) {
    int64_t py = (int64_t)content->y + y;
    if (py < 0 || py >= (int64_t)destination->height) continue;
    size_t sy = same_size ? (size_t)y : (size_t)y * source->height / (size_t)content->height;
    for (int x = 0; x < content->width; ++x) {
      int64_t px = (int64_t)content->x + x;
      if (px < 0 || px >= (int64_t)destination->width) continue;
      unsigned coverage = os1_gfx_rrect_coverage(x, y + title_height,
          content->width, content->height + title_height, radius);
      size_t sx = same_size ? (size_t)x : (size_t)x * source->width / (size_t)content->width;
      uint32_t *dst = destination->pixels + (size_t)py * destination->stride + (size_t)px;
      *dst = over_coverage(source->pixels[sy * source->stride + sx], *dst, coverage);
    }
  }
}
