/*
 * kernel/graphics/graphics.c
 * Graphics Subsystem (Bridging HAL and GL)
 *
 * Role:
 *   Thin HAL bridge between the GPU device driver and the GL rasteriser.
 *   graphics_screen_surface() is THE single accessor to the scanout buffer
 *   (S-ALIGN F7): it goes through the gpu_ops.get_framebuffer contract — the
 *   same method the compositor flush path consumes — so no caller knows which
 *   provider implementation sits underneath (ASTRA §1 rule 1/2).  The old
 *   graphics_get_screen_surface() read dev->framebuffer_virt directly (a
 *   second, field-level path to the same memory) and returned a function-
 *   static struct (GFX-GFX-01, SMP-unsafe) — both retired.
 *
 * Invariants:
 *   - All draw wrappers silently no-op if no GPU/framebuffer is present.
 *   - graphics_swap_buffers() is a memory-barrier-only stub; the subsystem
 *     operates in single-buffered mode (backbuffer lives in compositor.c).
 */
#include <drivers/gpu/gpu.h>
#include <graphics/gl.h>
#include <kernel/arch.h>
#include <kernel/fault.h>
#include <kernel/gfx_surface.h>
#include <kernel/graphics.h>
#include <kernel/printk.h>

/*
 * graphics_init - verify the primary GPU is discoverable through the HAL.
 *
 * Pure discovery log: the scanout is always reached per-call through
 * graphics_screen_surface(), never cached at init (a cached copy went stale
 * on every runtime resolution change — the retired g_ctx bug).
 */
void graphics_init(void) {
  struct gpu_device *dev = gpu_get_primary();
  if (dev)
    pr_info("Graphics: Initialized via HAL (%dx%d)\n", dev->width, dev->height);
  else
    pr_err("%s", "Graphics: No GPU device found!\n");
}

/*
 * graphics_screen_surface - fill 'out' with a gl_surface over the scanout.
 *
 * THE single accessor to the screen buffer (S-ALIGN F7): resolves the primary
 * GPU per call (picks up runtime mode changes) and reads the buffer through
 * the gpu_ops.get_framebuffer contract — the same method the compositor's
 * flush path uses.  The caller owns 'out' (no static: SMP- and fault-safe,
 * closes GFX-GFX-01).  vgpu_get_framebuffer is lock-free and NULL-safe, so
 * this path is safe from panic context (panic_screen below).
 *
 * Returns 0 on success, -1 if no GPU/framebuffer is present.
 */
int graphics_screen_surface(struct gl_surface *out) {
  struct gpu_device *dev = gpu_get_primary();
  if (!out || !dev || !dev->ops || !dev->ops->get_framebuffer)
    return -1;
  uint32_t *fb = (uint32_t *)dev->ops->get_framebuffer(dev, NULL);
  if (!fb)
    return -1;
  out->width = dev->width;
  out->height = dev->height;
  out->stride = dev->width;
  out->buffer = fb;
  /* S-STAB: the scanout's true size in pixels.  If a concurrent mode change
   * (vgpu_set_mode) left width/height and framebuffer_size momentarily torn —
   * the compositor↔GPU seam is unlocked here (S-ALIGN F7) — this makes the
   * inconsistency a loud, localized panic instead of an OOB blit into the
   * freed/short scanout backing. */
  out->capacity = dev->framebuffer_size / sizeof(uint32_t);
  gfx_surface_verify(out, "graphics_screen_surface");
  return 0;
}

/*
 * graphics_draw_pixel - write a single pixel to the screen framebuffer.
 *
 * Params: x, y — screen coordinates (clamped/rejected by gl_draw_pixel if
 *         out of surface bounds).  color — ARGB8888.
 * Side effects: writes one pixel to the HAL framebuffer via gl_draw_pixel.
 * Locking: none; wraps gl_draw_pixel which is not IRQ-safe.
 */
void graphics_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) == 0) {
    struct gl_surface *surf = &screen;
    gl_draw_pixel(surf, (int)x, (int)y, color);
  }
}

/*
 * graphics_draw_rect - fill an axis-aligned rectangle on the screen.
 *
 * Params: x, y — top-left corner; w, h — dimensions; color — ARGB8888.
 * Clips to screen surface bounds via gl_draw_rect_fill.
 * Side effects: writes pixels to the HAL framebuffer.
 * Locking: none.
 */
void graphics_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) == 0) {
    gfx_rect_t rect = {(int)x, (int)y, (int)w, (int)h};
    gfx_surface_fill(&screen, &rect, color);
  }
}

/*
 * graphics_clear - fill the entire screen with a solid colour.
 *
 * Param: color — ARGB8888 fill value.
 * Side effects: overwrites every pixel in the HAL framebuffer (stride*height
 *               words) via gl_clear.
 * Locking: none.
 */
void graphics_clear(uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) == 0)
    gfx_surface_clear(&screen, color);
}

/*
 * graphics_swap_buffers - issue a full memory barrier to commit pending writes.
 *
 * The system runs in single-buffered mode: there is no second framebuffer to
 * swap.  This function exists as an API hook for future double-buffered
 * support. Currently it only issues arch_mb() to ensure all preceding pixel
 * writes are visible to the GPU DMA engine before any following flush.
 *
 * Side effects: arch_mb() — a full store/load barrier on the current arch.
 * Locking: none required; barrier is inherently CPU-local.
 */
void graphics_swap_buffers(void) {
  /* In single-buffered modes, this is a flush/barrier */
  arch_mb();
}

/*
 * panic_screen - paint the captured fault transcript on a RED framebuffer.
 *
 * DIR-05 / #139: a kernel fault halts everything and is otherwise visible only
 * on the UART.  This makes it readable on the display too, for machines with no
 * serial.  Fault-safe by construction: it does NOT touch the compositor (which
 * may be wedged or mid-render), takes no locks of its own, writes the primary
 * GPU framebuffer directly, and presents through the device flush op (which
 * skips its own lock in fault context).  Best-effort: returns quietly if no GPU
 * / framebuffer is present.  Called from panic() after the other CPUs quiesce.
 */
void panic_screen(const char *text) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) != 0)
    return;
  struct gl_surface *s = &screen;
  if (!s->buffer || s->width <= 0 || s->height <= 0)
    return;

  const uint32_t RED = 0xFFB91C1C, WHITE = 0xFFFCFCFD;
  uint32_t npix = (uint32_t)s->width * (uint32_t)s->height;
  for (uint32_t i = 0; i < npix; i++)
    s->buffer[i] = RED;

  gl_draw_string(s, 12, 8, "*** KERNEL PANIC - system halted ***", WHITE);

  /* The fault transcript, line by line, clipped to the screen.  The top lines
   * (arch register dump + fault class) are the most diagnostic, so render from
   * the top. */
  int y = 32;
  char line[200];
  for (const char *p = text ? text : ""; *p && y < (int)s->height - 16;) {
    int k = 0;
    while (*p && *p != '\n' && k < (int)sizeof(line) - 1)
      line[k++] = *p++;
    line[k] = '\0';
    if (*p == '\n')
      p++;
    if (k > 0)
      gl_draw_string(s, 12, y, line, WHITE);
    y += 16;
  }

  struct gpu_device *dev = gpu_get_primary();
  if (dev && dev->ops && dev->ops->flush)
    dev->ops->flush(dev, 0, 0, dev->width, dev->height);
}

/*
 * graphics_draw_char - render one Unicode codepoint to the screen framebuffer.
 *
 * Params: x, y — baseline-relative top-left; codepoint — Unicode scalar value;
 *         color — ARGB8888 foreground.
 * Delegates to gl_draw_char (font.c) which performs per-glyph alpha blending.
 * Side effects: writes pixels to the HAL framebuffer.
 * Locking: none; must not be called from IRQ without holding compositor_lock.
 */
void graphics_draw_char(uint32_t x, uint32_t y, uint32_t codepoint,
                        uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) == 0) {
    struct gl_surface *surf = &screen;
    gl_draw_char(surf, (int)x, (int)y, codepoint, color);
  }
}

/*
 * graphics_draw_string - render a UTF-8 string to the screen framebuffer.
 *
 * Params: x, y — origin; str — null-terminated UTF-8 string; color — ARGB8888.
 * Delegates to gl_draw_string (font.c) which advances the cursor by each
 * glyph's advance width after calling gl_draw_char.
 * Side effects: writes pixels to the HAL framebuffer.
 * Locking: none; must not be called from IRQ without holding compositor_lock.
 */
void graphics_draw_string(uint32_t x, uint32_t y, const char *str,
                          uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) == 0) {
    struct gl_surface *surf = &screen;
    gl_draw_string(surf, (int)x, (int)y, str, color);
  }
}
