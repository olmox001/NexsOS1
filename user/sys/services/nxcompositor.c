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
#include <os1_surface.h>
#include <os1_gfx_buffer.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define NXCOMPOSITOR_MAX_WINDOWS 40
#define NXCOMPOSITOR_MAX_PIXELS (16u * 1024u * 1024u)
#define IPC_POINTER_POS 0x30
#define IPC_THEME_STYLE 0x31
#define IPC_THEME_BACKGROUND 0x32

/* The process owns frame/snapshot allocations and device state. No
 * translation-unit mutable storage or cross-service pointer ownership. */
struct compositor_state {
  struct font_ctx *font;
  int font_loaded;
  int pointer_x;
  int pointer_y;
  struct os1_gfx_buffer frame;
  struct os1_gfx_buffer snapshot;
};

static void consume_pointer_updates(struct compositor_state *owner) {
  struct ipc_message msg;
  for (unsigned i = 0; i < 32; ++i) {
    long received = OS1low_ipc_try_recv(-1, &msg);
    if (received == -EAGAIN)
      break;
    if (received != 0) {
      OS1_report_error("nxcompositor.input_receive",
                       (int)(received < 0 ? received : -EIO));
      break;
    }
    if (msg.type == IPC_POINTER_POS) {
      owner->pointer_x = (int)(uint32_t)msg.data1;
      owner->pointer_y = (int)(uint32_t)msg.data2;
    } else if (msg.type == IPC_THEME_STYLE) {
      int style_rc = os1_theme_set_style((int)(int64_t)msg.data1);
      if (style_rc != 0)
        OS1_report_error("nxcompositor.theme_style", style_rc);
      else {
        int palette_rc = os1_theme_set_palette((int)(int64_t)msg.data2);
        if (palette_rc != 0)
          OS1_report_error("nxcompositor.theme_palette", palette_rc);
      }
    } else if (msg.type == IPC_THEME_BACKGROUND) {
      int background_rc = os1_theme_set_background((int)(int64_t)msg.data1);
      if (background_rc != 0)
        OS1_report_error("nxcompositor.theme_background", background_rc);
    }
  }
}

static void
paint_background(struct os1_gfx_surface *frame,
                 const struct os1_compositor_background *background) {
  uint32_t top = background->bg_top;
  uint32_t bottom = background->bg_bottom;
  for (size_t y = 0U; y < NXCOMPOSITOR_MAX_PIXELS; ++y) {
    if (y >= frame->height) break;
    uint32_t row_color = top;
    if (frame->height > 1) {
      uint32_t weight = (uint32_t)(y * 255u / (frame->height - 1));
      uint32_t inverse = 255u - weight;
      uint32_t red =
          (((top >> 16) & 255u) * inverse + ((bottom >> 16) & 255u) * weight) /
          255u;
      uint32_t green =
          (((top >> 8) & 255u) * inverse + ((bottom >> 8) & 255u) * weight) /
          255u;
      uint32_t blue =
          ((top & 255u) * inverse + (bottom & 255u) * weight) / 255u;
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
  if (window->flags & WININFO_TOPMOST) return;
  struct os1_gfx_rect content = {window->x, window->y,
                                window->draw_width, window->draw_height};
  os1_gfx_window_decoration(frame, &content, style, theme,
      (window->flags & WININFO_FOCUSED) != 0,
      (window->flags & WININFO_PROTECTED) != 0);
  if (!style->titlebar || !font || !window->title[0]) return;
  int title_height = style->titlebar_height;
  int64_t top = (int64_t)window->y - title_height;
  int64_t left = (int64_t)window->x + 12;
  int64_t right = (int64_t)window->x + window->draw_width - 12;
  struct os1_gfx_button_geometry buttons;
  os1_gfx_button_geometry(window->x, window->draw_width, (int)top,
      title_height, style->button_shape, style->button_side, &buttons);
  if (!(window->flags & WININFO_PROTECTED) && buttons.size) {
    if (style->button_side == 0) left = buttons.bg_cx + buttons.size / 2 + 8;
    else right = buttons.bg_cx - buttons.size / 2 - 8;
  }
  int64_t bottom = top + title_height - 2;
  int64_t text_x = left, text_y = top + 2;
  if (left < 0) left = 0;
  if (top < 0) top = 0;
  if (right > (int64_t)frame->width) right = (int64_t)frame->width;
  if (bottom > (int64_t)frame->height) bottom = (int64_t)frame->height;
  if (left >= right || top >= bottom) return;
  size_t offset = (size_t)top * frame->stride + (size_t)left;
  /* The view clips even a long UTF-8 title before it reaches the controls. */
  struct os1_gfx_surface label = {frame->pixels + offset,
      (size_t)(right - left), (size_t)(bottom - top), frame->stride,
      frame->capacity - offset};
  font_draw_string_surface(&label, font, (int)(text_x - left),
      (int)(text_y - top), window->title,
      (window->flags & WININFO_FOCUSED) ? theme->title_text_active :
                                        theme->title_text_inactive);
}

static int snapshot_less(const struct os1_window_snapshot_v1 *a,
                         const struct os1_window_snapshot_v1 *b) {
  if ((a->flags & WININFO_TOPMOST) != (b->flags & WININFO_TOPMOST))
    return (a->flags & WININFO_TOPMOST) == 0;
  if (a->z_order != b->z_order)
    return a->z_order < b->z_order;
  return a->id < b->id;
}

static int enumerate_windows(struct os1_window_snapshot_v1 windows[
                                 NXCOMPOSITOR_MAX_WINDOWS]) {
  struct window_info legacy[NXCOMPOSITOR_MAX_WINDOWS];
  long count = OS1_window_enum(legacy, NXCOMPOSITOR_MAX_WINDOWS);
  if (count < 0) return (int)count;
  int found = 0;
  for (int i = 0; i < NXCOMPOSITOR_MAX_WINDOWS; ++i) {
    if ((long)i >= count) break;
    struct os1_window_snapshot_v1 snapshot = {
        .abi_version = OS1_COMPOSITOR_ABI_V1,
        .struct_size = sizeof(snapshot),
        .id = legacy[i].id
    };
    if (snapshot.id <= 0 || OS1_window_snapshot(&snapshot) != 0)
      continue;
    if (!(snapshot.flags & WININFO_VISIBLE) || snapshot.width <= 0 ||
        snapshot.height <= 0 || snapshot.draw_width <= 0 ||
        snapshot.draw_height <= 0 ||
        snapshot.pixel_count > NXCOMPOSITOR_MAX_PIXELS)
      continue;
    windows[found++] = snapshot;
  }
  return found;
}

static void sort_windows(struct os1_window_snapshot_v1 windows[
                             NXCOMPOSITOR_MAX_WINDOWS], int count) {
  for (int i = 1; i < NXCOMPOSITOR_MAX_WINDOWS; ++i) {
    if (i >= count) break;
    struct os1_window_snapshot_v1 key = windows[i];
    int j = i;
    for (int step = 0; step < NXCOMPOSITOR_MAX_WINDOWS; ++step) {
      if (j == 0 || !snapshot_less(&key, &windows[j - 1])) break;
      windows[j] = windows[j - 1];
      --j;
    }
    windows[j] = key;
  }
}

static int draw_windows(struct compositor_state *owner,
                        struct os1_gfx_surface *frame) {
  struct os1_window_snapshot_v1 windows[NXCOMPOSITOR_MAX_WINDOWS];
  int count = enumerate_windows(windows);
  if (count < 0) return count;
  sort_windows(windows, count);
  const struct os1_compositor_style *style = os1_theme_style_active();
  const struct os1_compositor_theme *theme = os1_theme_palette_active();
  if (!owner->font_loaded) {
    owner->font = font_load("/fonts/Rewir-Light.off");
    owner->font_loaded = 1;
  }
  for (int i = 0; i < NXCOMPOSITOR_MAX_WINDOWS; ++i) {
    if (i >= count) break;
    struct os1_window_snapshot_v1 *window = &windows[i];
    size_t pixels = window->pixel_count;
    if (pixels == 0U || pixels > NXCOMPOSITOR_MAX_PIXELS) continue;
    int rc = OS1_gfx_buffer_reserve(&owner->snapshot, pixels);
    if (rc != 0) return rc;
    window->pixels = owner->snapshot.active.view.address;
    window->pixel_capacity = pixels;
    if (OS1_window_snapshot(window) != 0) continue;
    struct os1_gfx_surface source = {
        owner->snapshot.active.view.address, (size_t)window->width,
        (size_t)window->height, (size_t)window->width, pixels};
    struct os1_gfx_rect content = {
        window->x, window->y, window->draw_width, window->draw_height};
    int decorated = !(window->flags & WININFO_TOPMOST);
    int title = decorated && style->titlebar ? style->titlebar_height : 0;
    int radius = decorated && style->rounded_corners ? style->border_radius : 0;
    os1_gfx_composite_rounded(frame, &source, &content, title, radius);
    paint_window_chrome(frame, window, style, theme, owner->font);
  }
  return 0;
}

/* Unchanged 12x16 monochrome system cursor, clipped to the display. */
static void draw_cursor(const struct compositor_state *owner,
                        struct os1_gfx_surface *frame) {
  static const char *cursor_bits[16] = {
      "X           ", "XX          ", "X.X         ", "X..X        ",
      "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
      "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
      "X.X X..X    ", "XX  X..X    ", "X    XX     ", "     XX     "};
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 12; ++x) {
      int cx = owner->pointer_x + x, cy = owner->pointer_y + y;
      if (cx < 0 || cy < 0 || (size_t)cx >= frame->width ||
          (size_t)cy >= frame->height) continue;
      char pixel = cursor_bits[y][x];
      if (pixel == 'X')
        frame->pixels[(size_t)cy * frame->stride + (size_t)cx] = 0xffffffffu;
      else if (pixel == '.')
        frame->pixels[(size_t)cy * frame->stride + (size_t)cx] = 0xff000000u;
    }
  }
}

static int render_frame(struct compositor_state *owner) {
  long packed = OS1_display_info();
  int width = (int)((packed >> 16) & 0xffff);
  int height = (int)(packed & 0xffff);
  if (width <= 0 || height <= 0 ||
      (size_t)width > NXCOMPOSITOR_MAX_PIXELS / (size_t)height)
    return -EINVAL;
  size_t total = (size_t)width * (size_t)height;
  int allocated = OS1_gfx_buffer_reserve(&owner->frame, total);
  if (allocated != 0) return allocated;
  struct os1_gfx_surface frame = {
      owner->frame.active.view.address, (size_t)width, (size_t)height,
      (size_t)width, total};
  paint_background(&frame, os1_theme_background_active());
  int drawn = draw_windows(owner, &frame);
  if (drawn != 0) return drawn;
  draw_cursor(owner, &frame);
  struct os1_compositor_present_v1 present = {
      .abi_version = OS1_COMPOSITOR_ABI_V1,
      .struct_size = sizeof(present),
      .width = width, .height = height, .stride = width,
      .pixels = owner->frame.active.view.address, .pixel_capacity = total
  };
  return (int)OS1_display_present(&present);
}

int main(void) {
  struct compositor_state owner = {.pointer_x = 640, .pointer_y = 400};
  OS1_gfx_buffer_init(&owner.frame);
  OS1_gfx_buffer_init(&owner.snapshot);
  long events = OS1_surface_events_open(OS1_RIGHT_READ | OS1_RIGHT_WAIT);
  if (events < 0) {
    OS1_report_error("nxcompositor.surface_events_open", (int)events);
    return 1;
  }
  for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
    struct os1_surface_state_v1 state;
    long current = OS1_surface_state((int)events, &state);
    if (current != 0) {
      OS1_report_error("nxcompositor.surface_state",
                       (int)(current < 0 ? current : -EIO));
      break;
    }
    consume_pointer_updates(&owner);
    int rendered = render_frame(&owner);
    if (rendered != 0) {
      OS1_report_error("nxcompositor.render_frame", rendered);
      break;
    }
    long waited = OS1_surface_wait((int)events, state.generation);
    if (waited != 0) {
      OS1_report_error("nxcompositor.surface_wait",
                       (int)(waited < 0 ? waited : -EIO));
      break;
    }
  }
  /* Retain explicit mapping/handle ownership through error cleanup. The
   * provider also detaches outstanding mappings when this process exits. */
  int snapshot_closed = OS1_gfx_buffer_release(&owner.snapshot);
  int frame_closed = OS1_gfx_buffer_release(&owner.frame);
  if (snapshot_closed != 0)
    OS1_report_error("nxcompositor.snapshot_release", snapshot_closed);
  if (frame_closed != 0)
    OS1_report_error("nxcompositor.frame_release", frame_closed);
  long closed = OS1low_handle_close((int)events);
  if (closed != 0)
    OS1_report_error("nxcompositor.events_close",
                     (int)(closed < 0 ? closed : -EIO));
  return 1;
}
