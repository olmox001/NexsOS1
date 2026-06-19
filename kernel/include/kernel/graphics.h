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
/* compositor_resize: retarget the desktop/backbuffer to w x h (GFX-DYN-01).
 * Allocation-free (capacity pre-allocated at init) so it is safe from any
 * context.  The caller must have already set the GPU scanout to the same size
 * (gpu_set_mode) so the next flush strides match. */
void compositor_resize(int w, int h);
/* compositor_get_size: current desktop (backbuffer) size. Backs SYS_DISPLAY_INFO. */
void compositor_get_size(int *w, int *h);
/* compositor_set_zoom: desktop zoom percent (HiDPI, F2). Resizes the GPU scanout
 * to native*100/percent; QEMU stretches it to the host window. 0/-1. */
int compositor_set_zoom(int percent);
/* compositor_set_native_mode: record a real resolution change as the zoom-100
 * reference (resets zoom). Call alongside gpu_set_mode + compositor_resize. */
void compositor_set_native_mode(int w, int h);
int compositor_create_window(int x, int y, int w, int h, const char *title,
                             int pid);
void compositor_destroy_window(int window_id);
/* compositor_window_owner: owning PID of a window id, -1 if not found.
 * Used by the SYS_DESTROY_WINDOW capability check (ABI-04). */
int compositor_window_owner(int window_id);
/* compositor_window_grid: terminal grid (cols x rows) of a window; 0 on
 * success (fills cols/rows), -1 if the id is unknown.  Backs SYS_WINDOW_GRID. */
int compositor_window_grid(int window_id, int *cols, int *rows);
/* compositor_focus_changed: erase the terminal caret off windows that no
 * longer own keyboard focus (new_pid is the new focus owner).  Called by
 * SYS_SET_FOCUS so the caret tracks the window the user types into. */
void compositor_focus_changed(int new_pid);
uint32_t *compositor_get_buffer(int window_id);
void compositor_move_window(int window_id, int x, int y);
/* compositor_resize_window: resize a window's logical surface to w x h
 * (reallocates the buffer, reflows the terminal).  Process context only.
 * Returns 0 on success, -1 on failure.  Backs SYS_WINDOW_RESIZE (GFX-DYN-01). */
int compositor_resize_window(int window_id, int w, int h);
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

/* Process/System seam (DIR-02).
 *
 * The compositor↔scheduler dependency is inverted: the scheduler owns the
 * keyboard-focus hint (keyboard_focus_pid) and the compositor only *pushes* it
 * down via sched_set_focus_pid() (#67/#83) — schedule() never calls back into
 * the compositor.  The remaining PID↔window relation is kept explicit here:
 *   - compositor_destroy_windows_by_pid(): process teardown closes its windows.
 *   - compositor_get_window_by_pid(): the one PID→primary-window lookup.
 * compositor_get_focus_pid() was removed (dead after SCHED-01): focus is read
 * from the scheduler's published hint, not queried from the compositor. */
void compositor_destroy_windows_by_pid(int pid);
int compositor_get_window_by_pid(int pid);
/* Alias documenting the relation as window-centric (DIR-02): the primary
 * window owned by a process, or -1 if it has none. */
static inline int compositor_primary_window_of_pid(int pid) {
  return compositor_get_window_by_pid(pid);
}
void compositor_tick(void);

#endif /* _KERNEL_GRAPHICS_H */
