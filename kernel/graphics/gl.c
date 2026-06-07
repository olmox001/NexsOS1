/*
 * kernel/graphics/gl.c
 * 2D Software Rasteriser (GL Surface Primitives)
 *
 * Role:
 *   Provides pixel-level drawing primitives that operate on struct gl_surface
 *   (an in-memory ARGB8888 pixel buffer with explicit width/height/stride).
 *   This is the 2D software rasteriser — NOT the project's 3D engine, which
 *   lives in the out-of-scope draw3d.c.
 *
 * Primitives:
 *   gl_clear          — flood-fill entire surface with a solid colour.
 *   gl_draw_pixel     — write one pixel with bounds check.
 *   gl_draw_line      — Bresenham integer line (delegates to gl_draw_pixel).
 *   gl_draw_rect_fill — filled axis-aligned rectangle with clip-to-surface.
 *   gl_blit           — composite one surface onto another at (dx, dy), with
 *                       per-pixel Porter-Duff "src over" alpha blending.
 *
 * Pixel format:
 *   All surfaces use ARGB8888 packed as 0xAARRGGBB in a uint32_t.
 *   Stride may exceed width (pitch-aligned framebuffer); row address =
 *   buffer + row * stride.
 *
 * Alpha blending model:
 *   blend_over() implements non-pre-multiplied "src over" destination:
 *     out_channel = (src_channel * a + dst_channel * (255-a)) / 255
 *   Output alpha is forced to 0xFF (fully opaque destination).
 *   gl_blit uses a three-way fast path: skip (a=0), copy (a=255), blend.
 *
 * Locking & IRQ context:
 *   None of these functions take any lock.  They are safe to call with
 *   compositor_lock held (as compositor_render_internal does), but they must
 *   not be called from hard-IRQ context without that protection.
 *
 * Known issues:
 *   GFX-GL-01 (W1 MISSING) gl_draw_rect (unfilled outline rectangle) and
 *              gl_swizzle_bgr are declared in <graphics/gl.h> but have no
 *              implementation here.  No kernel C caller currently exists, so
 *              there is no link error, but any future caller will break the
 *              link step.
 */
#include <graphics/gl.h>
#include <kernel/string.h>
#include <kernel/types.h>

/*
 * clip - clamp an integer value to [min, max] inclusive.
 * Used by gl_draw_rect_fill to intersect requested rect with surface bounds.
 */
/* Helper for clipping */
static inline int clip(int val, int min, int max) {
  if (val < min)
    return min;
  if (val > max)
    return max;
  return val;
}

/*
 * blend_over - Porter-Duff "src over" composite (non-pre-multiplied ARGB8888).
 *
 * Params: src — source pixel (ARGB8888); dst — destination pixel (ARGB8888).
 * Returns: blended pixel with alpha forced to 0xFF (opaque destination).
 *
 * Formula per channel: out = (src_ch * a + dst_ch * (255 - a)) / 255
 * Division by 255 is exact integer (not approximated by >>8, which would
 * introduce a systematic -1 error for fully saturated channels).
 *
 * Alpha blend: composite src (ARGB8888) over opaque dst.
 * Standard Porter-Duff "src over" for pre-multiplied-free ARGB:
 *   out = (src * a + dst * (255 - a)) / 255
 * Inline so the compiler can hoist it into the blit hot loop.
 */
static inline uint32_t blend_over(uint32_t src, uint32_t dst) {
  uint32_t a = (src >> 24) & 0xFF;
  uint32_t ia = 255 - a;

  uint32_t r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
  uint32_t g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
  uint32_t b = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;

  return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/*
 * gl_clear - fill an entire gl_surface with a solid colour.
 *
 * Params: surf — target surface; color — ARGB8888 fill value.
 * Writes stride * height pixels, covering the full allocation including any
 * pitch-padding columns between rows (important for framebuffers where
 * stride > width; a previous version used width*height and missed padding).
 *
 * Side effects: overwrites every word in surf->buffer.
 * Locking: none.
 */
void gl_clear(struct gl_surface *surf, uint32_t color) {
  if (!surf || !surf->buffer)
    return;

  /*
   * BUG FIX (minor): original used width*height, ignoring stride.
   * If stride > width (e.g. pitch-aligned framebuffer) the padding columns
   * between rows were never cleared, leaving stale data visible.
   * Use stride*height to cover the full allocation.
   */
  int size = surf->stride * surf->height;
  for (int i = 0; i < size; i++)
    surf->buffer[i] = color;
}

/*
 * gl_draw_pixel - write one pixel to a gl_surface with bounds checking.
 *
 * Params: surf — target surface; x, y — pixel coordinates; color — ARGB8888.
 * Silently returns if surf/buffer is NULL or (x, y) is outside [0, width) x
 * [0, height).  Row addressing uses stride (not width) for pitch-aligned
 * framebuffers: pixel address = buffer + y * stride + x.
 *
 * Side effects: writes one uint32_t to surf->buffer.
 * Locking: none.
 */
void gl_draw_pixel(struct gl_surface *surf, int x, int y, uint32_t color) {
  if (!surf || !surf->buffer)
    return;
  if (x < 0 || x >= surf->width || y < 0 || y >= surf->height)
    return;
  surf->buffer[y * surf->stride + x] = color;
}

/*
 * gl_draw_line - draw an antialiasing-free line using Bresenham's algorithm.
 *
 * Params: surf — target surface; (x0, y0) start, (x1, y1) end; color ARGB8888.
 * Uses integer error accumulation (classic Bresenham): dx, dy are absolute
 * deltas; sx, sy are the step signs; err drives the major-axis advance.
 * Each pixel is written via gl_draw_pixel which bounds-checks individually,
 * so clipping to surface bounds is handled pixel-by-pixel.
 *
 * Side effects: writes pixels to surf->buffer via gl_draw_pixel.
 * Locking: none.
 */
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

/*
 * gl_draw_rect_fill - draw a solid axis-aligned rectangle clipped to surface.
 *
 * Params: surf — target surface; x, y — top-left; w, h — dimensions;
 *         color — ARGB8888 fill.
 *
 * Clip math: early-reject if entirely off-surface (x >= width, y >= height,
 * or right/bottom edge <= 0), then clip each corner with clip():
 *   cx  = clip(x,     0, width-1)   — left edge
 *   cy  = clip(y,     0, height-1)  — top edge
 *   x2  = clip(x+w,  0, width)      — right edge (exclusive)
 *   y2  = clip(y+h,  0, height)     — bottom edge (exclusive)
 * The inner loops then run j in [cy, y2) and i in [cx, x2) with no further
 * bounds check — safety is guaranteed by the clip.
 *
 * Side effects: writes pixels to surf->buffer.
 * Locking: none.
 */
void gl_draw_rect_fill(struct gl_surface *surf, int x, int y, int w, int h,
                       uint32_t color) {
  if (!surf)
    return;
  if (x >= surf->width || y >= surf->height)
    return;
  if (x + w < 0 || y + h < 0)
    return;

  int cx = clip(x, 0, surf->width - 1);
  int cy = clip(y, 0, surf->height - 1);
  int x2 = clip(x + w, 0, surf->width);
  int y2 = clip(y + h, 0, surf->height);

  for (int j = cy; j < y2; j++)
    for (int i = cx; i < x2; i++)
      surf->buffer[j * surf->stride + i] = color;
}

/*
 * gl_blit - composite a source gl_surface onto a destination at (dx, dy).
 *
 * Params: dst — destination surface; src — source surface; dx, dy — top-left
 *         offset in dst where src[0,0] is placed.
 *
 * Clips src pixels to dst bounds per-pixel (negative dx/dy handled by
 * continue when dsty<0 or dstx<0; right/bottom handled by >= checks).
 *
 * Three-way alpha path (keyed on extracted alpha byte of each source pixel):
 *   alpha == 0:   skip — fully transparent, no write.
 *   alpha == 255: fast copy — no blend arithmetic, direct assignment.
 *   otherwise:    blend_over() — Porter-Duff "src over".
 *
 * Side effects: writes pixels to dst->buffer.
 * Locking: none; called under compositor_lock in compositor_render_internal.
 *
 * gl_blit — composite src surface onto dst at (dx, dy).
 *
 * BUG FIX (critical): The original implementation performed only an alpha
 * TEST: any pixel with alpha == 0 was skipped, and every other pixel —
 * regardless of its actual alpha value — was written at full opacity with
 * gl_draw_pixel().  Semi-transparent pixels (0 < alpha < 255) were therefore
 * composited as if they were completely opaque, corrupting the destination
 * with raw source colours instead of a proper blend.  This is the root cause
 * of the multicoloured artefact visible in the compositor output.
 *
 * Fix: three-way path keyed on the source alpha:
 *   alpha == 0   → skip (fully transparent, nothing to draw)
 *   alpha == 255 → fast copy (fully opaque, no arithmetic needed)
 *   otherwise    → Porter-Duff "src over" blend with the destination pixel
 */
void gl_blit(struct gl_surface *dst, struct gl_surface *src, int dx, int dy) {
  if (!dst || !src || !dst->buffer || !src->buffer)
    return;

  for (int y = 0; y < src->height; y++) {
    int dsty = dy + y;
    if (dsty < 0 || dsty >= dst->height)
      continue;

    for (int x = 0; x < src->width; x++) {
      int dstx = dx + x;
      if (dstx < 0 || dstx >= dst->width)
        continue;

      uint32_t col = src->buffer[y * src->stride + x];
      uint32_t alpha = col >> 24;

      if (alpha == 0)
        continue; /* fully transparent — nothing to do */

      uint32_t *dp = &dst->buffer[dsty * dst->stride + dstx];

      if (alpha == 255) {
        *dp = col; /* fully opaque — fast path, no blend math */
      } else {
        *dp = blend_over(col, *dp); /* semi-transparent — proper alpha blend */
      }
    }
  }
}