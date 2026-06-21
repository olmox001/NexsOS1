/*
 * kernel/graphics/font.c
 * Font rendering and management
 *
 * Role:
 *   Per-glyph alpha-masked blit from a pre-rasterised bitmap font embedded
 *   via <graphics/default_font.h> (Rewir-Light.ttf, compiled to a C array).
 *   Provides gl_draw_char / gl_draw_string for use by compositor.c's terminal
 *   emulator, and exports font metric queries (height, ascent, max width).
 *   Also implements sys_set_font(), the syscall entry point (syscall 253) that
 *   allows userland to replace the active font at runtime.
 *
 * Font state:
 *   The singleton current_font struct holds a font_header (magic, first_char,
 *   num_chars, ascent, descent, bitmap_size), a pointer to the glyph-info
 *   array (glyphs), a pointer to the bitmap data, and an is_dynamic flag.
 *   At startup, glyphs and bitmap point into the statically-linked default
 *   font arrays.  After sys_set_font(), they point into user-space memory.
 *
 * Rendering:
 *   gl_draw_char locates the glyph_info for a codepoint, reads the per-pixel
 *   alpha mask from the bitmap, and blends each non-zero pixel onto the
 *   surface using an >>8 approximation (not exact /255 division).  Fully
 *   opaque pixels (alpha=255) are written directly.
 *
 * Locking & IRQ context:
 *   No lock protects current_font.  gl_draw_char is called from
 *   compositor_window_write (under compositor_lock) and from
 *   compositor_render_internal (also under compositor_lock from
 *   compositor_tick, which fires from a timer IRQ).  sys_set_font is called
 *   from syscall context.  There is no synchronisation between them.
 *
 * Known issues:
 *   GFX-FONT-01 (W4 SECURITY BUG, FIXED) sys_set_font no longer stores the raw
 *               userland pointer: it copies the whole blob into a kmalloc'd
 *               kernel buffer, validates it against the kernel copy (magic,
 *               metrics, per-glyph bitmap bounds), and publishes an immutable
 *               descriptor behind font_lock, retiring the previous one.  This
 *               removes the dangling-pointer use-after-free (process exit after
 *               set-font), the kernel-memory info-leak (a kernel VA passed as
 *               'data'), and the size-overflow path (num_chars is uint16 and
 *               bitmap_size uint32, with the blob bounded by FONT_MAX_BLOB).
 *   GFX-FONT-02 (W3 BUG, FIXED) graphics_font_height() floors to the built-in
 *               default height when ascent+descent <= 0, so a malformed font
 *               (ascent=descent=0 via sys_set_font) can no longer divide-by-zero
 *               in compositor_create_window (h / char_h, compositor.c:204).
 */
#include <graphics/gl.h>
#include <kernel/graphics.h>
#include <kernel/types.h>
#include <kernel/arch.h>      /* arch_copy_from_user (GFX-FONT-01) */
#include <kernel/kmalloc.h>   /* kmalloc/kfree (GFX-FONT-01) */
#include <kernel/spinlock.h>  /* font_lock (GFX-FONT-01) */
#include <font.h>
#include <drivers/gpu/gpu.h>

/* Include the pre-rasterized Rewir-Light.ttf */
#include <graphics/default_font.h>

/* Forward declarations */
/* utf8_decode: defined in kernel/lib/utf8.c (or equivalent); advances 's' by
 * the byte width of the first codepoint and writes the decoded value to *code.
 * Returns byte count consumed (1..4), or <= 0 on invalid sequence. */
int utf8_decode(const char *s, size_t len, uint32_t *code);

/* Internal font state */
/* GFX-FONT-01: the active font is an immutable descriptor published behind
 * font_lock as current_font.  sys_set_font builds a new descriptor in a single
 * kmalloc block, swaps the pointer under the lock, and retires the previous
 * block; readers hold font_lock while they touch the descriptor, so a retired
 * buffer is never freed under a live reader.  heap_base is the kmalloc block to
 * free on retire (NULL for the static built-in default, which is never freed). */
struct font_state {
    struct font_header header;
    const struct font_glyph_info *glyphs;
    const uint8_t *bitmap;
    void *heap_base;
};

static struct font_state default_font = {
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
    .heap_base = 0
};

static struct font_state *current_font = &default_font;
static DEFINE_SPINLOCK(font_lock);

#define FONT_MAX_BLOB (8u * 1024 * 1024)  /* upper bound on a user font blob */

/*
 * gl_draw_char - render one Unicode codepoint from the active font onto surf.
 *
 * Params: surf — target surface; x, y — top-left origin for the glyph cell;
 *         codepoint — Unicode scalar value; color — ARGB8888 foreground.
 *
 * Algorithm:
 *   1. Compute glyph index: idx = codepoint - first_char.  Out-of-range
 *      codepoints (idx < 0 or >= num_chars) are silently skipped.
 *   2. Look up font_glyph_info gi from current_font.glyphs[idx].
 *   3. Compute pixel origin: start_x = x + gi->x0,
 *      start_y = y + ascent + gi->y0  (baseline offset).
 *   4. Walk the glyph's alpha mask (gi->height rows x gi->width cols):
 *      - alpha==0: skip (transparent pixel).
 *      - alpha==255: direct write (no blend arithmetic).
 *      - otherwise: blend using >>8 approximation (not exact /255).
 *        Note: >>8 differs from /255 by at most 1 LSB; visible only at
 *        specific alpha values (e.g. alpha=128, dst=255 → 127 vs 128).
 *   5. Per-pixel bounds check against surf->width/height before any write.
 *
 * GFX-FONT-01 (fixed): current_font.glyphs/bitmap now reference a kmalloc'd
 *   kernel descriptor (never userland memory).
 *
 * Locking: takes font_lock across the blit so a concurrent sys_set_font cannot
 *          retire/free the descriptor mid-read; also called under compositor_lock
 *          from compositor_window_write and compositor_render_internal
 *          (lock order: compositor_lock -> font_lock).
 * Side effects: writes pixels to surf->buffer.
 */
/*
 * Draw character using GL
 */
/* gl_draw_char_clipped - blit one glyph, writing only pixels inside the clip
 * box [cx1,cx2) x [cy1,cy2).  gl_draw_char passes the full surface as the clip
 * box; the compositor passes the per-frame damage box (GFX-COMP-03 damage clip)
 * so title text never paints outside the changed region. */
static void gl_draw_char_clipped(struct gl_surface *surf, int x, int y,
                                 uint32_t codepoint, uint32_t color, int cx1,
                                 int cy1, int cx2, int cy2) {
  if (!surf)
    return;

  /* Intersect the requested clip box with the surface bounds. */
  if (cx1 < 0)
    cx1 = 0;
  if (cy1 < 0)
    cy1 = 0;
  if (cx2 > (int)surf->width)
    cx2 = (int)surf->width;
  if (cy2 > (int)surf->height)
    cy2 = (int)surf->height;
  if (cx1 >= cx2 || cy1 >= cy2)
    return;

  /* GFX-FONT-01: hold font_lock across the whole blit so a concurrent
   * sys_set_font cannot free the descriptor's bitmap while we read it. */
  uint64_t flags;
  spin_lock_irqsave(&font_lock, &flags);
  const struct font_state *f = current_font;

  if (!f->bitmap) {
    spin_unlock_irqrestore(&font_lock, flags);
    return;
  }

  int idx = (int)codepoint - f->header.first_char;
  if (idx < 0 || idx >= f->header.num_chars) {
    spin_unlock_irqrestore(&font_lock, flags);
    return;
  }

  const struct font_glyph_info *gi = &f->glyphs[idx];
  const uint8_t *bitmap = f->bitmap + gi->data_offset;

  int start_x = x + gi->x0;
  int start_y = y + f->header.ascent + gi->y0;

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

      if (px >= cx1 && px < cx2 && py >= cy1 && py < cy2) {
        if (alpha == 255) {
          surf->buffer[py * surf->stride + px] = color;
          continue;
        }

        /* Proper coverage blend: round-to-nearest /255 (not >>8), so anti-
         * aliased glyph edges are full-strength, not dimmed (GFX-DYN-01 #121.4). */
        uint32_t bg = surf->buffer[py * surf->stride + px];
        uint32_t inv_alpha = 255 - alpha;

        uint32_t r = gl_div255(r_color * alpha + ((bg >> 16) & 0xFF) * inv_alpha);
        uint32_t gr = gl_div255(g_color * alpha + ((bg >> 8) & 0xFF) * inv_alpha);
        uint32_t b = gl_div255(b_color * alpha + (bg & 0xFF) * inv_alpha);

        surf->buffer[py * surf->stride + px] = 0xFF000000 | (r << 16) | (gr << 8) | b;
      }
    }
  }
  spin_unlock_irqrestore(&font_lock, flags);
}

void gl_draw_char(struct gl_surface *surf, int x, int y, uint32_t codepoint,
                  uint32_t color) {
  if (!surf)
    return;
  gl_draw_char_clipped(surf, x, y, codepoint, color, 0, 0, (int)surf->width,
                       (int)surf->height);
}

/*
 * graphics_char_width - return the horizontal advance width for a codepoint.
 *
 * Param: codepoint — Unicode scalar value.
 * Returns advance width in pixels from the active font's glyph_info, or 0 if
 * the codepoint is outside [first_char, first_char+num_chars).
 * Used by gl_draw_string and graphics_string_width to advance the cursor.
 *
 * Locking: the active font is read under font_lock (GFX-FONT-01).
 */
/*
 * Get character advance width
 */
int graphics_char_width(uint32_t codepoint) {
  uint64_t flags;
  spin_lock_irqsave(&font_lock, &flags);
  const struct font_state *f = current_font;
  int idx = (int)codepoint - f->header.first_char;
  int adv = (idx < 0 || idx >= f->header.num_chars) ? 0 : f->glyphs[idx].advance;
  spin_unlock_irqrestore(&font_lock, flags);
  return adv;
}

/*
 * gl_draw_string - render a UTF-8 string left-to-right onto surf.
 *
 * Params: surf — target surface; x, y — origin of the first glyph cell;
 *         str — null-terminated UTF-8 string; color — ARGB8888 foreground.
 *
 * Decodes each codepoint via utf8_decode, renders it with gl_draw_char at the
 * current cursor_x, then advances cursor_x by graphics_char_width.  Invalid
 * UTF-8 sequences (consumed <= 0) consume one byte and continue.
 *
 * Locking: none; called under compositor_lock in compositor_render_internal.
 * Side effects: writes pixels to surf->buffer.
 */
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
  size_t rem = 0;
  const char *p = str;
  while (*p) { p++; rem++; }

  while (*str) {
    consumed = utf8_decode(str, rem, &codepoint);
    if (consumed <= 0) {
        str++;
        rem--;
        continue;
    }
    gl_draw_char(surf, cursor_x, y, codepoint, color);
    cursor_x += graphics_char_width(codepoint);
    str += consumed;
    rem -= consumed;
  }
}

void gl_draw_string_clipped(struct gl_surface *surf, int x, int y,
                            const char *str, uint32_t color, int cx1, int cy1,
                            int cx2, int cy2) {
  if (!surf || !str)
    return;

  int cursor_x = x;
  uint32_t codepoint;
  int consumed;
  size_t rem = 0;
  const char *p = str;
  while (*p) { p++; rem++; }

  while (*str) {
    consumed = utf8_decode(str, rem, &codepoint);
    if (consumed <= 0) {
        str++;
        rem--;
        continue;
    }
    gl_draw_char_clipped(surf, cursor_x, y, codepoint, color, cx1, cy1, cx2, cy2);
    cursor_x += graphics_char_width(codepoint);
    str += consumed;
    rem -= consumed;
  }
}

/*
 * graphics_string_width - compute pixel width of a UTF-8 string.
 *
 * Param: str — null-terminated UTF-8 string (NULL-safe: returns 0).
 * Sums graphics_char_width for each decoded codepoint.  Mirrors the cursor
 * advance in gl_draw_string so callers can centre text (e.g. title bar).
 *
 * Locking: the active font is read under font_lock (GFX-FONT-01).
 */
/*
 * Get string width in pixels (UTF-8 supported)
 */
int graphics_string_width(const char *str) {
  if (!str)
    return 0;

  int width = 0;
  uint32_t codepoint;
  int consumed;
  size_t rem = 0;
  const char *p = str;
  while (*p) { p++; rem++; }

  while (*str) {
    consumed = utf8_decode(str, rem, &codepoint);
    if (consumed <= 0) {
        str++;
        rem--;
        continue;
    }
    width += graphics_char_width(codepoint);
    str += consumed;
    rem -= consumed;
  }
  return width;
}

/*
 * graphics_font_height - return the total line height of the active font.
 *
 * Returns ascent + descent in pixels.  Used as char_h in compositor for row
 * count and scroll arithmetic.
 *
 * GFX-FONT-02 (fixed): floors to the built-in default height when
 *   ascent+descent <= 0, so a malformed font (both fields zero, via
 *   sys_set_font) can no longer cause a divide-by-zero at
 *   compositor_create_window (h / char_h).
 *
 * Locking: reads current_font.header under font_lock (GFX-FONT-01).
 */
/*
 * Get font height
 */
int graphics_font_height(void) { 
    /* GFX-FONT-02: floor to the built-in default height so a malformed font
     * (ascent=descent=0 via sys_set_font) cannot make char_h==0 and trigger a
     * divide-by-zero in compositor row/scroll arithmetic — mirrors the
     * graphics_font_max_width() floor. */
    uint64_t flags;
    spin_lock_irqsave(&font_lock, &flags);
    int h = current_font->header.ascent + current_font->header.descent;
    spin_unlock_irqrestore(&font_lock, flags);
    return h > 0 ? h : (FONT_ASCENT + FONT_DESCENT);
}

/*
 * graphics_font_ascent - return the ascent of the active font in pixels.
 *
 * Ascent is the distance from the baseline to the top of the tallest glyph.
 * Used in gl_draw_char to compute start_y = y + ascent + gi->y0.
 *
 * Locking: reads current_font under font_lock (GFX-FONT-01).
 */
/*
 * Get font ascent
 */
int graphics_font_ascent(void) {
    uint64_t flags;
    spin_lock_irqsave(&font_lock, &flags);
    int a = current_font->header.ascent;
    spin_unlock_irqrestore(&font_lock, flags);
    return a;
}

/*
 * Get max character width (for grid systems)
 */
int graphics_font_max_width(void) {
    uint64_t flags;
    spin_lock_irqsave(&font_lock, &flags);
    const struct font_state *f = current_font;
    int max_w = 0;
    for (int i = 0; i < f->header.num_chars; i++) {
        if (f->glyphs[i].advance > max_w)
            max_w = f->glyphs[i].advance;
    }
    spin_unlock_irqrestore(&font_lock, flags);
    return max_w > 0 ? max_w : 8;
}

/*
 * System Call: Set Font
 */
int sys_set_font(void *data, size_t size) {
    /* GFX-FONT-01: 'data' is a raw userland pointer.  Copy the whole blob into a
     * kmalloc'd kernel buffer, validate against the *kernel* copy, then publish an
     * immutable descriptor behind font_lock and retire the previous one.  This
     * removes the use-after-free (the old code stored interior pointers into user
     * memory that dangled after the process exited) and the info-leak (a user
     * could point 'data' at kernel VAs and read them back via the framebuffer). */
    if (!data || size < sizeof(struct font_header))
        return -1;
    if (size > FONT_MAX_BLOB)            /* bound before kmalloc (cf. EXT4-07/#62) */
        return -1;

    /* 1. Copy + validate the header alone first (never deref the user pointer). */
    struct font_header h;
    if (arch_copy_from_user(&h, data, sizeof(h)) != 0)
        return -1;
    if (h.magic != FONT_MAGIC)
        return -2;
    if (h.num_chars == 0 || (uint32_t)h.ascent + (uint32_t)h.descent == 0)
        return -3;                        /* reject zero metrics (cf. GFX-FONT-02) */

    /* num_chars is uint16 and bitmap_size uint32, so this cannot overflow size_t
     * on a 64-bit kernel; FONT_MAX_BLOB bounds the allocation. */
    size_t glyphs_bytes = (size_t)h.num_chars * sizeof(struct font_glyph_info);
    size_t blob = sizeof(struct font_header) + glyphs_bytes + (size_t)h.bitmap_size;
    if (size < blob)
        return -3;

    /* 2. One kmalloc block holds [font_state | header | glyphs | bitmap]; the
     * block base is heap_base so retiring frees everything in one kfree. */
    uint8_t *mem = (uint8_t *)kmalloc(sizeof(struct font_state) + blob);
    if (!mem)
        return -1;
    struct font_state *ns = (struct font_state *)mem;
    uint8_t *kblob = mem + sizeof(struct font_state);
    if (arch_copy_from_user(kblob, data, blob) != 0) {
        kfree(mem);
        return -1;
    }

    /* 3. Re-validate against the kernel copy (defend against a TOCTOU header
     * change between the two copies) and bound every glyph's bitmap span. */
    struct font_header *kh = (struct font_header *)kblob;
    if (kh->magic != FONT_MAGIC || kh->num_chars != h.num_chars ||
        kh->bitmap_size != h.bitmap_size ||
        (uint32_t)kh->ascent + (uint32_t)kh->descent == 0) {
        kfree(mem);
        return -3;
    }
    struct font_glyph_info *kglyphs =
        (struct font_glyph_info *)(kblob + sizeof(struct font_header));
    for (uint16_t i = 0; i < kh->num_chars; i++) {
        uint32_t span = (uint32_t)kglyphs[i].width * (uint32_t)kglyphs[i].height;
        if (kglyphs[i].data_offset > kh->bitmap_size ||
            span > kh->bitmap_size - kglyphs[i].data_offset) {
            kfree(mem);                   /* a glyph would read past the bitmap */
            return -3;
        }
    }

    /* 4. Build the immutable descriptor (pointers reference the kernel blob). */
    ns->header = *kh;
    ns->glyphs = kglyphs;
    ns->bitmap = kblob + sizeof(struct font_header) + glyphs_bytes;
    ns->heap_base = mem;

    /* 5. Publish under the lock and retire the previous descriptor.  set_font
     * holds font_lock to swap, so no reader is mid-descriptor; after the swap no
     * new reader can obtain 'old' (they read the new pointer).  'old' therefore
     * has no users once the lock is released, so it is safe to free.  The static
     * default is never freed. */
    uint64_t flags;
    spin_lock_irqsave(&font_lock, &flags);
    struct font_state *old = current_font;
    current_font = ns;
    spin_unlock_irqrestore(&font_lock, flags);

    if (old != &default_font)
        kfree(old->heap_base);            /* heap_base == old (single block) */

    return 0;
}
