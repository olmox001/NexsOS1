/*
 * user/bin/memstat.c
 * Userspace poller for the kernel statistics surface (perf brief §1).
 *
 * Polls OS1_sys_stats() at ~1 Hz and prints ONE compact line WHEN something
 * changed (print-on-change) — never every tick, to avoid the console-contention
 * Heisenbug the brief warns about (§1).  Successive snapshots are diffed to
 * derive the per-interval signals the drift analysis needs: PMM alloc/free
 * rates, MEAN and MAX bitmap-search latency (the fragmentation-sensitive cost),
 * largest contiguous free run + free-run count (fragmentation), kmalloc live /
 * high-water bytes, context-switch rate (the per-switch TLB-flush-tax
 * denominator), runnable load, zombie count, and live kobjects per type.
 *
 * 64-bit stat fields are unsigned long long; printed via %lu after a cast to
 * unsigned long (== 64-bit on both LP64 targets) since the libos1 vsnprintf
 * supports the 'l' length modifier but not necessarily 'll'.
 */
#include <os1.h>

int main(void) {
  struct os1_sysstats cur, prev;
  int have_prev = 0;

  print("[memstat] polling OS1_sys_stats (print-on-change, ~1Hz)\n");

  for (;;) {
    long r = OS1_sys_stats(&cur);
    if (r < 0) {
      print("[memstat] OS1_sys_stats failed\n");
      OS1_sleep(1000);
      continue;
    }
    if (cur.version != OS1_SYSSTATS_VERSION) {
      printf("[memstat] WARN version=%lu (expected %d), struct=%lu bytes\n",
             (unsigned long)cur.version, OS1_SYSSTATS_VERSION,
             (unsigned long)cur.struct_size);
    }

    if (!have_prev) {
      printf("[memstat] ncpu=%lu total_pages=%lu free=%lu heap_total=%luKB "
             "structsz=%lu bytes (read %ld)\n",
             (unsigned long)cur.sched_ncpu,
             (unsigned long)cur.pmm_total_pages,
             (unsigned long)cur.pmm_free_pages,
             (unsigned long)(cur.km_heap_total_bytes / 1024),
             (unsigned long)cur.struct_size, r);
      prev = cur;
      have_prev = 1;
      OS1_sleep(1000);
      continue;
    }

    int changed = (cur.pmm_alloc_calls != prev.pmm_alloc_calls) ||
                  (cur.sched_ctx_switches != prev.sched_ctx_switches) ||
                  (cur.pmm_free_pages != prev.pmm_free_pages) ||
                  (cur.km_bytes_in_use != prev.km_bytes_in_use) ||
                  (cur.obj_live_by_type[OBJ_TYPE_PROCESS] !=
                   prev.obj_live_by_type[OBJ_TYPE_PROCESS]);

    if (changed) {
      unsigned long d_alloc =
          (unsigned long)(cur.pmm_alloc_calls - prev.pmm_alloc_calls);
      unsigned long d_free =
          (unsigned long)(cur.pmm_free_calls - prev.pmm_free_calls);
      unsigned long d_csw =
          (unsigned long)(cur.sched_ctx_switches - prev.sched_ctx_switches);
      unsigned long long d_searchns =
          cur.pmm_alloc_search_ns_total - prev.pmm_alloc_search_ns_total;
      unsigned long mean_searchns =
          d_alloc ? (unsigned long)(d_searchns / d_alloc) : 0UL;

      printf("[memstat] free=%lu lcr=%lu runs=%lu | a+%lu f+%lu srch~%luns "
             "max=%luns | kmUse=%luKB kmHi=%luKB live=%lu | csw+%lu run=%lu "
             "zomb=%lu | obj F=%lu P=%lu R=%lu W=%lu\n",
             (unsigned long)cur.pmm_free_pages,
             (unsigned long)cur.pmm_largest_contig_run,
             (unsigned long)cur.pmm_free_run_count, d_alloc, d_free,
             mean_searchns, (unsigned long)cur.pmm_alloc_search_ns_max,
             (unsigned long)(cur.km_bytes_in_use / 1024),
             (unsigned long)(cur.km_high_water_bytes / 1024),
             (unsigned long)cur.km_live_allocs, d_csw,
             (unsigned long)cur.sched_runnable,
             (unsigned long)cur.sched_zombie_count,
             (unsigned long)cur.obj_live_by_type[OBJ_TYPE_FILE],
             (unsigned long)cur.obj_live_by_type[OBJ_TYPE_PROCESS],
             (unsigned long)cur.obj_live_by_type[OBJ_TYPE_REGKEY],
             (unsigned long)cur.obj_live_by_type[OBJ_TYPE_WINDOW]);
    }

    prev = cur;
    OS1_sleep(1000);
  }
  return 0;
}
