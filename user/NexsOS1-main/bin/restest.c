/*
 * user/bin/restest.c — resize/zoom stress (S-STAB regression guard).
 *
 * Hammers the dynamic-display chain (SYS_SET_DISPLAY_MODE / SYS_SET_ZOOM) as
 * fast as it can.  On SMP this process migrates across cores, so its set_mode
 * calls race the compositor render that init now drives — exactly the
 * resize-vs-render interleaving that used to smash a kernel stack / NULL the
 * current_chip global (the amd64 nanosleep panic and the aarch64
 * current_chip->end #PF).  If the render is properly confined to process
 * context, this loops indefinitely with no fault.
 *
 * Run it from the shell (`restest`) or spawn it; it never returns.
 */
#include <os1.h>

int main(void) {
  static const int modes[][2] = {
      {1280, 720}, {1440, 900}, {1024, 768},
      {1280, 800}, {800, 600},  {1600, 900},
  };
  int n = (int)(sizeof(modes) / sizeof(modes[0]));

  for (int i = 0;; i++) {
    OS1_display_set_mode(modes[i % n][0], modes[i % n][1]);
    OS1_sleep(12);

    if ((i & 3) == 0) {
      OS1_display_set_zoom(100 + (i % 3) * 50);
      OS1_sleep(8);
    }
  }
  return 0;
}
