#ifndef OS1_TIME_SERVICE_H
#define OS1_TIME_SERVICE_H

#include <os1.h>

#define OS1_TIME_SERVICE_VERSION UINT32_C(4)
#define OS1_TIME_SERVICE_REQUEST_SNAPSHOT UINT64_C(1)
#define OS1_TIME_SERVICE_REQUEST_TIMER_STATS UINT64_C(2)
#define OS1_TIME_SERVICE_IPC_TYPE 0x210
/* Security/liveness policy: rotate a supervised service generation after a
 * finite number of completed receive epochs.  This bounds latent per-process
 * state even if a future request handler regresses and leaks a resource. */
#define OS1_TIME_SERVICE_EPOCH_REQUEST_LIMIT UINT32_C(4096)

/* Stable service data model. The kernel TIME wire ABI version is private to
 * nxtimerd; applications do not depend on revision-suffixed contract structs.
 * Changes to kernel wire transport are translated by the privileged service. */
struct os1_time_service_timer_cpu {
  uint32_t cpu_id;
  uint64_t arm_count;
  uint64_t cancel_count;
  uint64_t fire_count;
  uint64_t scan_count;
  uint64_t scan_pass_count;
  uint32_t queue_depth;
  uint32_t queue_high_water;
  uint32_t max_scan_per_pass;
  uint32_t max_fire_per_pass;
  uint64_t late_fire_count;
  uint64_t fire_lateness_ns_total;
  uint64_t max_fire_lateness_ns;
  uint64_t clockevent_late_count;
  uint64_t clockevent_lateness_ns_total;
  uint64_t max_clockevent_lateness_ns;
  uint64_t absolute_program_count;
  uint64_t irq_mask_interval_count;
  uint64_t irq_mask_total_ns;
  uint64_t irq_mask_max_ns;
  uint64_t irq_mask_invalid_sample_count;
  uint64_t irq_mask_active_ns;
  uint32_t irq_mask_flags;
};

struct os1_time_service_timer_stats {
  uint32_t cpu_count;
  uint64_t sequence;
  uint64_t monotonic_ns;
  struct os1_time_service_timer_cpu cpu[OS1_TIME_MAX_CPUS];
};

/* Fetch one complete, sanitized scheduler/time snapshot through OS1nx_time.
 * The privileged daemon owns the underlying OS1_NS_TIME capability; clients do
 * not acquire hardware-facing timer objects directly. */
int OS1_time_service_snapshot(struct os1_time_snapshot_v1 *snapshot);
int OS1_time_service_timer_stats(
    struct os1_time_service_timer_stats *snapshot);

#endif /* OS1_TIME_SERVICE_H */
