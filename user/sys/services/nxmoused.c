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

/* One owner per actual service process: no shared mutable translation-unit
 * state, no extra daemon and no duplicate pointer engine. All device/gesture
 * slots, button reference counts and frame queues stay in this same struct. */
#define NX_MOUSE_MAX_DEVICES 256U
struct nxmouse_state {
  int px, py, dragging, drag_dx, drag_dy;
  int resizing, resize_x, resize_y, resize_w, resize_h;
  int buttons[3], capture[3];
  uint8_t device_buttons[NX_MOUSE_MAX_DEVICES][3];
  uint8_t pending_button[NX_MOUSE_MAX_DEVICES][3];
  uint8_t pending_value[NX_MOUSE_MAX_DEVICES][3];
  unsigned button_holders[3];
  int frame_motion, frame_wheel;
};

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

static void send_mouse(struct nxmouse_state *state, const struct os1_window_snapshot_v1 *w,
                       unsigned button, unsigned button_state) {
  if (!w || w->pid <= 0) return;
  struct ipc_message m = {0};
  m.type = IPC_TYPE_MOUSE;
  m.data1 = button;
  m.data2 = button_state;
  int32_t x = state->px - w->x, y = state->py - w->y;
  const uint8_t *xb = (const uint8_t *)&x;
  const uint8_t *yb = (const uint8_t *)&y;
  for (size_t i = 0U; i < sizeof(x); ++i) {
    m.payload[i] = (char)xb[i];
    m.payload[sizeof(x) + i] = (char)yb[i];
  }
  long delivered = OS1low_ipc_send(w->pid, &m);
  if (delivered != 0)
    OS1_report_error("nxmoused.pointer_delivery",
                     (int)(delivered < 0 ? delivered : -EIO));
}

static void publish_pointer(struct nxmouse_state *state) {
  char text[20];
  long pid = OS1_registry_get("srv.compositor_pid", text, sizeof(text));
  if (pid != 0) return;
  int to = atoi(text);
  if (to <= 0) return;
  struct ipc_message m = {0};
  m.type = IPC_POINTER_POS;
  m.data1 = (uint32_t)state->px;
  m.data2 = (uint32_t)state->py;
  if (OS1low_ipc_send(to, &m) == 0) {
    long invalidated = OS1_surface_invalidate();
    if (invalidated < 0)
      OS1_report_error("nxmoused.surface_invalidate", (int)invalidated);
  }
}

static void clamp_pointer(struct nxmouse_state *state) {
  long d = OS1_display_info();
  int w = (int)((d >> 16) & 0xffff), h = (int)(d & 0xffff);
  if (w > 0 && state->px >= w) state->px = w - 1;
  if (h > 0 && state->py >= h) state->py = h - 1;
  if (state->px < 0) state->px = 0;
  if (state->py < 0) state->py = 0;
}

/* UI policy remains in the userland pointer router, not in NX-CORE.
 * Only the actual titlebar control is handled by this function. */
static int mouse_title_action(struct nxmouse_state *state,
                              const struct os1_window_snapshot_v1 *s,
                              int title) {
  const struct os1_compositor_style *style = os1_theme_style_active();
  if (!title || style == NULL || (s->flags & WININFO_PROTECTED))
    return 0;
  struct os1_gfx_button_geometry geometry;
  os1_gfx_button_geometry(s->x, s->draw_width, s->y - title, title,
                          style->button_shape, style->button_side, &geometry);
  int hit = os1_gfx_button_hit(&geometry, state->px, state->py);
  if (hit == OS1_GFX_BUTTON_CLOSE) {
    long closed = OS1_window_close(s->id);
    if (closed != 0)
      OS1_report_error("nxmoused.window_close",
                       (int)(closed < 0 ? closed : -EIO));
    return 1;
  }
  if (hit == OS1_GFX_BUTTON_BACKGROUND) {
    long minimized = OS1_window_minimize(s->id);
    if (minimized != 0)
      OS1_report_error("nxmoused.window_minimize",
                       (int)(minimized < 0 ? minimized : -EIO));
    return 1;
  }
  return 0;
}

static void button_event(struct nxmouse_state *state, unsigned code, int value) {
  int b = code == BTN_LEFT ? 0 : code == BTN_RIGHT ? 1 : 2;
  /* PS/2 repeats the full button state on every packet. Only transitions are
   * clicks; repeated held-state reports must not cancel state->capture or re-focus. */
  if ((state->buttons[b] != 0) == (value != 0)) return;
  state->buttons[b] = value != 0;
  if (!value) {
    int id = state->capture[b]; state->capture[b] = 0;
    if (id) {
      struct os1_window_snapshot_v1 s = {
        .abi_version = OS1_COMPOSITOR_ABI_V1,
        .struct_size = sizeof(s), .id = id
      };
      if (OS1_window_snapshot(&s) == 0) send_mouse(state, &s, code, 0);
    }
    if (b == 0) { state->dragging = 0; state->resizing = 0; }
    return;
  }
  struct os1_window_snapshot_v1 s;
  if (!hit_test(state->px, state->py, &s)) return;
  if (b == 0) {
    long focused = OS1_window_focus(s.id);
    if (focused != 0) {
      OS1_report_error("nxmoused.window_focus",
                       (int)(focused < 0 ? focused : -EIO));
      return;
    }
    int title = (s.flags & WININFO_TOPMOST) ? 0 : titlebar_height();
    if (mouse_title_action(state, &s, title))
      return;
    if (state->px >= s.x + s.draw_width - 12 && state->py >= s.y + s.draw_height - 12) {
      state->resizing = s.id; state->resize_x = state->px; state->resize_y = state->py;
      state->resize_w = s.width; state->resize_h = s.height;
      return;
    }
    if (title && state->py < s.y && state->py >= s.y - title) {
      state->dragging = s.id; state->drag_dx = state->px - s.x; state->drag_dy = state->py - s.y;
      return;
    }
  }
  state->capture[b] = s.id;
  send_mouse(state, &s, code, 1);
}

/* One bounded per-frame mouse state-machine batch, retaining device
 * button reference counting, drag/resize and motion coalescing. */
static void mouse_sync(struct nxmouse_state *state,
                       const struct os1_raw_input_event_v1 *e) {
    for (int b = 0; b < 3; ++b) {
      if (!state->pending_button[e->device_id][b]) continue;
      state->pending_button[e->device_id][b] = 0;
      uint8_t next = state->pending_value[e->device_id][b];
      uint8_t previous = state->device_buttons[e->device_id][b];
      if (next == previous) continue;
      state->device_buttons[e->device_id][b] = next;
      if (next) ++state->button_holders[b];
      else if (state->button_holders[b]) --state->button_holders[b];
      button_event(state, b == 0 ? BTN_LEFT : b == 1 ? BTN_RIGHT : BTN_MIDDLE,
                   state->button_holders[b] != 0);
    }
    clamp_pointer(state);
    if (state->frame_motion && state->dragging && state->buttons[0]) {
      long moved = OS1_window_move(state->dragging, state->px - state->drag_dx,
                                   state->py - state->drag_dy);
      if (moved != 0) {
        OS1_report_error("nxmoused.window_move",
                         (int)(moved < 0 ? moved : -EIO));
        state->dragging = 0;
      }
    } else if (state->frame_motion && state->resizing && state->buttons[0]) {
      long resized = OS1_window_resize(state->resizing,
          state->resize_w + state->px - state->resize_x,
          state->resize_h + state->py - state->resize_y);
      if (resized != 0) {
        OS1_report_error("nxmoused.window_resize",
                         (int)(resized < 0 ? resized : -EIO));
        state->resizing = 0;
      }
    }
    if (state->frame_motion) {
      if (!(state->buttons[0] && (state->dragging || state->resizing))) {
        struct os1_window_snapshot_v1 s;
        if (hit_test(state->px, state->py, &s)) send_mouse(state, &s, 0, 2);
      }
      publish_pointer(state);
    }
    if (state->frame_wheel) {
      struct os1_window_snapshot_v1 s;
      if (hit_test(state->px, state->py, &s))
        send_mouse(state, &s, 0x120u, (unsigned)state->frame_wheel);
    }
    state->frame_motion = 0;
    state->frame_wheel = 0;
}

static void route_event(struct nxmouse_state *state, const struct os1_raw_input_event_v1 *e) {
  /* The wire device_id is uint8_t; every representable ID has a slot. */
  if (state == NULL || e == NULL) {
    OS1_report_error("nxmoused.route_event.null", -EFAULT);
    return;
  }
  if (e->type == EV_KEY) {
    if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE ||
        e->code == BTN_TOUCH) {
      /* Touch contact extends the primary pointer button contract. */
      unsigned button = e->code == BTN_TOUCH ? BTN_LEFT : e->code;
      int b = button == BTN_LEFT ? 0 : button == BTN_RIGHT ? 1 : 2;
      state->pending_button[e->device_id][b] = 1;
      state->pending_value[e->device_id][b] = e->value != 0;
    }
    return;
  }
  if (e->type == EV_SYN) {
    mouse_sync(state, e);
    return;
  }
  if (e->type == EV_REL) {
    if (e->code == REL_X) { state->px += e->value; state->frame_motion = 1; }
    if (e->code == REL_Y) { state->py += e->value; state->frame_motion = 1; }
    if (e->code == REL_WHEEL) state->frame_wheel += e->value;
  } else if (e->type == EV_ABS) {
    long d = OS1_display_info();
    int w = (int)((d >> 16) & 0xffff), h = (int)(d & 0xffff);
    if (e->code == ABS_X && w > 0) { state->px = (int)((int64_t)e->value * (w - 1) / 32767); state->frame_motion = 1; }
    if (e->code == ABS_Y && h > 0) { state->py = (int)((int64_t)e->value * (h - 1) / 32767); state->frame_motion = 1; }
  }
}

int main(void) {
  struct nxmouse_state state = {.px = 400, .py = 300};
  /* Synchronize the cursor even when no device motion arrives after boot. */
  publish_pointer(&state);
  for (;;) {
    struct ipc_message control;
    for (unsigned i = 0; i < 8; ++i) {
      long received = OS1low_ipc_try_recv(-1, &control);
      if (received == -EAGAIN) break;
      if (received != 0) {
        OS1_report_error("nxmoused.control_receive",
                         (int)(received < 0 ? received : -EIO));
        break;
      }
      if (control.type == IPC_THEME_STYLE) {
        int style_rc = os1_theme_set_style((int)(int64_t)control.data1);
        if (style_rc != 0)
          OS1_report_error("nxmoused.theme_style", style_rc);
        else {
          int palette_rc = os1_theme_set_palette((int)(int64_t)control.data2);
          if (palette_rc != 0)
            OS1_report_error("nxmoused.theme_palette", palette_rc);
        }
      } else if (control.type == IPC_THEME_BACKGROUND) {
        int background_rc = os1_theme_set_background((int)(int64_t)control.data1);
        if (background_rc != 0)
          OS1_report_error("nxmoused.theme_background", background_rc);
      }
    }
    struct os1_raw_input_event_v1 e = {
      .abi_version = OS1_INPUT_ABI_V1, .struct_size = sizeof(e)
    };
    unsigned drained = 0;
    for (; drained < 128U; ++drained) {
      long next = OS1_input_next_mouse(&e);
      if (next == 0 || next == -EAGAIN)
        break;
      if (next < 0) {
        OS1_report_error("nxmoused.input_next", (int)next);
        break;
      }
      route_event(&state, &e);
    }
    OS1_sleep(5);
  }
}
