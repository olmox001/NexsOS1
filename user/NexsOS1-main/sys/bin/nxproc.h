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
 * Consumers:
 *   user/sys/bin/nxproc.c  — INLINE ELF: shell `ps`, one-shot stdout dump
 *                            via nxproc_render_inline().  No compositor
 *                            window is opened; every row goes through printf
 *                            (write(1, ...)), so the shell's terminal shows
 *                            the table.
 *   user/sys/bin/nxtop.c   — WINDOWED realtime: opens its own compositor
 *                            window and refreshes at ~1Hz via
 *                            nxproc_render_if_changed(), redrawing only when
 *                            the table actually changes.
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
  case 7:
    return "STOPPED"; /* PROC_STOPPED (Phase 2 job control) */
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
 * This is the rendering logic: a header row, a separator, then one colourised
 * row per process (bright green
 * for RUNNING, grey for SLEEPING, terminal default otherwise).  The screen is
 * cleared first via ANSI so the window always reflects the current table.
 *
 * Unconditional: callers that want change-detection should gate this behind
 * nxproc_render_if_changed() rather than calling it every tick.
 */
/*
 * nxproc_cpu_color - pick an ANSI 256-colour escape for the per-process CPU
 * id column.
 *
 * cpu is clamped to [0, 255]; the cpu id IS the xterm-256 palette index, so
 * every one of the 256 possible cpu ids gets its own swatch.  cpu == -1
 * (process not currently on a cpu, sleeping/created/zombie/dead) returns a
 * plain grey; the caller's STATE colour already telegraphs the situation, so
 * the cpu cell should not steal the show.
 */
static inline const char *nxproc_cpu_color(int cpu) {
  if (cpu < 0 || cpu > 255)
    return "\033[90m";
  /* Direct 1-to-1 mapping: cpu id is the xterm-256 palette index.  The
   * compositor's ANSI parser (\033[38;5;N in term.c:105) supports all 256
   * entries, so each of the 256 possible cpu ids gets its own swatch and
   * adjacent cpu ids are always distinguishable.  The palette is identity
   * (cpu → idx) by design: no remapping math, no saturation loss, the
   * digit carries the colour chosen by the system palette author. */
  static char buf[2][16];
  static int slot;
  slot ^= 1;
  char *p = buf[slot];
  int n = 0;
  p[n++] = '\033';
  p[n++] = '[';
  p[n++] = '3';
  p[n++] = '8';
  p[n++] = ';';
  p[n++] = '5';
  p[n++] = ';';
  int v = cpu;
  char tmp[3];
  int t = 0;
  if (v >= 100)
    tmp[t++] = '0' + (v / 100);
  v %= 100;
  if (v >= 10 || t > 0)
    tmp[t++] = '0' + (v / 10);
  v %= 10;
  tmp[t++] = '0' + v;
  for (int i = 0; i < t; i++)
    p[n++] = tmp[i];
  p[n++] = 'm';
  p[n] = '\0';
  return buf[slot];
}

static inline void nxproc_render_rows(int win_id, const struct ps_info *procs,
                                      int count) {
  /* Clear screen using ANSI (handled by our compositor Terminal Emulator). */
  _sys_window_write(win_id, "\033[H\033[J", 6);
  _sys_window_write(win_id, "\033[1;34m", 7); /* bold blue header */
  printf_win(win_id, "%-4s %-16s %-10s %-4s %-3s\n", "PID", "NAME", "STATE",
             "PRIO", "CPU");
  _sys_window_write(win_id, "\033[0m", 4);
  _sys_window_write(win_id, "--------------------------------------------\n",
                    45);

  for (int i = 0; i < count; i++) {
    const char *state_str = nxproc_state_str(procs[i].state);
    int cpu = procs[i].on_cpu;

    /* Per-process STATE colour reflects the lifecycle.  READY (6) gets aqua so
     * a runnable-but-not-currently-running process stays distinct from
     * RUNNING (2, bold green).  ZOMBIE is muted brown; DEAD dim red. */
    const char *state_color;
    int state_len;
    if (procs[i].state == 2) {              /* RUNNING */
      state_color = "\033[1;32m"; state_len = 7;
    } else if (procs[i].state == 6) {       /* READY */
      state_color = "\033[38;5;51m"; state_len = 10;
    } else if (procs[i].state == 3) {       /* SLEEPING */
      state_color = "\033[90m"; state_len = 5;
    } else if (procs[i].state == 4) {       /* ZOMBIE */
      state_color = "\033[38;5;130m"; state_len = 11;
    } else if (procs[i].state == 5) {       /* DEAD */
      state_color = "\033[31m"; state_len = 5;
    } else {                                 /* CREATED=1 or anything else */
      state_color = "\033[37m"; state_len = 5;
    }

    /* Identity columns in the STATE colour. */
    _sys_window_write(win_id, state_color, state_len);
    printf_win(win_id, "%-4d %-16s %-10s %-4d ", procs[i].pid, procs[i].name,
               state_str, procs[i].priority);

    /* CPU column: pad left, then emit the digits in the per-cpu gradient.
     * The left-pad spaces stay in the STATE colour so only the digits carry
     * the cpu hue (the OPSERV-PROCS-02 #141 rule: "only the number is
     * coloured").  cpu == -1 (not on a cpu) gets grey for the whole cell.
     * The column is 3 chars wide (header "%-3s"), so any cpu ≥ 3 digits
     * overflows the cell visually — acceptable since 1000+ cores is
     * unrealistic for the current scheduler and the per-cpu palette still
     * distinguishes it. */
    if (cpu >= 0 && cpu <= 255) {
      const char *ccol = nxproc_cpu_color(cpu);
      int digits;
      if (cpu < 10)
        digits = 1;
      else if (cpu < 100)
        digits = 2;
      else
        digits = 3;
      /* pad = 3 - digits spaces in state colour */
      for (int s = 0; s < 3 - digits; s++)
        _sys_window_write(win_id, " ", 1);
      /* IMPORTANT: _sys_window_write takes an explicit byte count — passing 0
       * here would silently drop the cpu-colour escape and leave the digits
       * in the previous (state) colour.  Use strlen() on the NUL-terminated
       * escape so we always emit the whole "\033[38;5;Nm" sequence. */
      _sys_window_write(win_id, ccol, (unsigned long)strlen(ccol));
      printf_win(win_id, "%d", cpu);
      _sys_window_write(win_id, "\033[0m", 4);
      _sys_window_write(win_id, state_color, state_len);
    } else {
      printf_win(win_id, "%-3d", cpu);
    }

    _sys_window_write(win_id, "\n", 1);
    _sys_window_write(win_id, "\033[0m", 4); /* Reset color */
  }
}

/*
 * nxproc_render_inline - emit the formatted, ANSI-coloured process list to the
 * caller's stdout (fd 1, the TTY/window the program is running in).
 *
 * procs: snapshot array.
 * count: number of valid entries.
 *
 * INLINE-STDOUT variant of nxproc_render_rows().  No compositor window is
 * created: every row goes through printf(), which lib.c:429 routes to
 * write(1, ...) — the same fd the kernel gives every spawned process as its
 * TTY.  Used by `nxproc --inline` so the shell's `ps` command can dump the
 * table onto its own terminal without spawning a window.  The ANSI escapes
 * (clear-screen + colour SGRs) are identical to the windowed version so the
 * caller's terminal emulator handles them the same way.
 *
 * Unconditional: one-shot callers (nxproc's `cmd_print`) render exactly once,
 * so no change-detection wrapper exists for the inline variant — the windowed
 * loop uses nxproc_render_if_changed() below.
 */
static inline void nxproc_render_inline(const struct ps_info *procs,
                                       int count) {
  /* Inline render into the caller's controlling terminal.  Intentionally NO
   * "\033[H\033[J" clear-screen here: when `nxproc` runs as a child of the
   * shell (USR-TTY-01, inherited ctty) the clearing would wipe the shell's
   * prompt too, forcing the user to press Enter to repaint it.  The ANSI SGR
   * colour sequences below are otherwise identical to the windowed variant
   * so the terminal emulator in the compositor renders them the same way. */
  printf("\033[1;34m"); /* bold blue header */
  printf("%-4s %-16s %-10s %-4s %-3s\n", "PID", "NAME", "STATE", "PRIO", "CPU");
  printf("\033[0m");
  printf("--------------------------------------------\n");

  for (int i = 0; i < count; i++) {
    const char *state_str = nxproc_state_str(procs[i].state);
    int cpu = procs[i].on_cpu;

    const char *state_color;
    if (procs[i].state == 2) {              /* RUNNING */
      state_color = "\033[1;32m";
    } else if (procs[i].state == 6) {       /* READY */
      state_color = "\033[38;5;51m";
    } else if (procs[i].state == 3) {       /* SLEEPING */
      state_color = "\033[90m";
    } else if (procs[i].state == 4) {       /* ZOMBIE */
      state_color = "\033[38;5;130m";
    } else if (procs[i].state == 5) {       /* DEAD */
      state_color = "\033[31m";
    } else {                                 /* CREATED=1 or anything else */
      state_color = "\033[37m";
    }

    printf("%s", state_color);
    printf("%-4d %-16s %-10s %-4d ", procs[i].pid, procs[i].name, state_str,
           procs[i].priority);

    if (cpu >= 0 && cpu <= 255) {
      const char *ccol = nxproc_cpu_color(cpu);
      int digits;
      if (cpu < 10)
        digits = 1;
      else if (cpu < 100)
        digits = 2;
      else
        digits = 3;
      for (int s = 0; s < 3 - digits; s++)
        printf(" ");
      /* The colour escape is a NUL-terminated string built by
       * nxproc_cpu_color(); printf("%s", ...) emits the whole sequence
       * (lib.c printf goes through vsnprintf -> strlen(buf) -> write(1, ...),
       * so there is no count=0 silent-drop hazard like _sys_window_write). */
      printf("%s%d", ccol, cpu);
      printf("\033[0m");
      printf("%s", state_color);
    } else {
      printf("%-3d", cpu);
    }

    printf("\n");
    printf("\033[0m");
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
    /* Surface the error once, but don't keep
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
