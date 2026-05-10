#include <os1.h>

#include <os1.h>

int main(void) {
  print("Counter Process Started\n");

  char title[32];
  sprintf(title, "Counter PID %d", get_pid());
  int win = create_window(700, 50, 200, 100, title);
  if (win <= 0) {
    print("Counter: Failed to create window\n");
    return 1;
  }

  int i = 0;
  while (1) {
    /* Draw Background */
    window_draw(win, 0, 0, 200, 100, 0xFF222222);

    /* Draw Count */
    /* Crude digit drawing or assume window_draw handles text? No, it's just
       rects. We need a text drawing helper. For now, just visually flash or use
       printf to UART. */
    /* Since we don't have window_draw_text in lib.c yet, we rely on UART for
       text. But to satisfy "drawing correctly", I should draw a progress bar or
       rect! */

    int bar_width = (i % 100) * 2;
    window_draw(win, 10, 40, bar_width, 20, 0xFF00FF00);

    /* Flush global compositor */
    // _sys_compositor_render();
    // Wait, flush() does that.
    flush();

    if ((i & 0xFF) == 0) {
      print("Count: ");
      print_hex(i);
      print("\n");
    }
    i++;

    sleep(10);
  }

  return 0;
}
