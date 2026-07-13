/*
 * kernel/graphics/chrome.c — window-chrome primitives.
 *
 * Code extracted verbatim from compositor_render_internal /
 * compositor_handle_click so shadows, tints, buttons and borders render
 * pixel-identically; only the parameterisation changed (style/theme structs
 * stay data-only in the compositor).  All functions are allocation-free.
 */
#include <kernel/gfx_chrome.h>
#include <graphics/gl.h>

/* Titlebar button metrics (chrome geometry, not colours). */
#define CHROME_BUTTON_SIZE 16
#define CHROME_BUTTON_GAP 6 /* px gap between the background and close buttons */

/* Integer sqrt by the same incremental search the shadow SDF used inline. */
static inline int chrome_isqrt(int v) {
  int root = 0;
  while ((root + 1) * (root + 1) <= v)
    root++;
  return root;
}

int gfx_rrect_contains(int lx, int ly, int w, int h, int r) {
  if (r <= 0)
    return 1;
  if (lx < 0 || ly < 0 || lx >= w || ly >= h)
    return 0;
  int cx, cy;
  if (lx < r && ly < r) {
    cx = r - 1 - lx;
    cy = r - 1 - ly;
  } else if (lx >= w - r && ly < r) {
    cx = lx - (w - r);
    cy = r - 1 - ly;
  } else if (lx < r && ly >= h - r) {
    cx = r - 1 - lx;
    cy = ly - (h - r);
  } else if (lx >= w - r && ly >= h - r) {
    cx = lx - (w - r);
    cy = ly - (h - r);
  } else {
    return 1; /* not in a corner square */
  }
  return (cx * cx + cy * cy) <= (r * r);
}

void gfx_chrome_shadow_margins(int shadow_type, int shadow_size,
                               gfx_chrome_margins_t *out) {
  if (!out)
    return;
  int so = shadow_size > 0 ? shadow_size : 0;
  if (shadow_type == 2) {
    out->left = so * 2;
    out->right = so * 2;
    out->top = so * 2;
    out->bottom = so * 2 + so;
  } else if (shadow_type == 1) {
    out->left = so;
    out->right = so;
    out->top = so;
    out->bottom = so;
  } else {
    out->left = 0;
    out->right = so;
    out->top = 0;
    out->bottom = so;
  }
}

/* Clip a screen pixel against surface bounds and the exclusive clip rect. */
static inline int chrome_clipped(const gfx_surface_t *s,
                                 const gfx_rect_t *clip, int px, int py) {
  if (px < 0 || px >= s->width || py < 0 || py >= s->height)
    return 1;
  if (px < clip->x || px >= clip->x + clip->width || py < clip->y ||
      py >= clip->y + clip->height)
    return 1;
  return 0;
}

void gfx_chrome_shadow_solid(gfx_surface_t *surface, const gfx_rect_t *clip,
                             const gfx_rect_t *frame, int radius,
                             uint32_t argb) {
  if (!gfx_surface_valid(surface) || !clip || !frame)
    return;
  for (int sy = 0; sy < frame->height; sy++) {
    int py = frame->y + sy;
    for (int sx = 0; sx < frame->width; sx++) {
      int px = frame->x + sx;
      if (chrome_clipped(surface, clip, px, py))
        continue;
      if (!gfx_rrect_contains(sx, sy, frame->width, frame->height, radius))
        continue;
      surface->buffer[py * surface->stride + px] = argb;
    }
  }
}

/* Distance from local pixel (lx,ly) to the rounded frame, minus the radius —
 * the single-pass SDF both gradient shadows are built on. */
static inline int chrome_sdf_dist(int lx, int ly, int w, int h, int r) {
  int cx = lx;
  if (cx < r)
    cx = r;
  else if (cx > w - 1 - r)
    cx = w - 1 - r;
  int cy = ly;
  if (cy < r)
    cy = r;
  else if (cy > h - 1 - r)
    cy = h - 1 - r;
  int dx = lx - cx;
  int dy = ly - cy;
  return chrome_isqrt(dx * dx + dy * dy) - r;
}

void gfx_chrome_shadow_fast(gfx_surface_t *surface, const gfx_rect_t *clip,
                            const gfx_rect_t *frame, int radius, int spread) {
  if (!gfx_surface_valid(surface) || !clip || !frame || spread <= 0)
    return;
  int dw = frame->width;
  int full_h = frame->height;
  for (int sy = -spread; sy < full_h + spread; sy++) {
    int py = frame->y + sy;
    for (int sx = -spread; sx < dw + spread; sx++) {
      int px = frame->x + sx;
      if (chrome_clipped(surface, clip, px, py))
        continue;
      if (sx >= 0 && sx < dw && sy >= 0 && sy < full_h &&
          gfx_rrect_contains(sx, sy, dw, full_h, radius))
        continue;

      int dist = chrome_sdf_dist(sx, sy, dw, full_h, radius);
      if (dist <= spread) {
        int norm = (dist <= 0) ? 64 : ((spread - dist) * 64 / spread);
        uint32_t a = (0x30u * (uint32_t)norm * (uint32_t)norm) / (64u * 64u);
        if (a > 0)
          surface->buffer[py * surface->stride + px] = gl_blend_pixel(
              (a << 24) | 0x000000u, surface->buffer[py * surface->stride + px]);
      }
    }
  }
}

void gfx_chrome_shadow_premium(gfx_surface_t *surface, const gfx_rect_t *clip,
                               const gfx_rect_t *frame, int radius,
                               int spread) {
  if (!gfx_surface_valid(surface) || !clip || !frame || spread <= 0)
    return;
  int dw = frame->width;
  int full_h = frame->height;

  /* Two stacked shadows: a small dark contact one (immediate depth) and a
   * wide light diffuse one (ambient elevation), both offset downwards. */
  int spread_diffuse = spread * 2;
  int spread_contact = (spread > 1) ? (spread / 2) : 1;
  int y_off_diffuse = spread;
  int y_off_contact = spread_contact / 2;

  int min_y = -spread_diffuse;
  int max_y = full_h + spread_diffuse + y_off_diffuse;
  int min_x = -spread_diffuse;
  int max_x = dw + spread_diffuse;

  for (int sy = min_y; sy < max_y; sy++) {
    int py = frame->y + sy;
    for (int sx = min_x; sx < max_x; sx++) {
      int px = frame->x + sx;
      if (chrome_clipped(surface, clip, px, py))
        continue;
      if (sx >= 0 && sx < dw && sy >= 0 && sy < full_h &&
          gfx_rrect_contains(sx, sy, dw, full_h, radius))
        continue;

      int dist_diffuse =
          chrome_sdf_dist(sx, sy - y_off_diffuse, dw, full_h, radius);
      int dist_contact =
          chrome_sdf_dist(sx, sy - y_off_contact, dw, full_h, radius);

      uint32_t a_total = 0;
      if (dist_diffuse <= 0) {
        a_total += 0x24;
      } else if (dist_diffuse <= spread_diffuse) {
        int norm = (spread_diffuse - dist_diffuse) * 64 / spread_diffuse;
        a_total += (0x24u * (uint32_t)norm * (uint32_t)norm) / (64u * 64u);
      }
      if (dist_contact <= 0) {
        a_total += 0x30;
      } else if (dist_contact <= spread_contact) {
        int norm = (spread_contact - dist_contact) * 64 / spread_contact;
        a_total += (0x30u * (uint32_t)norm * (uint32_t)norm) / (64u * 64u);
      }

      if (a_total > 0) {
        if (a_total > 255)
          a_total = 255;
        uint32_t shadow_color = (a_total << 24) | 0x050510u;
        surface->buffer[py * surface->stride + px] = gl_blend_pixel(
            shadow_color, surface->buffer[py * surface->stride + px]);
      }
    }
  }

  /* Rim light: one faint bright row just above the frame's top edge. */
  {
    int py = frame->y - 1;
    for (int sx = radius; sx < dw - radius; sx++) {
      int px = frame->x + sx;
      if (chrome_clipped(surface, clip, px, py))
        continue;
      surface->buffer[py * surface->stride + px] = gl_blend_pixel(
          0x18FFFFFFu, surface->buffer[py * surface->stride + px]);
    }
  }
}

uint32_t gfx_chrome_titlebar_tint(int shadow_type, int y, int titlebar_h,
                                  uint32_t base) {
  if (titlebar_h <= 0)
    return base;
  if (shadow_type == 1) {
    if (y == 0)
      base = gl_blend_pixel(0x20FFFFFF, base);
  } else if (shadow_type == 2) {
    if (y == 0)
      base = gl_blend_pixel(0x40FFFFFF, base);
    int grad = (y * 24) / titlebar_h;
    if (grad > 0)
      base = gl_blend_pixel(((uint32_t)grad << 24) | 0x000000, base);
  }
  return base;
}

uint32_t gfx_chrome_content_separator(int shadow_type, int y, uint32_t base) {
  if (shadow_type == 0) {
    if (y == 0)
      base = gl_blend_pixel(0x20000000, base);
  } else if (shadow_type == 1) {
    if (y == 0)
      base = gl_blend_pixel(0x30000000, base);
    else if (y == 1)
      base = gl_blend_pixel(0x10000000, base);
  } else if (shadow_type == 2 && y < 4) {
    int a = (4 - y) * 16;
    base = gl_blend_pixel(((uint32_t)a << 24) | 0x000000, base);
  }
  return base;
}

void gfx_chrome_button_geometry(int frame_x, int frame_w, int titlebar_y,
                                int titlebar_h, int shape, int side,
                                gfx_button_geometry_t *out) {
  if (!out)
    return;
  int btn_size = CHROME_BUTTON_SIZE;
  /* Smaller buttons for the square/rounded shapes. */
  if (shape == 1 || shape == 2)
    btn_size -= 2;

  out->size = btn_size;
  out->top = titlebar_y + (titlebar_h - btn_size) / 2;

  if (side == 0) {
    /* Buttons on the left. */
    int close_left = frame_x + 4;
    out->close_cx = close_left + btn_size / 2;
    out->bg_cx = out->close_cx + btn_size + CHROME_BUTTON_GAP;
  } else {
    /* Buttons on the right. */
    int close_right = frame_x + frame_w - 4;
    out->close_cx = close_right - btn_size / 2;
    out->bg_cx = out->close_cx - btn_size - CHROME_BUTTON_GAP;
  }
}

int gfx_chrome_button_hit(const gfx_button_geometry_t *geometry, int x,
                          int y) {
  if (!geometry)
    return GFX_BUTTON_NONE;
  int btn_size = geometry->size;
  if (y < geometry->top || y >= geometry->top + btn_size)
    return GFX_BUTTON_NONE;
  int close_x = geometry->close_cx - btn_size / 2;
  int bg_x = geometry->bg_cx - btn_size / 2;
  if (x >= close_x && x < close_x + btn_size)
    return GFX_BUTTON_CLOSE;
  if (x >= bg_x && x < bg_x + btn_size)
    return GFX_BUTTON_BACKGROUND;
  return GFX_BUTTON_NONE;
}

uint32_t gfx_chrome_button_pixel(const gfx_button_geometry_t *geometry,
                                 int shape, int x, int y, uint32_t close_argb,
                                 uint32_t bg_argb, int drop_shadow,
                                 uint32_t base) {
  if (!geometry)
    return base;
  int btn_size = geometry->size;
  int local_y = y - geometry->top;
  int local_x_close = x - geometry->close_cx + (btn_size / 2);
  int local_x_bg = x - geometry->bg_cx + (btn_size / 2);

  if (shape == 1) {
    /* Pure square. */
    if (local_x_close >= 0 && local_x_close < btn_size && local_y >= 0 &&
        local_y < btn_size)
      return close_argb;
    if (local_x_bg >= 0 && local_x_bg < btn_size && local_y >= 0 &&
        local_y < btn_size)
      return bg_argb;
    if (drop_shadow) {
      if (local_y == btn_size && local_x_close >= 0 && local_x_close < btn_size)
        return gl_blend_pixel(0x60000000, base);
      if (local_y == btn_size && local_x_bg >= 0 && local_x_bg < btn_size)
        return gl_blend_pixel(0x60000000, base);
    }
    return base;
  }

  if (shape == 2) {
    /* Rounded square (Material). */
    int corner_radius = 6;
    if (gfx_rrect_contains(local_x_close, local_y, btn_size, btn_size,
                           corner_radius))
      return close_argb;
    if (gfx_rrect_contains(local_x_bg, local_y, btn_size, btn_size,
                           corner_radius))
      return bg_argb;
    if (drop_shadow) {
      if (gfx_rrect_contains(local_x_close, local_y - 1, btn_size, btn_size,
                             corner_radius))
        return gl_blend_pixel(0x50000000, base);
      if (gfx_rrect_contains(local_x_bg, local_y - 1, btn_size, btn_size,
                             corner_radius))
        return gl_blend_pixel(0x50000000, base);
    }
    return base;
  }

  /* Classic discs. */
  int radius = btn_size / 2 - 1;
  int ddy = y - (geometry->top + btn_size / 2);
  int dcx = x - geometry->close_cx;
  int dbx = x - geometry->bg_cx;

  if (dcx * dcx + ddy * ddy <= radius * radius)
    return close_argb;
  if (dbx * dbx + ddy * ddy <= radius * radius)
    return bg_argb;
  if (drop_shadow) {
    int ddy_s = ddy - 1;
    if (dcx * dcx + ddy_s * ddy_s <= radius * radius + 2)
      return gl_blend_pixel(0x40000000, base);
    if (dbx * dbx + ddy_s * ddy_s <= radius * radius + 2)
      return gl_blend_pixel(0x40000000, base);
  }
  return base;
}

void gfx_chrome_border(gfx_surface_t *surface, const gfx_rect_t *clip,
                       const gfx_rect_t *frame, int radius, uint32_t argb) {
  if (!gfx_surface_valid(surface) || !clip || !frame)
    return;
  int dw = frame->width;
  int full_h = frame->height;
  for (int ly = 0; ly < full_h; ly++) {
    int py = frame->y + ly;
    for (int lx = 0; lx < dw; lx++) {
      /* perimeter pixels only */
      int on_edge = (lx == 0 || ly == 0 || lx == dw - 1 || ly == full_h - 1);
      /* for rounded corners, also treat the rounded boundary as edge */
      int inside = gfx_rrect_contains(lx, ly, dw, full_h, radius);
      int inside_inset =
          radius ? gfx_rrect_contains(lx - 1, ly - 1, dw - 2, full_h - 2,
                                      radius > 0 ? radius - 1 : 0)
                 : (lx > 0 && ly > 0 && lx < dw - 1 && ly < full_h - 1);
      if (inside && (on_edge || !inside_inset)) {
        int px = frame->x + lx;
        if (!chrome_clipped(surface, clip, px, py))
          surface->buffer[py * surface->stride + px] = argb;
      }
    }
  }
}
