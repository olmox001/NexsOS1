/*
 * user/sys/bin/proce.c
 * Process List Utility (compiled into shell)
 *
 * Implements the `ps` command for the shell.  Since the ASTRA stratified
 * process-management refactor, the actual snapshot + rendering logic lives in
 * the reusable header-only helper nxproc.h; this file is now a THIN wrapper that
 * keeps the long-standing proce_display_list() symbol working for shell.c's `ps`
 * command (so the shell's call site is unchanged).
 *
 * This file is not built as a standalone ELF; it is compiled together with
 * shell.c (via proce.h declaration) so that process listing runs in-process.
 */
#include "proce.h"
#include "nxproc.h"
#include <os1.h>

/*
 * proce_display_list - render the current process table to a compositor window.
 *
 * win_id: compositor window ID to write into (passed by the shell's ps command).
 *
 * One-shot render for the shell: takes a fresh snapshot via the helper and
 * writes the formatted, ANSI-coloured list unconditionally (the shell invokes
 * this once per `ps`, so there is no refresh loop to optimise here — the
 * change-detection path is used by the windowed front-ends top/nxproc instead).
 * Errors from the snapshot syscall are reported into the window.
 */
void proce_display_list(int win_id) {
  struct ps_info procs[NXPROC_MAX]; /* Stack-allocated; max 32 processes. */

  int count = nxproc_snapshot(procs, NXPROC_MAX);
  if (count < 0) {
    _sys_window_write(win_id, "Error fetching process list\n", 28);
    return;
  }

  nxproc_render_rows(win_id, procs, count);
}
