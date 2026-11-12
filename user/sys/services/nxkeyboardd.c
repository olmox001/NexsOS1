/* nxkeyboardd: ring-3 keyboard layout and focused-client event router. */
#include <os1.h>
#include <os1_input.h>
#include <string.h>
#include <nxkeyboard_layout.h>
#include <stdint.h>

#define EV_KEY 1

static void route_key(unsigned code, int value, struct nxs_keyboard_state *state,
                      int layout) {
  struct nxs_keyboard_key key;
  int translated = nxs_keyboard_translate(state, code, value, layout, &key);
  if (translated <= 0) return;
  struct window_info windows[40];
  long n = OS1_window_enum(windows, 40);
  if (n < 0) {
    OS1_report_error("nxkeyboardd.window_enum", (int)n);
    return;
  }
  int focus = 0;
  for (long i = 0; i < n && i < 40 && focus <= 0; ++i)
    if (windows[i].flags & WININFO_FOCUSED) focus = windows[i].pid;
  if (focus <= 0) return;
  struct ipc_message m = {0};
  m.type = IPC_TYPE_INPUT;
  m.data1 = ((uint64_t)key.code << 16) | key.key;
  m.data2 = (uint64_t)key.state;
  for (unsigned i = 0U; i < sizeof(key.utf8); ++i)
    m.payload[i] = key.utf8[i];
  long delivered = OS1low_ipc_send(focus, &m);
  if (delivered != 0)
    OS1_report_error("nxkeyboardd.focused_input", (int)(delivered < 0 ? delivered : -EIO));
}

int main(void) {
  static struct nxs_keyboard_state state[256];
  int layout = NXS_KBD_MAC19ITA;
  long checked_at = -1;
  for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
    struct os1_raw_input_event_v1 event = {
        .abi_version = OS1_INPUT_ABI_V1, .struct_size = sizeof(event)};
    long result = OS1_input_wait_keyboard(&event);

    if (result > 0) {
      long now = OS1_time_now();
      if (checked_at < 0 || now < checked_at ||
          now - checked_at >= 250) {
        char selected[24] = {0};
        if (OS1_registry_get(NXS_KBD_REGISTRY_KEY, selected,
                             sizeof(selected)) == 0) {
          int candidate = nxs_keyboard_layout_id(selected);
          if (candidate != 0) layout = candidate;
        }
        checked_at = now;
      }
      if (event.type == EV_KEY)
        route_key(event.code, event.value, &state[event.device_id], layout);
      continue;
    }
    if (result == -EAGAIN) {
      OS1_sleep(5);
      continue;
    }
    OS1_report_error("nxkeyboardd.input_wait",
                     (int)(result < 0 ? result : -EIO));
    OS1_sleep(50);
  }
  return 1; /* Service generation exhausted its finite work epoch. */
}
