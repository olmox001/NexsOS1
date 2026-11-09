/* nxmoused: ring-3 pointer aggregation, hit testing, focus and capture. */
#include <os1_surface.h>
#include <os1.h>
#include <os1_gfx.h>
#include <os1_input.h>
#include <os1_theme.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define EV_SYN 0
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8
#define ABS_X 0
#define ABS_Y 1
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_TOUCH 0x14a
#define IPC_POINTER_POS 0x30
#define IPC_THEME_STYLE 0x31
#define IPC_THEME_BACKGROUND 0x32
#define MAX_WINDOWS 40

static int px = 400, py = 300, dragging, drag_dx, drag_dy;
static int resizing, resize_x, resize_y, resize_w, resize_h;
static int buttons[3], capture[3];
static uint8_t device_buttons[256][3];
static uint8_t pending_button[256][3], pending_value[256][3];
static unsigned button_holders[3];
static int frame_motion, frame_wheel;

static int topmost(const struct os1_window_snapshot_v1 *a,
                   const struct os1_window_snapshot_v1 *b) {
  if ((a->flags & WININFO_TOPMOST) != (b->flags & WININFO_TOPMOST))
    return (a->flags & WININFO_TOPMOST) != 0;
  if (a->z_order != b->z_order) return a->z_order > b->z_order;
  return a->id > b->id;
}

static int titlebar_height(void) {
  const struct os1_compositor_style *style = os1_theme_style_active();
  return style && style->titlebar ? style->titlebar_height : 0;
}

static int hit_test(int x, int y, struct os1_window_snapshot_v1 *result) {
  struct window_info list[MAX_WINDOWS];
  long n = OS1_window_enum(list, MAX_WINDOWS);
  int found = 0;
  struct os1_window_snapshot_v1 best = {0};
  for (long i = 0; i < n && i < MAX_WINDOWS; ++i) {
    struct os1_window_snapshot_v1 s = {
      .abi_version = OS1_COMPOSITOR_ABI_V1,
      .struct_size = sizeof(s), .id = list[i].id
    };
    if (OS1_window_snapshot(&s) != 0 || !(s.flags & WININFO_VISIBLE) ||
        (s.flags & WININFO_PASSIVE)) continue;
    int title = (s.flags & WININFO_TOPMOST) ? 0 : titlebar_height();
    if (x < s.x || x >= s.x + s.draw_width || y < s.y - title ||
        y >= s.y + s.draw_height) continue;
    if (!found || topmost(&s, &best)) { best = s; found = 1; }
  }
  if (found && result) *result = best;
  return found;
}

static void send_mouse(const struct os1_window_snapshot_v1 *w,
                       unsigned button, unsigned state) {
  if (!w || w->pid <= 0) return;
  struct ipc_message m = {0};
  m.type = IPC_TYPE_MOUSE;
  m.data1 = button;
  m.data2 = state;
  int32_t x = px - w->x, y = py - w->y;
  memcpy(m.payload, &x, sizeof(x));
  memcpy(m.payload + sizeof(x), &y, sizeof(y));
  (void)OS1low_ipc_send(w->pid, &m);
}

static void publish_pointer(void) {
  char text[20];
  long pid = OS1_registry_get("srv.compositor_pid", text, sizeof(text));
  if (pid != 0) return;
  int to = atoi(text);
  if (to <= 0) return;
  struct ipc_message m = {0};
  m.type = IPC_POINTER_POS;
  m.data1 = (uint32_t)px;
  m.data2 = (uint32_t)py;
  if (OS1low_ipc_send(to, &m) == 0) (void)OS1_surface_invalidate();
}

static void clamp_pointer(void) {
  long d = OS1_display_info();
  int w = (int)((d >> 16) & 0xffff), h = (int)(d & 0xffff);
  if (w > 0 && px >= w) px = w - 1;
  if (h > 0 && py >= h) py = h - 1;
  if (px < 0) px = 0;
  if (py < 0) py = 0;
}

static void button_event(unsigned code, int value) {
  int b = code == BTN_LEFT ? 0 : code == BTN_RIGHT ? 1 : 2;
  /* PS/2 repeats the full button state on every packet. Only transitions are
   * clicks; repeated held-state reports must not cancel capture or re-focus. */
  if ((buttons[b] != 0) == (value != 0)) return;
  buttons[b] = value != 0;
  if (!value) {
    int id = capture[b]; capture[b] = 0;
    if (id) {
      struct os1_window_snapshot_v1 s = {
        .abi_version = OS1_COMPOSITOR_ABI_V1,
        .struct_size = sizeof(s), .id = id
      };
      if (OS1_window_snapshot(&s) == 0) send_mouse(&s, code, 0);
    }
    if (b == 0) { dragging = 0; resizing = 0; }
    return;
  }
  struct os1_window_snapshot_v1 s;
  if (!hit_test(px, py, &s)) return;
  if (b == 0) {
    (void)OS1_window_focus(s.id);
    int title = (s.flags & WININFO_TOPMOST) ? 0 : titlebar_height();
    const struct os1_compositor_style *style = os1_theme_style_active();
    if (title && style && !(s.flags & WININFO_PROTECTED)) {
      struct os1_gfx_button_geometry geometry;
      os1_gfx_button_geometry(s.x, s.draw_width, s.y - title, title,
                              style->button_shape, style->button_side,
                              &geometry);
      int hit = os1_gfx_button_hit(&geometry, px, py);
      if (hit == OS1_GFX_BUTTON_CLOSE) {
        (void)OS1_window_close(s.id);
        return;
      }
      if (hit == OS1_GFX_BUTTON_BACKGROUND) {
        (void)OS1_window_minimize(s.id);
        return;
      }
    }
    if (px >= s.x + s.draw_width - 12 && py >= s.y + s.draw_height - 12) {
      resizing = s.id; resize_x = px; resize_y = py;
      resize_w = s.width; resize_h = s.height;
      return;
    }
    if (title && py < s.y && py >= s.y - title) {
      dragging = s.id; drag_dx = px - s.x; drag_dy = py - s.y;
      return;
    }
  }
  capture[b] = s.id;
  send_mouse(&s, code, 1);
}

static void route_event(const struct os1_raw_input_event_v1 *e) {
  if (e->type == EV_KEY) {
    if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE ||
        e->code == BTN_TOUCH) {
      /* Touch contact extends the primary pointer button contract. */
      unsigned button = e->code == BTN_TOUCH ? BTN_LEFT : e->code;
      int b = button == BTN_LEFT ? 0 : button == BTN_RIGHT ? 1 : 2;
      pending_button[e->device_id][b] = 1;
      pending_value[e->device_id][b] = e->value != 0;
    }
    return;
  }
  if (e->type == EV_SYN) {
    for (int b = 0; b < 3; ++b) {
      if (!pending_button[e->device_id][b]) continue;
      pending_button[e->device_id][b] = 0;
      uint8_t next = pending_value[e->device_id][b];
      uint8_t previous = device_buttons[e->device_id][b];
      if (next == previous) continue;
      device_buttons[e->device_id][b] = next;
      if (next) ++button_holders[b];
      else if (button_holders[b]) --button_holders[b];
      button_event(b == 0 ? BTN_LEFT : b == 1 ? BTN_RIGHT : BTN_MIDDLE,
                   button_holders[b] != 0);
    }
    clamp_pointer();
    if (frame_motion && dragging && buttons[0])
      (void)OS1_window_move(dragging, px - drag_dx, py - drag_dy);
    else if (frame_motion && resizing && buttons[0])
      (void)OS1_window_resize(resizing, resize_w + px - resize_x,
                              resize_h + py - resize_y);
    if (frame_motion) {
      if (!(buttons[0] && (dragging || resizing))) {
        struct os1_window_snapshot_v1 s;
        if (hit_test(px, py, &s)) send_mouse(&s, 0, 2);
      }
      publish_pointer();
    }
    if (frame_wheel) {
      struct os1_window_snapshot_v1 s;
      if (hit_test(px, py, &s))
        send_mouse(&s, 0x120u, (unsigned)frame_wheel);
    }
    frame_motion = 0;
    frame_wheel = 0;
    return;
  }
  if (e->type == EV_REL) {
    if (e->code == REL_X) { px += e->value; frame_motion = 1; }
    if (e->code == REL_Y) { py += e->value; frame_motion = 1; }
    if (e->code == REL_WHEEL) frame_wheel += e->value;
  } else if (e->type == EV_ABS) {
    long d = OS1_display_info();
    int w = (int)((d >> 16) & 0xffff), h = (int)(d & 0xffff);
    if (e->code == ABS_X && w > 0) { px = (int)((int64_t)e->value * (w - 1) / 32767); frame_motion = 1; }
    if (e->code == ABS_Y && h > 0) { py = (int)((int64_t)e->value * (h - 1) / 32767); frame_motion = 1; }
  }
}

int main(void) {
  print("[nxmoused] ring-3 pointer service started\n");
  /* Synchronize the cursor even when no device motion arrives after boot. */
  publish_pointer();
  for (;;) {
    struct ipc_message control;
    for (unsigned i = 0; i < 8; ++i) {
      if (OS1low_ipc_try_recv(-1, &control) != 0) break;
      if (control.type == IPC_THEME_STYLE) {
        (void)os1_theme_set_style((int)(int64_t)control.data1);
        (void)os1_theme_set_palette((int)(int64_t)control.data2);
      } else if (control.type == IPC_THEME_BACKGROUND) {
        (void)os1_theme_set_background((int)(int64_t)control.data1);
      }
    }
    struct os1_raw_input_event_v1 e = {
      .abi_version = OS1_INPUT_ABI_V1, .struct_size = sizeof(e)
    };
    unsigned drained = 0;
    while (drained < 128 && OS1_input_next_mouse(&e) > 0) {
      route_event(&e); ++drained;
    }
    OS1_sleep(5);
  }
}
