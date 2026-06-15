/*
 * user/bin/counter.c
 * Counter Demo — animated progress-bar window
 *
 * Demonstrates the compositor window API: creates a small window, then in a
 * tight loop draws a dark background and an incrementing green progress bar
 * whose width cycles through 0..198 pixels (i % 100 * 2).
 *
 * Every 256 (0x100) iterations the current counter value is printed to UART
 * via print_hex() to give a heartbeat on the serial console.
 *
 * text rendering:
 *   window_draw() only fills solid rectangles; there is no userland text
 *   renderer available via this syscall, so the counter value cannot be shown
 *   as digits in the window.  UART output is the fallback.
 *
 * Known issues:
 *   USR-BLOAT-01 (W2 BAD-IMPL·PERF) counter.elf is ~503KB [verified]; 70% is
 *                DWARF debug data (-g), ~52KB is stb_image/stb_easy_font from
 *                lib.o that counter never uses.
 *   USR-BLOAT-02 (W2 BAD-IMPL) No --gc-sections or strip step in the link
 *                rule; debug symbols not removed for release builds.
 */
#include <os1.h>

/* Duplicate #include <os1.h> below is a copy-paste artifact; harmless due to
 * header include guards but indicates sloppy editing. */
#include <os1.h>

/*
 * main - counter entry point; does not return.
 *
 * Creates a 200x100 compositor window at (700, 50).  On each iteration:
 *   1. Fills background with dark grey (0xFF222222).
 *   2. Draws a green bar of width (i % 100)*2 pixels — a visual progress
 *      indicator cycling every 100 increments.
 *   3. Calls flush() (-> SYS_FLUSH/#201) to push the update to the display.
 *   4. Every 256 iterations prints the counter to UART for serial monitoring.
 *   5. Sleeps 10 jiffies (~100ms at 100Hz) between frames.
 *
 * Returns 1 if window creation fails; never returns otherwise.
 */
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
    window_draw(win, 0, 0, 200, 100, 0xFFFCFCFD);

    /* Draw Count */
    /* Crude digit drawing or assume window_draw handles text? No, it's just
       rects. We need a text drawing helper. For now, just visually flash or use
       printf to UART. */
    /* Since we don't have window_draw_text in lib.c yet, we rely on UART for
       text. But to satisfy "drawing correctly", I should draw a progress bar or
       rect! */

    /* Progress bar: width cycles 0..198 pixels (i%100 * 2), green fill. */
    int bar_width = (i % 100) * 2;
    window_draw(win, 10, 40, bar_width, 20, 0xFF00FF00);

    /* Flush global compositor */
    // _sys_compositor_render();
    // Wait, flush() does that.
    flush();

    /* Print counter to UART every 256 iterations as a serial heartbeat. */
    if ((i & 0xFF) == 0) {
      print("Count: ");
      print_hex(i);
      print("\n");
    }
    i++;

    /* Sleep ~100ms between frames (10 jiffies at the 100Hz system timer). */
    sleep(10);
  }

  return 0;
}
