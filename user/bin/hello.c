#include <os1.h>

int main(void) {
  int my_win;

  my_win = _sys_create_window(100, 100, 400, 300, "Hello Window");
  if (my_win < 0) {
    print("window creation failed\n");
    return 1;
  }

  _sys_window_write(my_win, "ciao mondo", 10);

  /* Mantieni il processo vivo senza CPU burn: sleep bloccante reale
   * (SYS_NANOSLEEP) invece di yield()-spin, che bruciava un core. */
  while (1) {
    sleep(1000);
  }

  return 0;
}