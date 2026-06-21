/*
 * include/api/sysstats.h
 * System statistics ABI — the single source of truth shared by the kernel
 * (kernel/sched/process.c: sys_sysstats) and userland (os1.h, /bin/memstat),
 * exactly like object.h / ps_info.
 *
 * This is the low-overhead instrumentation surface for the progressive-slowdown
 * campaign (perf brief §1).  A single OS1_sys_stats() syscall (SYS_SYSSTATS,
 * high-level OS1_ introspection namespace, sibling of SYS_GETPROCS/
 * SYS_GET_IDENTITY) copies ONE struct os1_sysstats snapshot to userland; a
 * userspace poller (/bin/memstat) diffs successive snapshots to derive rates,
 * per-interval mean latency, and monotonic-drift signals.  No per-tick console
 * printing — the snapshot is pulled on demand to avoid the console-contention
 * Heisenbug.
 *
 * NO HARDCODED SYSTEM DIMENSIONS (capability / real-source-of-truth model):
 * this struct never invents its own CPU count or object-type count.
 *   - The CPU count reported (sched_ncpu) is the REAL detected count (kernel
 *     `nr_cpus`, set at SMP bring-up).  Scheduler load is reported as
 *     aggregates (runnable total + deepest single-CPU runqueue) rather than a
 *     fixed-size per-CPU array, so the ABI layout depends on NO CPU-count
 *     constant at all — adding cores never changes the struct.
 *   - obj_live_by_type[] is dimensioned by OBJ_TYPE_COUNT from object.h (the
 *     real object-type source of truth), not a private copy.
 *
 * Counter conventions:
 *   - *_calls / *_switches / *_ns_total are CUMULATIVE since boot; the poller
 *     subtracts successive samples to get per-interval rates and means.
 *   - *_ns_max / *_high_water_bytes are running maxima since boot.
 *   - largest_contig_run / free_run_count / zombie_count / runnable /
 *     runq_max / live_by_type are INSTANTANEOUS snapshots at collection time.
 *
 * Types: deliberately uses unsigned long long (8 B) / unsigned int (4 B) only,
 * so the header needs no stdint dependency and has identical layout on both
 * LP64 targets (x86_64-elf, aarch64-none-elf).  Versioned (version +
 * struct_size) so the struct can grow without breaking older pollers.
 */
#ifndef NEXS_API_SYSSTATS_H
#define NEXS_API_SYSSTATS_H

#include "object.h" /* OBJ_TYPE_COUNT — real object-type source of truth */

#define OS1_SYSSTATS_VERSION 1

struct os1_sysstats {
  unsigned int version;     /* = OS1_SYSSTATS_VERSION                            */
  unsigned int struct_size; /* = sizeof(struct os1_sysstats), forward-compat     */
  unsigned long long uptime_ns; /* monotonic ns since boot (rate denominator)    */

  /* --- PMM (page-frame allocator), page-granular --- */
  unsigned long long pmm_total_pages;
  unsigned long long pmm_free_pages;
  unsigned long long pmm_largest_contig_run;     /* pages — fragmentation signal */
  unsigned long long pmm_free_run_count;         /* distinct free runs — frag.    */
  unsigned long long pmm_alloc_calls;            /* cumulative                    */
  unsigned long long pmm_free_calls;             /* cumulative                    */
  unsigned long long pmm_alloc_search_ns_total;  /* cumulative bitmap-search time */
  unsigned long long pmm_alloc_search_ns_max;    /* worst single search (ns)      */

  /* --- kmalloc (kernel heap) --- */
  unsigned long long km_heap_total_bytes;        /* pool size (never shrinks yet) */
  unsigned long long km_bytes_in_use;            /* live user bytes               */
  unsigned long long km_high_water_bytes;        /* peak bytes_in_use             */
  unsigned long long km_live_allocs;             /* live allocations              */

  /* --- scheduler (aggregates over the REAL online CPUs, no fixed array) --- */
  unsigned long long sched_ctx_switches; /* cumulative real switches             */
  unsigned int sched_nproc;              /* active process slots                 */
  unsigned int sched_zombie_count;       /* DEAD/ZOMBIE not yet reaped           */
  unsigned int sched_ncpu;               /* REAL online CPUs (kernel nr_cpus)    */
  unsigned int sched_runnable;           /* READY+RUNNING processes (load)       */
  unsigned int sched_runq_max;           /* deepest single-CPU runqueue (imbalance) */
  unsigned int _pad0;                    /* keep following ull array 8-aligned    */

  /* --- object/handle manager (capability layer) --- */
  unsigned long long obj_live_by_type[OBJ_TYPE_COUNT]; /* live kobjects per type */
};

#endif /* NEXS_API_SYSSTATS_H */
