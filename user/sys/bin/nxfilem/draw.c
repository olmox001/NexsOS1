/*
 * user/sys/bin/nxfilem/draw.c
 * Framebuffer ownership + low-level pixel/text primitives.
 *
 * Same technique as nxsettings.c/nxui.c: an owned ARGB buffer blitted once
 * per frame, glyphs drawn with font_lib directly onto that buffer (no
 * printf_win — this window is not a terminal), a single corner-quarter-
 * circle mask test shared by every rounded shape.
 */
#include "nxfilem.h"

uint32_t *fm_fb;
int fm_fb_w, fm_fb_h;
int fm_win_id = -1;
struct font_ctx *fm_font;

void fm_gfx_init(int win_id, int ww, int wh) {
  fm_win_id = win_id;
  fm_fb_w = ww;
  fm_fb_h = wh;
  fm_fb = (uint32_t *)malloc((size_t)ww * (size_t)wh * 4u);
  fm_font = font_load("/fonts/Rewir-Light.off");
}

void fm_gfx_realloc(int ww, int wh) {
  uint32_t *nb = (uint32_t *)malloc((size_t)ww * (size_t)wh * 4u);
  if (!nb)
    return;
  free(fm_fb);
  fm_fb = nb;
  fm_fb_w = ww;
  fm_fb_h = wh;
}

void fm_fill_rect(int x, int y, int w, int h, uint32_t color) {
  if (!fm_fb)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > fm_fb_w)
    w = fm_fb_w - x;
  if (y + h > fm_fb_h)
    h = fm_fb_h - y;
  if (w <= 0 || h <= 0)
    return;
  for (int j = 0; j < h; j++) {
    uint32_t *row = fm_fb + (y + j) * fm_fb_w + x;
    for (int i = 0; i < w; i++)
      row[i] = color;
  }
}

/* fm_rrect - filled rounded rectangle (corners clipped to a quarter-circle
 * of radius r). Identical technique to nxsettings.c's fb_rrect. */
void fm_rrect(int x, int y, int w, int h, int r, uint32_t color) {
  if (!fm_fb || w <= 0 || h <= 0)
    return;
  if (r < 0)
    r = 0;
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int ccx = -1, ccy = 0;
      if (i < r && j < r) {
        ccx = r;
        ccy = r;
      } else if (i >= w - r && j < r) {
        ccx = w - r - 1;
        ccy = r;
      } else if (i < r && j >= h - r) {
        ccx = r;
        ccy = h - r - 1;
      } else if (i >= w - r && j >= h - r) {
        ccx = w - r - 1;
        ccy = h - r - 1;
      }
      if (ccx >= 0) {
        int dx = i - ccx, dy = j - ccy;
        if (dx * dx + dy * dy > r * r)
          continue;
      }
      int px = x + i, py = y + j;
      if (px >= 0 && px < fm_fb_w && py >= 0 && py < fm_fb_h)
        fm_fb[py * fm_fb_w + px] = color;
    }
  }
}

static void fm_draw_glyph(int x, int y, uint32_t codepoint, uint32_t color) {
  if (!fm_font)
    return;
  int idx = (int)codepoint - fm_font->header.first_char;
  if (idx < 0 || idx >= fm_font->header.num_chars)
    return;
  struct font_glyph_info *gi = &fm_font->glyphs[idx];
  uint8_t *bitmap = fm_font->bitmap + gi->data_offset;
  int start_x = x + gi->x0;
  int start_y = y + fm_font->header.ascent + gi->y0;
  for (int gy = 0; gy < gi->height; gy++) {
    for (int gx = 0; gx < gi->width; gx++) {
      uint8_t alpha = bitmap[gy * gi->width + gx];
      if (alpha > 64) {
        int px = start_x + gx, py = start_y + gy;
        if (px >= 0 && px < fm_fb_w && py >= 0 && py < fm_fb_h)
          fm_fb[py * fm_fb_w + px] = color;
      }
    }
  }
}

int fm_text_width(const char *s) {
  if (!fm_font || !s)
    return 0;
  return font_string_width(fm_font, s);
}

void fm_draw_text(int x, int y, const char *s, uint32_t color) {
  if (!fm_font || !s)
    return;
  uint32_t cp;
  int consumed, cursor = x;
  size_t rem = strlen(s);
  const char *p = s;
  while (*p) {
    consumed = utf8_decode(p, rem, &cp);
    if (consumed <= 0) {
      p++;
      rem--;
      continue;
    }
    fm_draw_glyph(cursor, y, cp, color);
    int idx = (int)cp - fm_font->header.first_char;
    if (idx >= 0 && idx < fm_font->header.num_chars)
      cursor += fm_font->glyphs[idx].advance;
    p += consumed;
    rem -= consumed;
  }
}

/* fm_draw_icon - a cached per-filetype/folder icon (nxicon.h) when one is
 * available for the current theme, else a flat rounded-square fallback in
 * 'fallback_color' (same degrade-gracefully contract nxui.c's dock tiles
 * use for app icons). */
void fm_draw_icon(int x, int y, int size, int icon_id, int light,
                  uint32_t fallback_color) {
  os1_image_t *icon = nxicon_get(icon_id, light, size);
  if (!icon) {
    fm_rrect(x, y, size, size, 3, fallback_color);
    return;
  }
  for (int j = 0; j < size; j++) {
    for (int i = 0; i < size; i++) {
      uint32_t src = icon->pixels[j * size + i];
      uint8_t alpha = (src >> 24) & 0xFF;
      if (alpha == 0)
        continue;
      int px = x + i, py = y + j;
      if (px < 0 || px >= fm_fb_w || py < 0 || py >= fm_fb_h)
        continue;
      if (alpha == 255) {
        fm_fb[py * fm_fb_w + px] = src;
        continue;
      }
      uint32_t dst = fm_fb[py * fm_fb_w + px];
      uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
      uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
      uint32_t out_r = (sr * alpha + dr * (255 - alpha)) / 255;
      uint32_t out_g = (sg * alpha + dg * (255 - alpha)) / 255;
      uint32_t out_b = (sb * alpha + db * (255 - alpha)) / 255;
      fm_fb[py * fm_fb_w + px] =
          0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
    }
  }
}

void fm_blit(void) {
  if (fm_win_id < 0 || !fm_fb)
    return;
  window_blit(fm_win_id, 0, 0, fm_fb_w, fm_fb_h, fm_fb);
  compositor_render();
}
