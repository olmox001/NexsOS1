/*
 * kernel/graphics/graphics.c
 * Legacy Graphics (Deprecated by HAL+GL+Compositor)
 */
#include <kernel/graphics.h>
#include <kernel/printk.h>

static struct graphics_context g_ctx = {0};

void graphics_init(void) {
  pr_info("%s", "Graphics: Legacy Init (Stub detected).\n");
}

struct graphics_context *graphics_get_context(void) { return &g_ctx; }

void graphics_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
  (void)x;
  (void)y;
  (void)color;
}

void graphics_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t color) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)color;
}

void graphics_clear(uint32_t color) { (void)color; }

void graphics_swap_buffers(void) {}

void graphics_draw_char(uint32_t x, uint32_t y, char c, uint32_t color) {
  (void)x;
  (void)y;
  (void)c;
  (void)color;
}

void graphics_draw_line(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1,
                        uint32_t color) {
  (void)x0;
  (void)x1;
  (void)y0;
  (void)y1;
  (void)color;
}
