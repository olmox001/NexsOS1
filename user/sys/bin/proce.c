/*
 * user/sys/bin/proce.c
 * Process List Utility (compiled into shell)
 *
 * Implements the `ps` command for the shell.  Calls SYS_GET_PS (#222) to
 * retrieve a snapshot of up to 32 process descriptors from the kernel, then
 * formats and renders them into the shell's compositor window using ANSI
 * escape sequences (handled by the compositor's built-in terminal emulator).
 *
 * This file is not built as a standalone ELF; it is compiled together with
 * shell.c (via proce.h declaration) so that process listing runs in-process.
 *
 * State integer to string mapping uses the values defined in the kernel's
 * sched/process.h (PROC_CREATED=1, RUNNING=2, SLEEPING=3, ZOMBIE=4,
 * DEAD=5, READY=6).  Any other value maps to "UNUSED".
 */
#include "proce.h"
#include <os1.h>

/*
 * proce_display_list - render the current process table to a compositor window.
 *
 * win_id: compositor window ID to write into (passed by the shell's ps command).
 *
 * Calls _sys_get_procs() directly (SYS_GET_PS, syscall #222) to fill a
 * local array of up to 32 struct ps_info entries.  Returns immediately on
 * error.
 *
 * Output format (ANSI-coloured):
 *   PID  NAME              STATE      PRIO CPU
 *   ---- ----------------  ---------- ---- ---
 *   ...
 *
 * Running processes are shown in bright green (\033[92m); sleeping in grey
 * (\033[90m); all others in the terminal default colour.
 *
 * Side effects: writes ANSI sequences and formatted rows to win_id via
 *   _sys_write() and printf_win().  No heap allocation.
 */
void proce_display_list(int win_id) {
  struct ps_info procs[32]; /* Stack-allocated; max 32 processes queried */

  int count = _sys_get_procs(procs, 32);
  if (count < 0) {
    _sys_window_write(win_id, "Error fetching process list\n", 28);
    return;
  }

  /* Clear screen using ANSI (handled by our compositor Terminal Emulator) */
  _sys_window_write(win_id, "\033[H\033[J", 6);
  _sys_window_write(win_id, "\033[1;33m", 7); /* Bold Yellow */
  printf_win(win_id, "%-4s %-16s %-10s %-4s %-3s\n", "PID", "NAME", "STATE",
             "PRIO", "CPU");
  _sys_window_write(win_id, "\033[0m", 4); /* Reset */
  _sys_window_write(win_id, "--------------------------------------------\n", 45);

  for (int i = 0; i < count; i++) {
    /* Map numeric state to a human-readable string.
     * Values match kernel enum proc_state (kernel/sched/process.h). */
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
      _sys_window_write(win_id, "\033[92m", 5); /* Bright Green for Running */
    else if (procs[i].state == 3)
      _sys_window_write(win_id, "\033[90m", 5); /* Grey for Sleeping */

    printf_win(win_id, "%-4d %-16s %-10s %-4d %-3d\n", procs[i].pid,
               procs[i].name, state_str, procs[i].priority, procs[i].on_cpu);

    _sys_window_write(win_id, "\033[0m", 4); /* Reset color */
  }
}
