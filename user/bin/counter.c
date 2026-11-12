/*
 * user/bin/counter.c
 *
 * Windowed scheduler/timer liveness probe.
 *
 * The process updates a compositor window, sleeps for 100 ms and emits a UART
 * heartbeat every ten iterations. Each heartbeat includes both the frame
 * counter and the monotonic millisecond clock so nxadb can distinguish task
 * progress from a guest that merely remains alive.
 */
#include <os1.h>

enum {
  COUNTER_SLEEP_MS = 100,
  COUNTER_HEARTBEAT_FRAMES = 10
};

static void counter_heartbeat(int frame) {
  print("[COUNTER] frame=");
  print_hex(frame);
  print(" mono_ms=");
  print_hex(get_time());
  print("\n");
}

int main(void) {
  char title[32];
  int frame = 0;
  int win;

  print("[COUNTER] START\n");
  sprintf(title, "Counter PID %d", get_pid());
  win = create_window(700, 50, 200, 100, title);
  if (win <= 0) {
    print("[COUNTER] ERROR create_window\n");
    return 1;
  }

  for (;;) {
    int bar_width = (frame % 100) * 2;

    window_draw(win, 0, 0, 200, 100, 0xFFFCFCFD);
    window_draw(win, 10, 40, bar_width, 20, 0xFF00FF00);
    flush();

    if ((frame % COUNTER_HEARTBEAT_FRAMES) == 0)
      counter_heartbeat(frame);

    ++frame;
    OS1_sleep(COUNTER_SLEEP_MS);
  }
}
