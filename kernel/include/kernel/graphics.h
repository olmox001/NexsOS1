#ifndef _KERNEL_GRAPHICS_H
#define _KERNEL_GRAPHICS_H

#include <kernel/types.h>
#include <stdint.h>

/* 3D Math Types */
typedef struct {
  float x, y, z, w;
} vec4_t;

typedef struct {
  float m[4][4];
} mat4_t;

struct graphics_context {
  uint32_t *buffer;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t pitch;
  uint32_t bpp;
  uint32_t stride;
};

void graphics_init(void);
struct graphics_context *graphics_get_context(void);
struct gl_surface *graphics_get_screen_surface(void);
void graphics_swap_buffers(void);
void graphics_draw_pixel(uint32_t x, uint32_t y, uint32_t color);
void graphics_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t color);
void graphics_draw_char(uint32_t x, uint32_t y, uint32_t codepoint, uint32_t color);
void graphics_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                        uint32_t color);
void graphics_clear(uint32_t color);

/* Font/String API */
int graphics_char_width(uint32_t codepoint);
int graphics_string_width(const char *str);
int utf8_decode(const char *s, uint32_t *code);
int graphics_font_height(void);
int graphics_font_ascent(void);
int graphics_font_max_width(void);
void graphics_draw_string(uint32_t x, uint32_t y, const char *str,
                          uint32_t color);
int sys_set_font(void *data, size_t size);

/* 3D Renderer API */
void render3d_init(uint32_t width, uint32_t height);
void render3d_clear_zbuffer(void);

/* Compositor API */
void compositor_init(void);
int compositor_create_window(int x, int y, int w, int h, const char *title,
                             int pid);
void compositor_destroy_window(int window_id);
/* compositor_window_owner: owning PID of a window id, -1 if not found.
 * Used by the SYS_DESTROY_WINDOW capability check (ABI-04). */
int compositor_window_owner(int window_id);
uint32_t *compositor_get_buffer(int window_id);
void compositor_move_window(int window_id, int x, int y);
void compositor_render(void);
void compositor_handle_click(int button, int state);
void compositor_update_mouse(int dx, int dy, int absolute);
void compositor_window_write(int win_id, const char *buf, size_t count);

/* Protected drawing (with PID check) */
void compositor_draw_rect(int window_id, int x, int y, int w, int h,
                          uint32_t color, int caller_pid);
void compositor_blit(int window_id, int x, int y, int w, int h,
                     const uint32_t *user_buf, int caller_pid);
void compositor_set_window_flags(int window_id, int flags);

/* Process/System API */
void compositor_destroy_windows_by_pid(int pid);
int compositor_get_window_by_pid(int pid);
int compositor_get_focus_pid(void);
void compositor_tick(void);

#endif /* _KERNEL_GRAPHICS_H */
