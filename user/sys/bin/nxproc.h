#ifndef _USER_NXPROC_H
#define _USER_NXPROC_H

/*
 * user/sys/bin/nxproc.h
 * NEXS stratified process-management helper (ASTRA service layer).
 *
 * A REUSABLE helper layer that both user and system apps may link against to
 * inspect and act on the process table.  It is intentionally header-only
 * (all routines `static inline`) so consumers pull it in without any extra
 * object or link-rule churn in the Makefile.
 *
 * SECURITY MODEL: this layer adds NO ambient permission checks of its own.
 * Every routine is a thin wrapper over an existing OS1 syscall, and the kernel
 * already gates those syscalls per caller (e.g. kill is checked against the
 * caller's capability/level).  Security is therefore "by caller" automatically:
 * a privileged system app and an unprivileged user app calling the same helper
 * get exactly the rights the kernel grants each of them — the helper neither
 * widens nor narrows that.
 *
 * State integer to string mapping uses the values defined in the kernel's
 * sched/process.h (PROC_CREATED=1, RUNNING=2, SLEEPING=3, ZOMBIE=4, DEAD=5,
 * READY=6).  Any other value maps to "UNUSED".
 *
 * Consumers: user/sys/bin/proce.c (shell `ps`), user/sys/bin/top.c,
 *            user/sys/bin/nxproc.c.
 */

#include <os1.h>

/* Hard cap on processes we ever snapshot/render in one pass (matches the
 * kernel's MAX_PROCESSES ceiling used elsewhere in userland). */
#define NXPROC_MAX 32

/*
 * nxproc_snapshot - fill a caller-provided array with the live process table.
 *
 * out:       caller-owned array of at least `max` struct ps_info entries.
 * max:       capacity of `out`; clamped to NXPROC_MAX internally.
 *
 * Thin wrapper over _sys_get_procs() (SYS_GET_PS).  Returns the number of
 * entries written (0..max), or a negative value on syscall error.  No heap
 * allocation; the snapshot is a point-in-time copy.
 */
static inline int nxproc_snapshot(struct ps_info *out, int max) {
  if (!out || max <= 0)
    return 0;
  if (max > NXPROC_MAX)
    max = NXPROC_MAX;
  return _sys_get_procs(out, (size_t)max);
}

/*
 * nxproc_kill - request termination of a process by pid.
 *
 * pid: target process id.
 *
 * Thin wrapper over kill_process() (which routes to SYS_KILL).  Returns the
 * syscall result (0 on success, negative on failure/denial).  The kernel
 * enforces whether the *caller* is allowed to kill `pid`; this wrapper performs
 * no check of its own.
 */
static inline int nxproc_kill(int pid) { return kill_process(pid); }

/*
 * nxproc_state_str - map a numeric proc_state to a human-readable string.
 *
 * Values match kernel enum proc_state (kernel/sched/process.h).  Unknown values
 * map to "UNUSED" so callers always get a printable, fixed token.
 */
static inline const char *nxproc_state_str(int state) {
  switch (state) {
  case 1:
    return "CREATED";
  case 2:
    return "RUNNING";
  case 3:
    return "SLEEPING";
  case 4:
    return "ZOMBIE";
  case 5:
    return "DEAD";
  case 6:
    return "READY";
  default:
    return "UNUSED";
  }
}

/*
 * nxproc_signature - fold the render-relevant fields of a snapshot into a
 * single 64-bit value.
 *
 * procs: snapshot array.
 * count: number of valid entries.
 *
 * Only the fields the rendered list actually shows (count, pid, state,
 * priority, on_cpu) are mixed in, using a cheap FNV-1a-style rolling hash.
 * Two snapshots that would render identically produce the same signature, so a
 * renderer can compare against the previous signature and skip the redraw when
 * nothing visible changed.  cpu_time is deliberately excluded: it ticks every
 * sample and would defeat change-detection without altering the displayed
 * columns.
 */
static inline unsigned long nxproc_signature(const struct ps_info *procs,
                                             int count) {
  unsigned long h = 1469598103934665603UL; /* FNV-1a 64-bit offset basis */
  /* Mix the count itself so a process appearing/disappearing always shifts. */
  h = (h ^ (unsigned long)(unsigned int)count) * 1099511628211UL;
  for (int i = 0; i < count; i++) {
    h = (h ^ (unsigned long)(unsigned int)procs[i].pid) * 1099511628211UL;
    h = (h ^ (unsigned long)(unsigned int)procs[i].state) * 1099511628211UL;
    h = (h ^ (unsigned long)(unsigned int)procs[i].priority) * 1099511628211UL;
    h = (h ^ (unsigned long)(unsigned int)procs[i].on_cpu) * 1099511628211UL;
  }
  return h;
}

/*
 * nxproc_render_rows - write the formatted, ANSI-coloured process list to a
 * window.
 *
 * win_id: compositor window id to write into.
 * procs:  snapshot array.
 * count:  number of valid entries.
 *
 * This is the rendering logic migrated from proce.c's proce_display_list():
 * a header row, a separator, then one colourised row per process (bright green
 * for RUNNING, grey for SLEEPING, terminal default otherwise).  The screen is
 * cleared first via ANSI so the window always reflects the current table.
 *
 * Unconditional: callers that want change-detection should gate this behind
 * nxproc_render_if_changed() rather than calling it every tick.
 */
static inline void nxproc_render_rows(int win_id, const struct ps_info *procs,
                                      int count) {
  /* Clear screen using ANSI (handled by our compositor Terminal Emulator). */
  _sys_window_write(win_id, "\033[H\033[J", 6);
  _sys_window_write(win_id, "\033[1;34m", 7); // bold Blue
  printf_win(win_id, "%-4s %-16s %-10s %-4s %-3s\n", "PID", "NAME", "STATE",
             "PRIO", "CPU");
  _sys_window_write(win_id, "\033[0m", 4); /* Reset */
  _sys_window_write(win_id, "--------------------------------------------\n",
                    45);

  for (int i = 0; i < count; i++) {
    const char *state_str = nxproc_state_str(procs[i].state);

    /* Colorize based on state. */
    if (procs[i].state == 2)
      _sys_window_write(win_id, "\033[92m", 5); /* Bright Green for Running */
    else if (procs[i].state == 3)
      _sys_window_write(win_id, "\033[90m", 5); /* Grey for Sleeping */

    printf_win(win_id, "%-4d %-16s %-10s %-4d %-3d\n", procs[i].pid,
               procs[i].name, state_str, procs[i].priority, procs[i].on_cpu);

    _sys_window_write(win_id, "\033[0m", 4); /* Reset color */
  }
}

/*
 * nxproc_render_if_changed - OPTIMIZED render: snapshot the table and only
 * rebuild+write the window when the visible content actually changed.
 *
 * win_id:   compositor window id to write into.
 * last_sig: in/out pointer to the caller's "previous signature" state.  Seed it
 *           with a sentinel (e.g. 0) on the first call; the helper updates it.
 *
 * Computes the snapshot's signature (see nxproc_signature) and compares it to
 * *last_sig.  If unchanged, returns immediately WITHOUT touching the window, so
 * an N-Hz refresh loop never busy-redraws an unchanged list.  If changed, it
 * re-renders via nxproc_render_rows() and stores the new signature.
 *
 * Returns 1 if it (re)rendered, 0 if it skipped because nothing changed, or a
 * negative value if the snapshot syscall failed.
 */
static inline int nxproc_render_if_changed(int win_id,
                                           unsigned long *last_sig) {
  struct ps_info procs[NXPROC_MAX];

  int count = nxproc_snapshot(procs, NXPROC_MAX);
  if (count < 0) {
    /* Surface the error once, mirroring proce.c's behaviour, but don't keep
     * rewriting it: fold the error into the signature so a persistent failure
     * is reported a single time. */
    unsigned long err_sig = ~0UL;
    if (last_sig && *last_sig != err_sig) {
      _sys_window_write(win_id, "Error fetching process list\n", 28);
      *last_sig = err_sig;
    }
    return count;
  }

  unsigned long sig = nxproc_signature(procs, count);
  if (last_sig && *last_sig == sig)
    return 0; /* Unchanged since last render: skip the write entirely. */

  nxproc_render_rows(win_id, procs, count);
  if (last_sig)
    *last_sig = sig;
  return 1;
}

#endif /* _USER_NXPROC_H */
