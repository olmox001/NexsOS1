#ifndef _USER_NXINFO_H
#define _USER_NXINFO_H

/*
 * user/sys/bin/nxinfo.h
 * NEXS stratified system-information helper (ASTRA service layer).
 *
 * A REUSABLE helper layer that both user and system apps may link against to
 * read coarse system state: uptime, live process count, desktop resolution,
 * the caller's own pid/cwd, and the OS version banner.  Like nxproc.h it is
 * intentionally header-only (all routines `static inline`) so consumers pull it
 * in without any extra object or link-rule churn in the Makefile.
 *
 * SECURITY MODEL: this layer adds NO ambient permission checks of its own.
 * Every routine is a thin wrapper over an existing READ-ONLY OS1 syscall
 * (get_time/os1_mono_ns, _sys_get_procs, _sys_display_info, get_pid, getcwd),
 * and the kernel already gates those syscalls per caller.  Security is
 * therefore "by caller" automatically: a privileged system app and an
 * unprivileged user app calling the same helper each see exactly what the
 * kernel grants them — the helper neither widens nor narrows that.  Nothing
 * here mutates system state.
 *
 * Process counting reuses nxproc_snapshot() from nxproc.h rather than
 * duplicating the SYS_GET_PS marshalling, keeping a single source of truth for
 * the table.
 *
 * Consumers: user/sys/bin/nxinfo.c.
 */

#include "nxproc.h" /* nxproc_snapshot(), NXPROC_MAX, struct ps_info */
#include <os1.h>

/* OS version banner.  Mirrors the build's VERSION (Makefile: VERSION ?=
 * V0.0.4.2); kept as a userland constant since there is no syscall that reports
 * it.
 * TODO: surface the kernel build string via a syscall and read it here instead
 * of hardcoding, so the banner can never drift from the running kernel. */
#define NXINFO_OS_NAME "NEXS"
#define NXINFO_OS_VERSION "V0.0.4.2"

/*
 * nxinfo_uptime_ms - milliseconds elapsed since boot.
 *
 * Thin wrapper over get_time() (SYS_GET_TIME), which is the millisecond
 * monotonic clock the rest of userland already uses.  Never negative in normal
 * operation; the raw syscall value is returned unmodified.
 */
static inline long nxinfo_uptime_ms(void) { return get_time(); }

/*
 * nxinfo_proc_count - number of entries currently in the process table.
 *
 * Takes a point-in-time snapshot via nxproc_snapshot() (capped at NXPROC_MAX)
 * and returns the count it reported (0..NXPROC_MAX), or a negative value if the
 * underlying syscall failed.  No heap allocation: the snapshot lives on the
 * caller's stack for the duration of the call only.
 */
static inline int nxinfo_proc_count(void) {
  struct ps_info procs[NXPROC_MAX];
  return nxproc_snapshot(procs, NXPROC_MAX);
}

/*
 * nxinfo_display - current desktop resolution.
 *
 * w/out, h/out: optional out-params; either may be NULL if the caller only
 *               wants the other dimension (or just the side effect of probing).
 *
 * Thin wrapper over OS1_display_info(), which returns the packed value
 * ((w << 16) | h).  Unpacks the two 16-bit fields into *w and *h.
 */
static inline void nxinfo_display(int *w, int *h) {
  long info = OS1_display_info();
  if (w)
    *w = (int)((info >> 16) & 0xFFFF);
  if (h)
    *h = (int)(info & 0xFFFF);
}

/*
 * nxinfo_emit - internal: route one preformatted line to stdout or a window.
 *
 * win_id_or_neg: a compositor window id to write into, or any negative value to
 *                emit to the caller's stdout/own window via printf().
 *
 * This is the single place that branches on the output sink so the summary
 * formatter below can stay sink-agnostic.  `s` is expected to be
 * NUL-terminated.
 */
static inline void nxinfo_emit(int win_id_or_neg, const char *s) {
  if (win_id_or_neg < 0)
    printf("%s", s);
  else
    printf_win(win_id_or_neg, "%s", s);
}

/*
 * nxinfo_print_summary - format and emit a readable multi-line system summary.
 *
 * win_id_or_neg: negative -> write to the caller's stdout (printf); otherwise
 *                write into the given compositor window (printf_win).
 *
 * Lines: OS banner, uptime (ms and seconds), process count, desktop resolution,
 * own pid, and current working directory.  All values come from the read-only
 * wrappers above, so the summary is "secure by caller" — it reveals only what
 * the kernel already lets this caller read.  Each line is rendered into a small
 * stack buffer with snprintf() (bounded, no heap) and then routed to the chosen
 * sink, so the formatting is shared between the stdout and windowed paths.
 */
static inline void nxinfo_print_summary(int win_id_or_neg) {
  char line[160];
  char cwd[128];

  long up_ms = nxinfo_uptime_ms();
  int procs = nxinfo_proc_count();
  int w = 0, h = 0;
  nxinfo_display(&w, &h);
  int pid = get_pid();

  snprintf(line, sizeof(line), "%s %s\n", NXINFO_OS_NAME, NXINFO_OS_VERSION);
  nxinfo_emit(win_id_or_neg, line);

  /* Uptime shown both raw (ms) and in whole seconds for quick reading. */
  snprintf(line, sizeof(line), "uptime:     %ld ms (%ld s)\n", up_ms,
           up_ms / 1000);
  nxinfo_emit(win_id_or_neg, line);

  if (procs < 0)
    snprintf(line, sizeof(line), "processes:  <unavailable> (err %d)\n", procs);
  else
    snprintf(line, sizeof(line), "processes:  %d\n", procs);
  nxinfo_emit(win_id_or_neg, line);

  snprintf(line, sizeof(line), "resolution: %dx%d\n", w, h);
  nxinfo_emit(win_id_or_neg, line);

  snprintf(line, sizeof(line), "pid:        %d\n", pid);
  nxinfo_emit(win_id_or_neg, line);

  /* getcwd writes a NUL-terminated path; fall back to "?" on failure so the
   * line is always printable. */
  if (getcwd(cwd, sizeof(cwd)) != 0)
    cwd[0] = '\0';
  snprintf(line, sizeof(line), "cwd:        %s\n", cwd[0] ? cwd : "?");
  nxinfo_emit(win_id_or_neg, line);
}

#endif /* _USER_NXINFO_H */
