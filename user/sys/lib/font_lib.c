/*
 * user/sys/lib/font_lib.c
 * Userland font rendering library
 *
 * Provides loading, measurement, and rendering of pre-rasterised bitmap fonts
 * produced by fontman.  The on-disk format is:
 *
 *   [ struct font_header ]
 *   [ struct font_glyph_info * header.num_chars ]
 *   [ raw alpha bitmap bytes ]
 *
 * The font_ctx structure holds pointers into a single contiguous heap buffer
 * (raw_data) allocated by font_load().
 *
 * Rendering path (draw_glyph):
 *   Each glyph row is converted into horizontal opaque spans and each span is
 *   drawn with one window_draw() call. This preserves transparent backgrounds
 *   while avoiding the old one-syscall-per-lit-pixel path.
 *
 * UTF-8 iteration (font_draw_string, font_string_width):
 *   Calls utf8_decode() (lib.c) per character; advances by the byte count
 *   returned.  Invalid sequences (utf8_decode returns 0) skip one byte to
 *   prevent an infinite loop.
 *
 * NOTE: font_lib.c is compiled as part of lib.o (included by lib.c:57) rather
 * than compiled separately.  Its symbols are therefore available to all ELFs
 * that link lib.o.
 */
#include <font_lib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int add_overflows_size(size_t a, size_t b) {
    return a > ((size_t)-1) - b;
}

static int mul_overflows_size(size_t a, size_t b) {
    return b != 0 && a > ((size_t)-1) / b;
}

static int font_validate_buffer(const void *data, size_t size) {
    if (!data || size < sizeof(struct font_header)) return 0;

    const struct font_header *h = (const struct font_header *)data;
    if (h->magic != FONT_MAGIC) return 0;
    if (h->num_chars == 0) return 0;
    if ((uint32_t)h->ascent + (uint32_t)h->descent == 0) return 0;

    if (mul_overflows_size(h->num_chars, sizeof(struct font_glyph_info))) return 0;
    size_t glyph_bytes = (size_t)h->num_chars * sizeof(struct font_glyph_info);
    if (add_overflows_size(sizeof(struct font_header), glyph_bytes)) return 0;
    size_t bitmap_offset = sizeof(struct font_header) + glyph_bytes;
    if (add_overflows_size(bitmap_offset, h->bitmap_size)) return 0;
    if (bitmap_offset + h->bitmap_size > size) return 0;

    const struct font_glyph_info *glyphs =
        (const struct font_glyph_info *)((const uint8_t *)data + sizeof(struct font_header));
    for (uint16_t i = 0; i < h->num_chars; i++) {
        const struct font_glyph_info *gi = &glyphs[i];
        if (mul_overflows_size(gi->width, gi->height)) return 0;
        size_t glyph_size = (size_t)gi->width * (size_t)gi->height;
        if (add_overflows_size(gi->data_offset, glyph_size)) return 0;
        if (gi->data_offset + glyph_size > h->bitmap_size) return 0;
    }

    return 1;
}

/*
 * font_load - read a packed font file and set up a font_ctx.
 *
 * path: filesystem path to a packed font produced by fontman.
 *
 * Reads the file in two calls (size probe then full read), validates the
 * FONT_MAGIC header field, and sets up interior pointers into the single
 * raw_data allocation:
 *   ctx->glyphs  = raw_data + sizeof(font_header)
 *   ctx->bitmap  = raw_data + sizeof(font_header) + num_chars * sizeof(glyph_info)
 *
 * Returns a heap-allocated font_ctx on success, or NULL on file-not-found,
 * malloc failure, or magic mismatch.
 *
 * Ownership: caller must pass the returned ctx to font_free() when done.
 */
struct font_ctx *font_load(const char *path) {
    /* Probe file size: file_read with buf=NULL, size=0 returns byte count. */
    int size = file_read(path, NULL, 0, 0);
    if (size <= 0) return NULL;

    void *data = malloc(size);
    if (!data) return NULL;

    if (file_read(path, data, size, 0) != size) {
        free(data);
        return NULL;
    }

    if (!font_validate_buffer(data, (size_t)size)) {
        free(data);
        return NULL;
    }

    struct font_header *h = (struct font_header *)data;
    struct font_ctx *ctx = malloc(sizeof(struct font_ctx));
    if (!ctx) {
        free(data);
        return NULL;
    }

    ctx->header = *h;  /* Copy header fields by value */
    /* glyphs array immediately follows the font_header in the packed buffer. */
    ctx->glyphs = (struct font_glyph_info *)((uint8_t *)data + sizeof(struct font_header));
    /* Bitmap bytes follow the glyph array. */
    ctx->bitmap = (uint8_t *)ctx->glyphs + h->num_chars * sizeof(struct font_glyph_info);
    ctx->raw_data = data;  /* Retains ownership for font_free() */

    return ctx;
}

/*
 * font_free - release a font_ctx and its underlying heap buffer.
 *
 * Frees ctx->raw_data (which also invalidates ctx->glyphs and ctx->bitmap
 * since they point into it), then frees ctx itself.
 *
 * Safe to call with NULL ctx.
 */
void font_free(struct font_ctx *ctx) {
    if (ctx) {
        if (ctx->raw_data) free(ctx->raw_data);
        free(ctx);
    }
}

/*
 * draw_glyph - render a single Unicode codepoint into a compositor window.
 *
 * win_id:    compositor window ID.
 * ctx:       loaded font context.
 * x, y:      pen position (baseline left corner).
 * codepoint: Unicode codepoint to render.
 * color:     ARGB foreground colour (0xAARRGGBB).
 *
 * The glyph bitmap is a row-major alpha (grayscale) image; each byte is an
 * opacity value 0..255. Pixels with alpha > 128 are drawn as filled spans.
 *
 * start_y accounts for the font ascent so that the baseline is at y + ascent
 * and gi->y0 is a signed offset from that baseline.
 *
 * Codepoints outside the font's first_char..first_char+num_chars range are
 * silently skipped (no replacement glyph).
 *
 * Performance note: contiguous pixels on the same row are coalesced into a
 * single rectangle, which keeps text transparent while sharply reducing
 * compositor syscalls for normal glyphs.
 */
static void draw_glyph(int win_id, struct font_ctx *ctx, int x, int y, uint32_t codepoint, uint32_t color) {
    int idx = (int)codepoint - ctx->header.first_char;
    if (idx < 0 || idx >= ctx->header.num_chars) return;

    struct font_glyph_info *gi = &ctx->glyphs[idx];
    uint8_t *bitmap = ctx->bitmap + gi->data_offset;

    /* start_y: baseline = y + ascent; gi->y0 is a signed descent offset. */
    int start_x = x + gi->x0;
    int start_y = y + ctx->header.ascent + gi->y0;

    for (int gy = 0; gy < gi->height; gy++) {
        int gx = 0;
        while (gx < gi->width) {
            while (gx < gi->width && bitmap[gy * gi->width + gx] <= 128) {
                gx++;
            }

            int span_start = gx;
            while (gx < gi->width && bitmap[gy * gi->width + gx] > 128) {
                gx++;
            }

            if (gx > span_start) {
                window_draw(win_id, start_x + span_start, start_y + gy,
                            gx - span_start, 1, color);
            }
        }
    }
}

/*
 * font_draw_string - render a UTF-8 string into a compositor window.
 *
 * Iterates the string using utf8_decode() to handle multi-byte sequences,
 * calls draw_glyph() for each codepoint, and advances cursor_x by the
 * glyph's advance width.
 *
 * Invalid UTF-8 bytes (utf8_decode returns 0) advance the pointer by one
 * byte to prevent an infinite loop; no replacement glyph is drawn.
 *
 * Codepoints outside the font range are skipped (no advance applied either,
 * so they consume no horizontal space).
 */
void font_draw_string(int win_id, struct font_ctx *ctx, int x, int y, const char *str, uint32_t color) {
    if (!ctx || !str) return;

  int cursor_x = x;
  uint32_t codepoint;
  int consumed;
  size_t rem = 0;
  const char *p = str;
  while (*p) { p++; rem++; }

  while (*str) {
      consumed = utf8_decode(str, rem, &codepoint);
      if (consumed <= 0) {
          str++;  /* Skip invalid byte to avoid infinite loop */
          rem--;
          continue;
      }
      draw_glyph(win_id, ctx, cursor_x, y, codepoint, color);
      int idx = (int)codepoint - ctx->header.first_char;
      if (idx >= 0 && idx < ctx->header.num_chars) {
          cursor_x += ctx->glyphs[idx].advance;
      }
      str += consumed;
      rem -= consumed;
  }
}

/*
 * font_string_width - compute the rendered pixel width of a UTF-8 string.
 *
 * Walks the string identically to font_draw_string but accumulates advance
 * widths without drawing.  Used by callers to centre or right-align text.
 *
 * Returns the total advance in pixels, or 0 for NULL/empty input.
 *
 * Note: does not account for kerning (stb_truetype provides per-pair
 * kerning but the packed font format does not store a kerning table).
 */
int font_string_width(struct font_ctx *ctx, const char *str) {
    if (!ctx || !str) return 0;

  int width = 0;
  uint32_t codepoint;
  int consumed;
  size_t rem = 0;
  const char *p = str;
  while (*p) { p++; rem++; }

  while (*str) {
      consumed = utf8_decode(str, rem, &codepoint);
      if (consumed <= 0) {
          str++;  /* Skip invalid byte */
          rem--;
          continue;
      }
      int idx = (int)codepoint - ctx->header.first_char;
      if (idx >= 0 && idx < ctx->header.num_chars) {
          width += ctx->glyphs[idx].advance;
      }
      str += consumed;
      rem -= consumed;
  }
    return width;
}
