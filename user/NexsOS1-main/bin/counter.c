/*
 * user/bin/counter.c
 *
 * Windowed scheduler/timer liveness probe.
 *
 * The process updates a compositor window, sleeps for 100 ms and emits a
 * compact diagnostic heartbeat every five seconds. Each heartbeat includes
 * frame, monotonic time and elapsed time since the prior heartbeat so nxadb can
 * distinguish forward progress from a guest that merely remains alive without
 * flooding the TTY/UART diagnostic stream.
 */
#include <os1.h>

enum {
  COUNTER_SLEEP_MS = 100,
  COUNTER_HEARTBEAT_FRAMES = 50
};

static void counter_heartbeat(int frame) {
  static long previous_ms;
  long now_ms = get_time();
  long delta_ms = previous_ms > 0 ? now_ms - previous_ms : 0;
  printf("[COUNTER] frame=%d mono_ms=%ld delta_ms=%ld\n",
         frame, now_ms, delta_ms);
  previous_ms = now_ms;
}

int main(void) {
  char title[32];
  int frame = 0;
  int win;

  printf("[COUNTER] START pid=%d frame_ms=%d heartbeat_ms=%d\n",
         get_pid(), COUNTER_SLEEP_MS,
         COUNTER_SLEEP_MS * COUNTER_HEARTBEAT_FRAMES);
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
