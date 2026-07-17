#ifndef _USER_NXJOBS_H
#define _USER_NXJOBS_H

/*
 * user/sys/bin/nxjobs.h
 * NEXS shell job table (ASTRA service layer) — "job control, light".
 *
 * Tracks pids launched in the BACKGROUND (`cmd &`) so `jobs`/`fg` have
 * something to operate on. Deliberately scoped to what's possible without
 * any kernel change:
 *
 *   - background (&), jobs, fg   -> fully supported today. A background
 *     job is just a spawned pid the shell chose not to wait() on
 *     immediately; nxjobs_poll() drains SYS_WAIT non-blockingly each loop
 *     tick to notice when one finishes.
 *
 *   - real suspend (Ctrl-Z) / bg -> NOT supported. There is no STOPPED
 *     process state in the scheduler (syscall_nums.h / sysstats.h expose
 *     only running/zombie), so nothing exists to "resume". `bg` is kept as
 *     a builtin that explains this rather than silently pretending to work
 *     — see nxjobs_bg_unsupported_message() below, used verbatim by the
 *     `bg` builtin in nxshell.c.
 *
 * No exit-status tracking either: SYS_WAIT only reports running/not-running
 * (see lib.c system()'s comment — OS1 does not yet expose a per-process
 * exit code), so nxjobs reports a job as "Done", never "Done (0)"/"Exit 1".
 * That's a kernel-side gap (process exit_code + SYS_WAIT payload), not
 * something this header can paper over correctly.
 */

#include "nxexec.h" /* nxexec_window_stable() — the single shared debounce
                      * also used by nxexec_run_foreground() and nxexec.c's
                      * grace probe, see nxjobs_poll() below. */
#include <os1.h>
#include <string.h>
#include <sys/wait.h> /* WIFEXITED/WEXITSTATUS/WIFSIGNALED — Phase 2 real status */

#define NXJOBS_MAX 16
#define NXJOBS_CMD_MAX 64

enum { NXJOB_RUNNING = 0, NXJOB_DONE = 1, NXJOB_STOPPED = 2 };

struct nxjob {
  int in_use;
  int id; /* 1-based, stable for the job's lifetime (the "%1" in fg %1) */
  int pid;
  int state;
  int status; /* POSIX wait status once NXJOB_DONE (Phase 2): WEXITSTATUS /
               * WIFSIGNALED decode it — "Done (N)" vs "Killed". */
  char cmd[NXJOBS_CMD_MAX];
  /* Debounce state for the "did this background job turn out to be a GUI
   * app?" test in nxjobs_poll() — same shape as nxexec_run_foreground()'s
   * local last_win/stable, just kept per-slot since nxjobs_poll() checks
   * every tracked job on a single shared tick instead of blocking on one. */
  int win_last;
  int win_stable;
};

struct nxjobs {
  struct nxjob slot[NXJOBS_MAX];
  int next_id;
};

static inline void nxjobs_init(struct nxjobs *j) {
  memset(j, 0, sizeof(*j));
  j->next_id = 1;
}

/* nxjobs_add - register a newly-spawned BACKGROUND pid. Returns the job id
 * (>=1), or 0 if the table is full (the process still runs, it's just
 * untracked by `jobs`/`fg` — never a reason to refuse the spawn itself). */
static inline int nxjobs_add(struct nxjobs *j, int pid, const char *cmd) {
  for (int i = 0; i < NXJOBS_MAX; i++) {
    if (!j->slot[i].in_use) {
      j->slot[i].in_use = 1;
      j->slot[i].id = j->next_id++;
      j->slot[i].pid = pid;
      j->slot[i].state = NXJOB_RUNNING;
      j->slot[i].status = 0;
      j->slot[i].win_last = -1;
      j->slot[i].win_stable = 0;
      snprintf(j->slot[i].cmd, NXJOBS_CMD_MAX, "%s", cmd);
      return j->slot[i].id;
    }
  }
  return 0;
}

/* nxjobs_reap - drop a job from the table (finished, or just promoted to
 * the foreground and waited on directly). Defined ahead of nxjobs_poll()
 * since that now calls it directly (windowed-handoff case) rather than only
 * being called from nxshell.c's `fg` path. */
static inline void nxjobs_reap(struct nxjobs *j, int slot) {
  if (slot >= 0 && slot < NXJOBS_MAX)
    j->slot[slot].in_use = 0;
}

/* nxjobs_poll - non-blocking refresh of every tracked job's state. wait()
 * (OS1low_process_wait / SYS_WAIT) is itself non-blocking (-1 = still
 * running), so this is cheap to call once per shell loop tick.
 *
 * A background job ('cmd &') is spawned through the same non-detached path
 * as a foreground one (nxexec_spawn_search(..., detached=0) in nxshell.c),
 * so it never goes through nxexec_run_foreground()'s own window-ownership
 * debounce — nothing ever told nxjobs a backgrounded child turned out to be
 * a GUI app.  Left unchecked, such a job sits in the table as "Running"
 * forever (wait() never returns non-(-1) for it — it only ever exits when
 * the user closes its window, which nxjobs has no way to notice either),
 * a phantom entry that both `jobs` misreports and `fg`/reap can never
 * clean up.  Uses the same nxexec_window_stable() debounce nxexec.h's own
 * watchers use (single source of truth for the threshold/reset rule) on a
 * per-slot basis instead of blocking: once a job's window id is stable
 * across NXEXEC_STABLE_POLLS ticks, it is handed off — reaped from this
 * table without ever appearing as "Done" (it isn't dead, it just left the
 * shell's job-control domain for nxwins/dock's, per nxexec.h's ownership
 * model). */
static inline void nxjobs_poll(struct nxjobs *j) {
  for (int i = 0; i < NXJOBS_MAX; i++) {
    if (!j->slot[i].in_use || j->slot[i].state != NXJOB_RUNNING)
      continue;

    if (nxexec_window_stable(j->slot[i].pid, &j->slot[i].win_last,
                             &j->slot[i].win_stable)) {
      nxjobs_reap(j, i); /* handed off to nxwins/dock; not "Done" */
      continue;
    }

    /* Standard waitpid (Phase 2): WNOHANG poll, real POSIX status. r>0 reaped
     * (status valid), r==-1 gone (reaped elsewhere), r==0 still running. */
    int status = 0;
    int r = waitpid(j->slot[i].pid, &status, WNOHANG);
    if (r != 0) {
      j->slot[i].status = (r > 0) ? status : 0;
      j->slot[i].state = NXJOB_DONE;
    }
  }
}

/* nxjobs_find - look up a job by its %N (or bare N) id. Returns the slot
 * index, or -1 if not found. */
static inline int nxjobs_find(struct nxjobs *j, int id) {
  for (int i = 0; i < NXJOBS_MAX; i++)
    if (j->slot[i].in_use && j->slot[i].id == id)
      return i;
  return -1;
}

/* nxjobs_last - the most recently added still-tracked job's slot, for a
 * bare `fg`/`jobs` with no explicit %N (matches everyday shell habit).
 * Returns -1 if the table is empty. */
static inline int nxjobs_last(struct nxjobs *j) {
  int best = -1;
  for (int i = 0; i < NXJOBS_MAX; i++)
    if (j->slot[i].in_use && (best < 0 || j->slot[i].id > j->slot[best].id))
      best = i;
  return best;
}

/* nxjobs_print - `jobs` builtin body. */
static inline void nxjobs_print(struct nxjobs *j) {
  int any = 0;
  for (int i = 0; i < NXJOBS_MAX; i++) {
    if (!j->slot[i].in_use)
      continue;
    any = 1;
    const char *st;
    char stbuf[24];
    if (j->slot[i].state == NXJOB_RUNNING) {
      st = "Running";
    } else if (j->slot[i].state == NXJOB_STOPPED) {
      st = "Stopped";
    } else { /* NXJOB_DONE: decode the real POSIX status (Phase 2) */
      int s = j->slot[i].status;
      if (WIFSIGNALED(s))
        snprintf(stbuf, sizeof(stbuf), "Killed (%d)", WTERMSIG(s));
      else
        snprintf(stbuf, sizeof(stbuf), "Done (%d)", WEXITSTATUS(s));
      st = stbuf;
    }
    printf("[%d]  %-12s pid=%-6d %s\n", j->slot[i].id, st, j->slot[i].pid,
           j->slot[i].cmd);
  }
  if (!any)
    print("No background jobs.\n");
}

/* nxjobs_parse_id - parse a `fg`/`bg` argument in either "%3" or "3" form.
 * Returns the id, or -1 if arg isn't a valid job reference. */
static inline int nxjobs_parse_id(const char *arg) {
  if (!arg || !*arg)
    return -1;
  if (arg[0] == '%')
    arg++;
  if (!*arg)
    return -1;
  int id = atoi(arg);
  return id > 0 ? id : -1;
}

/* nxjobs_stop - suspend a job (Ctrl-Z / `stop %N`): kernel PROC_STOPPED via
 * OS1_process_stop, then mark it Stopped so `jobs` reflects it and `bg`/`fg`
 * can resume it (Phase 2 — a real stopped state exists now). */
static inline int nxjobs_stop(struct nxjobs *j, int slot) {
  if (slot < 0 || slot >= NXJOBS_MAX || !j->slot[slot].in_use)
    return -1;
  int r = OS1_process_stop(j->slot[slot].pid);
  if (r == 0)
    j->slot[slot].state = NXJOB_STOPPED;
  return r;
}

/* nxjobs_cont - resume a stopped job in the BACKGROUND (`bg`): OS1_process_cont
 * + mark Running.  For `fg`, the caller conts and then waits in the foreground.
 * Returns 0, or a negative errno (e.g. the job was not stopped). */
static inline int nxjobs_cont(struct nxjobs *j, int slot) {
  if (slot < 0 || slot >= NXJOBS_MAX || !j->slot[slot].in_use)
    return -1;
  int r = OS1_process_cont(j->slot[slot].pid);
  if (r == 0 && j->slot[slot].state == NXJOB_STOPPED)
    j->slot[slot].state = NXJOB_RUNNING;
  return r;
}

#endif /* _USER_NXJOBS_H */