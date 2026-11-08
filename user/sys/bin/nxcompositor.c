/*
 * nxcompositor — ring-3 scene compositor for legacy v1 window clients.
 *
 * Window creation and storage still enter through the v1 compatibility ABI.
 * This process snapshots those surfaces, owns z-order rendering, decoration,
 * scaling and alpha composition, then submits one validated frame to the
 * kernel's display presentation boundary.  The kernel render path is not
 * called by this service.
 */
#include <font_lib.h>
#include <os1.h>
#include <os1_gfx.h>
#include <os1_theme.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define NXCOMPOSITOR_FRAME_MS 33
#define NXCOMPOSITOR_MAX_WINDOWS 40
#define NXCOMPOSITOR_MAX_PIXELS (16u * 1024u * 1024u)
#define IPC_POINTER_POS 0x30
#define IPC_THEME_STYLE 0x31
#define IPC_THEME_BACKGROUND 0x32

static struct font_ctx *compositor_font;
static int compositor_font_loaded;
static int pointer_x = 640;
static int pointer_y = 400;

static void consume_pointer_updates(void) {
  struct ipc_message msg;
  for (unsigned i = 0; i < 32; ++i) {
    if (OS1low_ipc_try_recv(-1, &msg) != 0) break;
    if (msg.type == IPC_POINTER_POS) {
      pointer_x = (int)(uint32_t)msg.data1;
      pointer_y = (int)(uint32_t)msg.data2;
    } else if (msg.type == IPC_THEME_STYLE) {
      (void)os1_theme_set_style((int)(int64_t)msg.data1);
      (void)os1_theme_set_palette((int)(int64_t)msg.data2);
    } else if (msg.type == IPC_THEME_BACKGROUND) {
      (void)os1_theme_set_background((int)(int64_t)msg.data1);
    }
  }
}

static void paint_background(struct os1_gfx_surface *frame,
                             const struct os1_compositor_background *background) {
  uint32_t top = background->bg_top;
  uint32_t bottom = background->bg_bottom;
  for (size_t y = 0; y < frame->height; ++y) {
    uint32_t row_color = top;
    if (frame->height > 1) {
      uint32_t weight = (uint32_t)(y * 255u / (frame->height - 1));
      uint32_t inverse = 255u - weight;
      uint32_t red = (((top >> 16) & 255u) * inverse +
                      ((bottom >> 16) & 255u) * weight) / 255u;
      uint32_t green = (((top >> 8) & 255u) * inverse +
                        ((bottom >> 8) & 255u) * weight) / 255u;
      uint32_t blue = ((top & 255u) * inverse + (bottom & 255u) * weight) / 255u;
      row_color = 0xff000000u | (red << 16) | (green << 8) | blue;
    }
    struct os1_gfx_rect row = {0, (int)y, (int)frame->width, 1};
    os1_gfx_fill_rect(frame, &row, row_color);
  }
}

static void paint_window_chrome(struct os1_gfx_surface *frame,
                                const struct os1_window_snapshot_v1 *window,
                                const struct os1_compositor_style *style,
                                const struct os1_compositor_theme *theme,
                                struct font_ctx *font) {
  if (!style->titlebar || (window->flags & WININFO_TOPMOST))
    return;
  int title_height = style->titlebar_height;
  if (title_height <= 0)
    return;
  int title_y = window->y - title_height;
  uint32_t title_color = (window->flags & WININFO_FOCUSED)
                             ? theme->title_active
                             : theme->title_inactive;
  struct os1_gfx_rect title = {window->x, title_y, window->draw_width,
                               title_height};
  os1_gfx_fill_rect(frame, &title, title_color);

  int close_x = style->button_side == 0
                    ? window->x + 14
                    : window->x + window->draw_width - 14;
  int close_y = title_y + title_height / 2;
  struct os1_gfx_rect close = {close_x - 5, close_y - 5, 10, 10};
  os1_gfx_fill_rect(frame, &close, theme->close_btn);
  for (int d = -2; d <= 2; ++d) {
    struct os1_gfx_rect slash = {close_x + d, close_y + d, 1, 1};
    struct os1_gfx_rect backslash = {close_x + d, close_y - d, 1, 1};
    os1_gfx_fill_rect(frame, &slash, theme->title_text_active);
    os1_gfx_fill_rect(frame, &backslash, theme->title_text_active);
  }

  if (style->window_borders) {
    struct os1_gfx_rect top = {window->x, title_y, window->draw_width, 1};
    struct os1_gfx_rect bottom = {window->x,
                                  window->y + window->draw_height - 1,
                                  window->draw_width, 1};
    struct os1_gfx_rect left = {window->x, title_y, 1,
                                title_height + window->draw_height};
    struct os1_gfx_rect right = {window->x + window->draw_width - 1, title_y,
                                 1, title_height + window->draw_height};
    os1_gfx_fill_rect(frame, &top, theme->border);
    os1_gfx_fill_rect(frame, &bottom, theme->border);
    os1_gfx_fill_rect(frame, &left, theme->border);
    os1_gfx_fill_rect(frame, &right, theme->border);
  }

  if (font && window->title[0])
    font_draw_string_surface(frame, font,
                             window->x + (style->button_side == 0 ? 28 : 12),
                             title_y + 2,
                             window->title,
                             (window->flags & WININFO_FOCUSED)
                                 ? theme->title_text_active
                                 : theme->title_text_inactive);
}

static int snapshot_less(const struct os1_window_snapshot_v1 *a,
                         const struct os1_window_snapshot_v1 *b) {
  if ((a->flags & WININFO_TOPMOST) != (b->flags & WININFO_TOPMOST))
    return (a->flags & WININFO_TOPMOST) == 0;
  if (a->z_order != b->z_order)
    return a->z_order < b->z_order;
  return a->id < b->id;
}

static int render_frame(void) {
  long packed = OS1_display_info();
  int width = (int)((packed >> 16) & 0xffff);
  int height = (int)(packed & 0xffff);
  if (width <= 0 || height <= 0 ||
      (size_t)width > NXCOMPOSITOR_MAX_PIXELS / (size_t)height)
    return -1;
  size_t frame_pixels = (size_t)width * (size_t)height;
  uint32_t *frame_pixels_buf = malloc(frame_pixels * sizeof(uint32_t));
  if (!frame_pixels_buf)
    return -1;
  struct os1_gfx_surface frame = {(uint32_t *)frame_pixels_buf,
                                  (size_t)width, (size_t)height,
                                  (size_t)width, frame_pixels};
  paint_background(&frame, os1_theme_background_active());

  struct window_info legacy[NXCOMPOSITOR_MAX_WINDOWS];
  long count = OS1_window_enum(legacy, NXCOMPOSITOR_MAX_WINDOWS);
  if (count < 0)
    count = 0;
  if (count > NXCOMPOSITOR_MAX_WINDOWS)
    count = NXCOMPOSITOR_MAX_WINDOWS;

  struct os1_window_snapshot_v1 windows[NXCOMPOSITOR_MAX_WINDOWS];
  int window_count = 0;
  for (long i = 0; i < count; ++i) {
    struct os1_window_snapshot_v1 snapshot = {0};
    snapshot.abi_version = OS1_COMPOSITOR_ABI_V1;
    snapshot.struct_size = sizeof(snapshot);
    snapshot.id = legacy[i].id;
    if (snapshot.id <= 0 || OS1_window_snapshot(&snapshot) != 0)
      continue;
    if (!(snapshot.flags & WININFO_VISIBLE) || snapshot.width <= 0 ||
        snapshot.height <= 0 || snapshot.draw_width <= 0 ||
        snapshot.draw_height <= 0 ||
        snapshot.pixel_count > NXCOMPOSITOR_MAX_PIXELS)
      continue;
    windows[window_count++] = snapshot;
  }

  /* Stable insertion sort: low z-order is painted first; topmost policy is
   * applied by the comparison and remains independent from window IDs. */
  for (int i = 1; i < window_count; ++i) {
    struct os1_window_snapshot_v1 key = windows[i];
    int j = i;
    while (j > 0 && snapshot_less(&key, &windows[j - 1])) {
      windows[j] = windows[j - 1];
      --j;
    }
    windows[j] = key;
  }

  const struct os1_compositor_style *style = os1_theme_style_active();
  const struct os1_compositor_theme *theme = os1_theme_palette_active();
  if (!compositor_font_loaded) {
    compositor_font = font_load("/fonts/Rewir-Light.off");
    compositor_font_loaded = 1;
  }
  for (int i = 0; i < window_count; ++i) {
    struct os1_window_snapshot_v1 *window = &windows[i];
    size_t pixel_count = window->pixel_count;
    if (pixel_count == 0 || pixel_count > NXCOMPOSITOR_MAX_PIXELS)
      continue;
    uint32_t *pixels = malloc(pixel_count * sizeof(uint32_t));
    if (!pixels)
      continue;
    window->pixels = pixels;
    window->pixel_capacity = pixel_count;
    if (OS1_window_snapshot(window) == 0) {
      struct os1_gfx_surface source = {pixels, (size_t)window->width,
                                        (size_t)window->height,
                                        (size_t)window->width, pixel_count};
      paint_window_chrome(&frame, window, style, theme, compositor_font);
      os1_gfx_composite_scaled(&frame, &source, window->x, window->y,
                               window->draw_width, window->draw_height);
    }
    free(pixels);
  }
  /* Preserve the original 12x16 monochrome system cursor exactly. */
  static const char *cursor_bits[16] = {
      "X           ", "XX          ", "X.X         ", "X..X        ",
      "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
      "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
      "X.X X..X    ", "XX  X..X    ", "X    XX     ", "     XX     "};
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 12; ++x) {
      int cx = pointer_x + x, cy = pointer_y + y;
      if (cx < 0 || cy < 0 || (size_t)cx >= frame.width ||
          (size_t)cy >= frame.height)
        continue;
      char p = cursor_bits[y][x];
      if (p == 'X') frame.pixels[(size_t)cy * frame.stride + (size_t)cx] = 0xffffffffu;
      else if (p == '.') frame.pixels[(size_t)cy * frame.stride + (size_t)cx] = 0xff000000u;
    }
  }
  struct os1_compositor_present_v1 present = {
      .abi_version = OS1_COMPOSITOR_ABI_V1,
      .struct_size = sizeof(present),
      .width = width,
      .height = height,
      .stride = width,
      .pixels = frame_pixels_buf,
      .pixel_capacity = frame_pixels,
  };
  int rc = (int)OS1_display_present(&present);
  free(frame_pixels_buf);
  return rc;
}

int main(void) {
  print("[nxcompositor] ring-3 scene compositor started\n");
  for (;;) {
    (void)OS1_display_poll();
    consume_pointer_updates();
    (void)render_frame();
    OS1_sleep(NXCOMPOSITOR_FRAME_MS);
  }
}
