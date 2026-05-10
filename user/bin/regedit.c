/*
 * user/regedit.c
 * Registry Editor (Control Panel)
 */
#include <os1.h>

#define MAX_KEYS 16
#define KEY_WIDTH 250
#define KEY_HEIGHT 30
#define PADDING 10

int main(void) {
  int w = 400;
  int h = 300;
  int win_id = create_window(100, 100, w, h, "Control Panel");
  if (win_id < 0)
    return 1;

  char keys[MAX_KEYS][64] = {"theme.color", "system.hostname",
                             "mouse.sensitivity"};
  int key_count = 3;

  char val_buf[128];

  while (1) {
    /* Clear background */
    window_draw(win_id, 0, 0, w, h, 0xFFE0E0E0);

    /* Draw Title */
    // TODO: Font rendering in window_draw? For now we only have basic rects.
    // We really need a proper UI library or font support in userspace.
    // Using `notify` as a poor-man's persistent display is not great.

    int y = PADDING;
    for (int i = 0; i < key_count; i++) {
      /* Draw Key Entry Background */
      window_draw(win_id, PADDING, y, w - 2 * PADDING, KEY_HEIGHT, 0xFFFFFFFF);

      /* Fetch Value */
      if (registry_read(keys[i], val_buf, sizeof(val_buf)) == 0) {
        /* Display Key */
        // window_print(win_id, PADDING + 5, y + 5, keys[i], 0xFF000000);
        /* Display Value */
        // window_print(win_id, PADDING + 150, y + 5, val_buf, 0xFF0000BB);
      }

      y += KEY_HEIGHT + PADDING;
    }

    /*
     * CRITICAL LIMITATION: We don't have text rendering in userspace library
     * yet! The `window_draw` syscall only does rectangles. The kernel
     * compositor draws text, but userspace cannot (easily).
     *
     * Workaround: Use `notify()` to show current values when "clicked".
     */

    struct ipc_message msg;
    if (recv(0, &msg)) { /* Non-blocking check? No, recv blocks. */
                         /* We need an event loop. */
    }

    compositor_render();
    yield();
  }

  return 0;
}
