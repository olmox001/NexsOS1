#ifndef _USER_NXMEMSTAT_H
#define _USER_NXMEMSTAT_H

/*
 * user/sys/bin/nxmemstat.h
 * NEXS stratified system-statistics helper (ASTRA service layer).
 *
 * Reusable header-only helper (all routines `static inline`) over the
 * privileged OS1_sys_stats() syscall, mirroring nxproc.h.  SECURITY MODEL: no
 * ambient checks of its own — OS1_sys_stats() is gated to ROOT/machine in the
 * kernel, so only an NX service (this runs as /sys/bin/nxmemstat at ROOT) can
 * read the raw stats; USER apps consume the service's GUI, never the syscall.
 *
 * Two surfaces:
 *   - GUI: nxmemstat_render_if_changed() draws a compositor WINDOW (virtio UI),
 *     redrawing only when a MEANINGFUL value changed (the signature
 * deliberately EXCLUDES the always-incrementing cumulative counters — ctx
 * switches, alloc /free calls, search-ns total, uptime — so the window does not
 * "repeat" every tick; those are shown as derived rates but do not force a
 * redraw).
 *   - Log: nxmemstat_csv_line() formats one MEMSTAT CSV record for a headless
 *     serial capture consumed by tools/analyze_drift.py.
 */

#include <os1.h>

/* nxmemstat_snapshot - fill *out with one stats snapshot.  Returns >0 (bytes)
 * on success, or a negative errno (-EPERM if the caller is not ROOT/machine).
 */
static inline long nxmemstat_snapshot(struct os1_sysstats *out) {
  return OS1_sys_stats(out);
}

/* Signature over the SLOWLY-changing, user-meaningful fields only.  Page counts
 * are bucketed to MB so ordinary churn of a few pages does not force a redraw;
 * the cumulative rate counters are intentionally absent. */
static inline unsigned long nxmemstat_signature(const struct os1_sysstats *s) {
  unsigned long h = 1469598103934665603UL; /* FNV-1a basis */
#define MIX(v) (h = (h ^ (unsigned long)(v)) * 1099511628211UL)
  MIX(s->pmm_free_pages >> 8);         /* ~MB buckets */
  MIX(s->pmm_largest_contig_run >> 8); /* ~MB buckets */
  MIX(s->pmm_free_run_count);
  MIX(s->km_bytes_in_use >> 10); /* KB buckets */
  MIX(s->km_live_allocs);
  MIX(s->sched_nproc);
  MIX(s->sched_zombie_count);
  MIX(s->sched_runnable);
  for (int i = 0; i < OBJ_TYPE_COUNT; i++)
    MIX(s->obj_live_by_type[i]);
#undef MIX
  return h;
}

/* nxmemstat_render - draw the stats into window win_id.  prev/dt_ms give the
 * previous sample and elapsed ms so per-interval rates can be shown; pass
 * prev==NULL (or dt_ms==0) on the first frame to omit rates. */
static inline void nxmemstat_render(int win_id, const struct os1_sysstats *s,
                                    const struct os1_sysstats *prev,
                                    unsigned long dt_ms) {
  unsigned long free_mb = (unsigned long)((s->pmm_free_pages * 4UL) / 1024UL);
  unsigned long tot_mb = (unsigned long)((s->pmm_total_pages * 4UL) / 1024UL);
  unsigned long lcr_mb =
      (unsigned long)((s->pmm_largest_contig_run * 4UL) / 1024UL);

  unsigned long csw_rate = 0, a_rate = 0, mean_srch = 0;
  if (prev && dt_ms > 0) {
    csw_rate =
        (unsigned long)((s->sched_ctx_switches - prev->sched_ctx_switches) *
                        1000UL / dt_ms);
    unsigned long da =
        (unsigned long)(s->pmm_alloc_calls - prev->pmm_alloc_calls);
    a_rate = da * 1000UL / dt_ms;
    if (da)
      mean_srch = (unsigned long)((s->pmm_alloc_search_ns_total -
                                   prev->pmm_alloc_search_ns_total) /
                                  da);
  }

  _sys_window_write(win_id, "\033[H\033[J", 6); /* clear */
  _sys_window_write(win_id, "\033[1;34m", 7);   /* bold Blue */
  printf_win(win_id, "NEXS System Monitor            uptime %lus\n",
             (unsigned long)(s->uptime_ns / 1000000000ULL));
  _sys_window_write(win_id, "\033[0m", 4);
  _sys_window_write(win_id, "--------------------------------------------\n",
                    45);
  printf_win(win_id, "CPU    : %lu online   runnable %lu   zombies %lu\n",
             (unsigned long)s->sched_ncpu, (unsigned long)s->sched_runnable,
             (unsigned long)s->sched_zombie_count);
  printf_win(win_id, "ctxsw  : %lu total   (~%lu/s)\n",
             (unsigned long)s->sched_ctx_switches, csw_rate);
  printf_win(win_id, "PMM    : free %lu / %lu MB   used %lu MB\n", free_mb,
             tot_mb, tot_mb - free_mb);
  printf_win(win_id, "frag   : largest %lu MB (%lu pg)   runs %lu\n", lcr_mb,
             (unsigned long)s->pmm_largest_contig_run,
             (unsigned long)s->pmm_free_run_count);
  printf_win(win_id, "alloc  : %lu calls (~%lu/s)   search ~%lu ns (max %lu)\n",
             (unsigned long)s->pmm_alloc_calls, a_rate, mean_srch,
             (unsigned long)s->pmm_alloc_search_ns_max);
  printf_win(win_id,
             "kheap  : in-use %lu KB  hi %lu KB  live %lu  pool %lu KB\n",
             (unsigned long)(s->km_bytes_in_use / 1024UL),
             (unsigned long)(s->km_high_water_bytes / 1024UL),
             (unsigned long)s->km_live_allocs,
             (unsigned long)(s->km_heap_total_bytes / 1024UL));
  printf_win(win_id, "objs   : FILE %lu  PROC %lu  REGKEY %lu  WINDOW %lu\n",
             (unsigned long)s->obj_live_by_type[OBJ_TYPE_FILE],
             (unsigned long)s->obj_live_by_type[OBJ_TYPE_PROCESS],
             (unsigned long)s->obj_live_by_type[OBJ_TYPE_REGKEY],
             (unsigned long)s->obj_live_by_type[OBJ_TYPE_WINDOW]);
}

/* nxmemstat_render_if_changed - snapshot, redraw the window only when the
 * meaningful signature changed.  *prev holds the last snapshot (for rates);
 * *last_sig the last signature; dt_ms the loop cadence.  Returns 1 if redrawn,
 * 0 if skipped, negative on syscall error. */
static inline int nxmemstat_render_if_changed(int win_id,
                                              struct os1_sysstats *prev,
                                              unsigned long *last_sig,
                                              unsigned long dt_ms) {
  struct os1_sysstats cur;
  long r = nxmemstat_snapshot(&cur);
  if (r < 0) {
    unsigned long err_sig = ~0UL;
    if (last_sig && *last_sig != err_sig) {
      _sys_window_write(win_id, "OS1_sys_stats denied (need ROOT) or failed\n",
                        43);
      *last_sig = err_sig;
    }
    return (int)r;
  }
  unsigned long sig = nxmemstat_signature(&cur);
  int had_prev = (last_sig && *last_sig != 0 && *last_sig != ~0UL);
  if (last_sig && *last_sig == sig) {
    *prev = cur; /* keep rates fresh even when we skip the redraw */
    return 0;
  }
  nxmemstat_render(win_id, &cur, had_prev ? prev : (struct os1_sysstats *)0,
                   dt_ms);
  if (prev)
    *prev = cur;
  if (last_sig)
    *last_sig = sig;
  return 1;
}

/* nxmemstat_csv_line - format one MEMSTAT CSV record (headless --log mode),
 * matching tools/analyze_drift.py's column order.  't_s' is seconds since the
 * logger started; 'cycles' is not known to the service, emitted as 0. */
static inline int nxmemstat_csv_line(char *buf, int size,
                                     const struct os1_sysstats *s,
                                     unsigned long t_s) {
  return snprintf(
      buf, size,
      "MEMSTAT,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
      "%lu,%lu,0\n",
      t_s, (unsigned long)s->pmm_free_pages,
      (unsigned long)s->pmm_largest_contig_run,
      (unsigned long)s->pmm_free_run_count, (unsigned long)s->pmm_alloc_calls,
      (unsigned long)s->pmm_free_calls,
      (unsigned long)s->pmm_alloc_search_ns_total,
      (unsigned long)s->pmm_alloc_search_ns_max,
      (unsigned long)s->km_bytes_in_use, (unsigned long)s->km_high_water_bytes,
      (unsigned long)s->km_live_allocs, (unsigned long)s->sched_ctx_switches,
      (unsigned long)s->sched_runnable, (unsigned long)s->sched_zombie_count,
      (unsigned long)s->obj_live_by_type[OBJ_TYPE_FILE],
      (unsigned long)s->obj_live_by_type[OBJ_TYPE_PROCESS],
      (unsigned long)s->obj_live_by_type[OBJ_TYPE_REGKEY],
      (unsigned long)s->obj_live_by_type[OBJ_TYPE_WINDOW]);
}

#endif /* _USER_NXMEMSTAT_H */
