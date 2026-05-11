/*
 * kernel/graphics/graphics.c
 * Graphics Subsystem (Bridging HAL and GL)
 */
#include <drivers/gpu/gpu.h>
#include <graphics/gl.h>
#include <kernel/arch.h>
#include <kernel/graphics.h>
#include <kernel/printk.h>

static struct graphics_context g_ctx = {0};

void graphics_init(void) {
  struct gpu_device *dev = gpu_get_primary();
  if (dev) {
    g_ctx.width = dev->width;
    g_ctx.height = dev->height;
    g_ctx.buffer = (uint32_t *)dev->framebuffer_virt;
    pr_info("Graphics: Initialized via HAL (%dx%d)\n", dev->width, dev->height);
  } else {
    pr_err("%s", "Graphics: No GPU device found!\n");
  }
}

struct graphics_context *graphics_get_context(void) { return &g_ctx; }

/* 
 * Helper to get a GL surface wrapping the main screen
 */
struct gl_surface *graphics_get_screen_surface(void) {
  static struct gl_surface screen_surf;
  struct gpu_device *dev = gpu_get_primary();
  if (!dev || !dev->framebuffer_virt) return NULL;
  
  screen_surf.width = dev->width;
  screen_surf.height = dev->height;
  screen_surf.stride = dev->width;
  screen_surf.buffer = (uint32_t *)dev->framebuffer_virt;
  return &screen_surf;
}

void graphics_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
  struct gl_surface *surf = graphics_get_screen_surface();
  if (surf) {
    gl_draw_pixel(surf, (int)x, (int)y, color);
  }
}

void graphics_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t color) {
  struct gl_surface *surf = graphics_get_screen_surface();
  if (surf) {
    gl_draw_rect_fill(surf, (int)x, (int)y, (int)w, (int)h, color);
  }
}

void graphics_clear(uint32_t color) {
  struct gl_surface *surf = graphics_get_screen_surface();
  if (surf) {
    gl_clear(surf, color);
  }
}

void graphics_swap_buffers(void) {
  /* In single-buffered modes, this is a flush/barrier */
  arch_data_barrier();
}

void graphics_draw_char(uint32_t x, uint32_t y, char c, uint32_t color) {
  struct gl_surface *surf = graphics_get_screen_surface();
  if (surf) {
    gl_draw_char(surf, (int)x, (int)y, c, color);
  }
}

void graphics_draw_string(uint32_t x, uint32_t y, const char *str, uint32_t color) {
  struct gl_surface *surf = graphics_get_screen_surface();
  if (surf) {
    gl_draw_string(surf, (int)x, (int)y, str, color);
  }
}
