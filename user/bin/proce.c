#include "proce.h"
#include <os1.h>

/* Real process list diagnostic using SYS_GET_PS */
void proce_display_list(int win_id) {
  struct ps_info procs[32];

  int count = _sys_get_procs(procs, 32);
  if (count < 0) {
    _sys_write(win_id, "Error fetching process list\n", 28);
    return;
  }

  /* Clear screen using ANSI (handled by our compositor Terminal Emulator) */
  _sys_write(win_id, "\033[H\033[J", 6);
  _sys_write(win_id, "\033[1;33m", 7); /* Bold Yellow */
  printf_win(win_id, "%-4s %-16s %-10s %-4s %-3s\n", "PID", "NAME", "STATE",
             "PRIO", "CPU");
  _sys_write(win_id, "\033[0m", 4); /* Reset */
  _sys_write(win_id, "--------------------------------------------\n", 45);

  for (int i = 0; i < count; i++) {
    const char *state_str = "UNKNOWN";
    switch (procs[i].state) {
    case 1:
      state_str = "CREATED";
      break;
    case 2:
      state_str = "RUNNING";
      break;
    case 3:
      state_str = "SLEEPING";
      break;
    case 4:
      state_str = "ZOMBIE";
      break;
    case 5:
      state_str = "DEAD";
      break;
    case 6:
      state_str = "READY";
      break;
    default:
      state_str = "UNUSED";
      break;
    }

    /* Colorize based on state */
    if (procs[i].state == 2)
      _sys_write(win_id, "\033[92m", 5); /* Bright Green for Running */
    else if (procs[i].state == 3)
      _sys_write(win_id, "\033[90m", 5); /* Grey for Sleeping */

    printf_win(win_id, "%-4d %-16s %-10s %-4d %-3d\n", procs[i].pid,
               procs[i].name, state_str, procs[i].priority, procs[i].on_cpu);

    _sys_write(win_id, "\033[0m", 4); /* Reset color */
  }
}
