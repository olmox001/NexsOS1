#ifndef _USER_NXJOBS_H
#define _USER_NXJOBS_H

/*
 * user/sys/bin/nxjobs.h
 * NEXS shell job table (ASTRA service layer) — "job control, light".
 *
 * Tracks pids launched in the BACKGROUND (`cmd &`) and jobs suspended with
 * Ctrl-Z, so `jobs`/`fg`/`bg` have something to operate on.
 *
 *   - background (&), jobs, fg   A background job is a spawned pid the shell
 *     chose not to wait() on immediately; nxjobs_poll() drains SYS_WAIT
 *     non-blockingly each loop tick to notice when one finishes.
 *
 *   - suspend (Ctrl-Z), bg, fg   REAL since Phase 2: the scheduler has a
 *     PROC_STOPPED state with process_stop/process_cont behind the
 *     DESTROY-gated OBJ_CTL_STOP/CONT verbs, so a stopped job genuinely
 *     stops and genuinely resumes.
 *
 *   - exit status                REAL since Phase 2/9b: waitpid() carries a
 *     POSIX status that survives reaping, so a job reports "Done (N)" or
 *     "Killed (N)", not a bare "Done".
 *
 * (This header used to state the opposite for the last two — written before
 * the kernel work landed and never updated.  The stale text was quoted back
 * as fact by a later review, so it is corrected here rather than deleted:
 * PLAN-2026-07-17 §"Known live bugs" records that failure mode.)
 *
 * Job SELECTION follows POSIX XCU §2.9.3.1 — see nxjobs_resolve().
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
  /* POSIX current/previous job (`%%`/`%+` and `%-`, and what a bare `fg`/`bg`
   * operates on).  Stored as IDS and updated on the EVENTS that define them,
   * because "most recently stopped" is not recoverable from the table's state:
   * with two stopped jobs, nothing in the slots says which stopped last. */
  int current_id;
  int previous_id;
};

static inline void nxjobs_init(struct nxjobs *j) {
  memset(j, 0, sizeof(*j));
  j->next_id = 1;
}

/* __nxjobs_slot_of_id - slot for `id`, but only if it still names a job that
 * can be foregrounded.  A finished job is deliberately NOT eligible: it is
 * about to leave the table, and selecting a corpse is not a selection. */
static inline int __nxjobs_slot_of_id(struct nxjobs *j, int id) {
  if (id <= 0)
    return -1;
  for (int i = 0; i < NXJOBS_MAX; i++)
    if (j->slot[i].in_use && j->slot[i].id == id &&
        j->slot[i].state != NXJOB_DONE)
      return i;
  return -1;
}

/* __nxjobs_pick - the POSIX current-job RULE, applied from scratch.
 *
 * "the most recently stopped job, or, if there are no stopped jobs, the most
 * recently started background job".  `skip_id` asks for the next one down,
 * which is how the previous job is chosen.  Used only to REBUILD the ids after
 * the job they named went away — the event-driven path above is what makes
 * "most recently stopped" mean the right thing while both jobs are stopped. */
static inline int __nxjobs_pick(struct nxjobs *j, int skip_id) {
  for (int pass = 0; pass < 2; pass++) {
    int want = (pass == 0) ? NXJOB_STOPPED : NXJOB_RUNNING;
    int best = -1;
    for (int i = 0; i < NXJOBS_MAX; i++) {
      if (!j->slot[i].in_use || j->slot[i].state != want)
        continue;
      if (j->slot[i].id == skip_id)
        continue;
      if (best < 0 || j->slot[i].id > j->slot[best].id)
        best = i;
    }
    if (best >= 0)
      return best;
  }
  return -1;
}

/* __nxjobs_resync - drop current/previous ids that no longer name an eligible
 * job and refill them from the rule.  Called before every resolution, so a job
 * finishing or being disowned cannot leave `fg` pointing at nothing. */
static inline void __nxjobs_resync(struct nxjobs *j) {
  if (__nxjobs_slot_of_id(j, j->current_id) < 0)
    j->current_id = 0;
  if (__nxjobs_slot_of_id(j, j->previous_id) < 0)
    j->previous_id = 0;
  if (j->current_id == 0) {
    int s = __nxjobs_pick(j, j->previous_id);
    j->current_id = (s >= 0) ? j->slot[s].id : 0;
  }
  if (j->previous_id == 0) {
    int s = __nxjobs_pick(j, j->current_id);
    j->previous_id = (s >= 0) ? j->slot[s].id : 0;
  }
}

/* __nxjobs_promote - `id` becomes the current job, the old current becomes the
 * previous one.  This is the only way current_id is set on the happy path: it
 * is called when a job is started in the background and when one is stopped,
 * which are exactly the two events POSIX defines the current job by. */
static inline void __nxjobs_promote(struct nxjobs *j, int id) {
  if (id <= 0 || id == j->current_id)
    return;
  j->previous_id = j->current_id;
  j->current_id = id;
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
      __nxjobs_promote(j, j->slot[i].id); /* POSIX: a new background job is
                                           * the current job */
      return j->slot[i].id;
    }
  }
  return 0;
}

/* nxjobs_mark_stopped - record that a job has been suspended.
 *
 * Separate from nxjobs_stop() because Ctrl-Z is detected by the foreground
 * watcher, which has already suspended the process — only the bookkeeping is
 * left.  Both paths must come through here, though, because a stopped job
 * becomes the CURRENT job, and a shell where Ctrl-Z did not change what a bare
 * `fg` resumes is the one thing job control has to get right. */
static inline void nxjobs_mark_stopped(struct nxjobs *j, int slot) {
  if (slot < 0 || slot >= NXJOBS_MAX || !j->slot[slot].in_use)
    return;
  j->slot[slot].state = NXJOB_STOPPED;
  __nxjobs_promote(j, j->slot[slot].id);
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

/* nxjobs_print - `jobs` builtin body.
 *
 * The current job is marked '+' and the previous one '-', as POSIX specifies:
 * without them the output does not say what a bare `fg` would act on, which is
 * the only reason a user needs to read it before typing one. */
static inline void nxjobs_print(struct nxjobs *j) {
  int any = 0;
  __nxjobs_resync(j);
  for (int i = 0; i < NXJOBS_MAX; i++) {
    if (!j->slot[i].in_use)
      continue;
    any = 1;
    char mark = (j->slot[i].id == j->current_id)    ? '+'
                : (j->slot[i].id == j->previous_id) ? '-'
                                                    : ' ';
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
    printf("[%d]%c %-12s pid=%-6d %s\n", j->slot[i].id, mark, st,
           j->slot[i].pid, j->slot[i].cmd);
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

/* __nxjobs_match - most recent job whose command matches `s`, by prefix or
 * (with `anywhere`) by substring.  POSIX `%string` and `%?string`. */
static inline int __nxjobs_match(struct nxjobs *j, const char *s, int anywhere) {
  if (!s || !*s)
    return -1;
  size_t len = strlen(s);
  int best = -1;
  for (int i = 0; i < NXJOBS_MAX; i++) {
    if (!j->slot[i].in_use)
      continue;
    int hit = anywhere ? (strstr(j->slot[i].cmd, s) != (char *)0)
                       : (strncmp(j->slot[i].cmd, s, len) == 0);
    if (hit && (best < 0 || j->slot[i].id > j->slot[best].id))
      best = i;
  }
  return best;
}

/*
 * nxjobs_resolve - turn a `fg`/`bg`/`disown` operand into a slot index.
 *
 * POSIX job_id syntax (XCU §2.9.3.1), which this now actually implements:
 *
 *   (omitted)  the CURRENT job
 *   %% or %+   the current job          %-        the previous job
 *   %N         job number N             %string   job whose command starts
 *   %?string   ...whose command contains string     with string
 *
 * TWO CORRECTIONS, 2026-07-23:
 *
 * 1. A bare `fg` used to take "the highest job id still in the table"
 *    (nxjobs_last).  That is not the current job and, worse, the table
 *    includes FINISHED jobs — so `fg` could resume nothing, and Ctrl-Z'ing a
 *    job did not change what a bare `fg` would bring back.  It now follows the
 *    real rule: most recently stopped, else most recently backgrounded, never
 *    a finished one.  Omitting the operand IS standard; picking the wrong job
 *    for it was not.
 *
 * 2. A BARE NAME (`fg lua`, no `%`) used to resolve by command prefix.  That
 *    is not a job id in any shell — `lua` there is a command name, and
 *    accepting it means `fg` silently guesses when the user meant something
 *    else.  Removed; `fg %lua` is the spelling.  A bare NUMBER stays: `fg 1`
 *    for `%1` is the one extension every shell accepts.
 *
 * Returns the slot, or -1 if the operand names no job.
 */
static inline int nxjobs_resolve(struct nxjobs *j, const char *arg) {
  __nxjobs_resync(j);

  if (!arg || !*arg)
    return __nxjobs_slot_of_id(j, j->current_id);

  if (arg[0] != '%') {
    for (const char *p = arg; *p; p++)
      if (*p < '0' || *p > '9')
        return -1; /* a bare name is a command, not a job id */
    return nxjobs_find(j, atoi(arg));
  }

  arg++;
  if (!*arg)
    return -1; /* a lone "%" names nothing */
  if (arg[0] == '%' || arg[0] == '+')
    return __nxjobs_slot_of_id(j, j->current_id);
  if (arg[0] == '-')
    return __nxjobs_slot_of_id(j, j->previous_id);
  if (arg[0] == '?')
    return __nxjobs_match(j, arg + 1, 1);

  int numeric = 1;
  for (const char *p = arg; *p; p++)
    if (*p < '0' || *p > '9') {
      numeric = 0;
      break;
    }
  if (numeric)
    return nxjobs_find(j, atoi(arg));
  return __nxjobs_match(j, arg, 0);
}

/* nxjobs_stop - suspend a job (Ctrl-Z / `stop %N`): kernel PROC_STOPPED via
 * OS1_process_stop, then mark it Stopped so `jobs` reflects it and `bg`/`fg`
 * can resume it (Phase 2 — a real stopped state exists now). */
static inline int nxjobs_stop(struct nxjobs *j, int slot) {
  if (slot < 0 || slot >= NXJOBS_MAX || !j->slot[slot].in_use)
    return -1;
  int r = OS1_process_stop(j->slot[slot].pid);
  if (r == 0)
    nxjobs_mark_stopped(j, slot); /* also makes it the current job */
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