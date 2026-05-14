/*
 * kernel/graphics/font.c
 * Font rendering and management
 */
#include <graphics/gl.h>
#include <kernel/graphics.h>
#include <kernel/types.h>
#include <font.h>
#include <drivers/gpu/gpu.h>

/* Include the pre-rasterized Rewir-Light.ttf */
#include <graphics/default_font.h>

/* Forward declarations */
int utf8_decode(const char *s, uint32_t *code);

/* Internal font state */
static struct {
    struct font_header header;
    const struct font_glyph_info *glyphs;
    const uint8_t *bitmap;
    int is_dynamic;
} current_font = {
    .header = {
        .magic = FONT_MAGIC,
        .first_char = FONT_FIRST_CHAR,
        .num_chars = FONT_NUM_CHARS,
        .ascent = FONT_ASCENT,
        .descent = FONT_DESCENT,
        .bitmap_size = 0
    },
    .glyphs = font_glyphs,
    .bitmap = font_bitmap,
    .is_dynamic = 0
};

/*
 * Draw character using GL
 */
void gl_draw_char(struct gl_surface *surf, int x, int y, uint32_t codepoint,
                  uint32_t color) {
  if (!surf || !current_font.bitmap)
    return;

  int idx = (int)codepoint - current_font.header.first_char;
  if (idx < 0 || idx >= current_font.header.num_chars)
    return;

  const struct font_glyph_info *gi = &current_font.glyphs[idx];
  const uint8_t *bitmap = current_font.bitmap + gi->data_offset;

  int start_x = x + gi->x0;
  int start_y = y + current_font.header.ascent + gi->y0;

  uint32_t r_color = (color >> 16) & 0xFF;
  uint32_t g_color = (color >> 8) & 0xFF;
  uint32_t b_color = color & 0xFF;

  for (int gy = 0; gy < gi->height; gy++) {
    for (int gx = 0; gx < gi->width; gx++) {
      uint8_t alpha = bitmap[gy * gi->width + gx];
      if (alpha == 0)
        continue;

      int px = start_x + gx;
      int py = start_y + gy;

      if (px >= 0 && px < (int)surf->width && py >= 0 && py < (int)surf->height) {
        if (alpha == 255) {
          surf->buffer[py * surf->stride + px] = color;
          continue;
        }

        /* Alpha blending approximation using shifts */
        uint32_t bg = surf->buffer[py * surf->stride + px];
        uint32_t inv_alpha = 255 - alpha;

        uint32_t r = (r_color * alpha + ((bg >> 16) & 0xFF) * inv_alpha) >> 8;
        uint32_t gr = (g_color * alpha + ((bg >> 8) & 0xFF) * inv_alpha) >> 8;
        uint32_t b = (b_color * alpha + (bg & 0xFF) * inv_alpha) >> 8;

        surf->buffer[py * surf->stride + px] = 0xFF000000 | (r << 16) | (gr << 8) | b;
      }
    }
  }
}

/*
 * Get character advance width
 */
int graphics_char_width(uint32_t codepoint) {
  int idx = (int)codepoint - current_font.header.first_char;
  if (idx < 0 || idx >= current_font.header.num_chars)
    return 0;
  return current_font.glyphs[idx].advance;
}

/*
 * Draw string using GL (UTF-8 supported)
 */
void gl_draw_string(struct gl_surface *surf, int x, int y, const char *str,
                    uint32_t color) {
  if (!surf || !str)
    return;

  int cursor_x = x;
  uint32_t codepoint;
  int consumed;

  while (*str) {
    consumed = utf8_decode(str, &codepoint);
    if (consumed <= 0) {
        str++;
        continue;
    }
    gl_draw_char(surf, cursor_x, y, codepoint, color);
    cursor_x += graphics_char_width(codepoint);
    str += consumed;
  }
}

/*
 * Get string width in pixels (UTF-8 supported)
 */
int graphics_string_width(const char *str) {
  if (!str)
    return 0;

  int width = 0;
  uint32_t codepoint;
  int consumed;

  while (*str) {
    consumed = utf8_decode(str, &codepoint);
    if (consumed <= 0) {
        str++;
        continue;
    }
    width += graphics_char_width(codepoint);
    str += consumed;
  }
  return width;
}

/*
 * Get font height
 */
int graphics_font_height(void) { 
    return current_font.header.ascent + current_font.header.descent; 
}

/*
 * Get font ascent
 */
int graphics_font_ascent(void) { 
    return current_font.header.ascent; 
}

/*
 * Get max character width (for grid systems)
 */
int graphics_font_max_width(void) {
    int max_w = 0;
    for (int i = 0; i < current_font.header.num_chars; i++) {
        if (current_font.glyphs[i].advance > max_w)
            max_w = current_font.glyphs[i].advance;
    }
    return max_w > 0 ? max_w : 8;
}

/*
 * System Call: Set Font
 */
int sys_set_font(void *data, size_t size) {
    if (!data || size < sizeof(struct font_header)) return -1;

    struct font_header *h = (struct font_header *)data;
    if (h->magic != FONT_MAGIC) return -2;

    size_t expected = sizeof(struct font_header) + 
                     h->num_chars * sizeof(struct font_glyph_info) + 
                     h->bitmap_size;
    if (size < expected) return -3;

    current_font.header = *h;
    current_font.glyphs = (struct font_glyph_info *)((uint8_t *)data + sizeof(struct font_header));
    current_font.bitmap = (uint8_t *)current_font.glyphs + h->num_chars * sizeof(struct font_glyph_info);
    current_font.is_dynamic = 1;

    return 0;
}
