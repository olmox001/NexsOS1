#ifndef OS1LIBKERNEL_TIMER_H
#define OS1LIBKERNEL_TIMER_H

#include <hal/frame.h>
#include <kernel/list.h>
#include <kernel/types.h>
#include <os1_time.h>

#define OS1LIBKERNEL_TIMER_HZ UINT64_C(1000)
#define OS1LIBKERNEL_NSEC_PER_SEC UINT64_C(1000000000)
#define OS1LIBKERNEL_NSEC_PER_MSEC UINT64_C(1000000)
#define OS1LIBKERNEL_NSEC_PER_TICK \
  (OS1LIBKERNEL_NSEC_PER_SEC / OS1LIBKERNEL_TIMER_HZ)

typedef void (*OS1libkernel_timer_callback_t)(void *data);

struct OS1libkernel_timer {
  struct list_head list;
  uint64_t deadline_ns;
  OS1libkernel_timer_callback_t callback;
  void *data;
  bool pending;
  int cpu;
};

struct OS1libkernel_timer_stats {
  uint64_t arm_count;
  uint64_t cancel_count;
  uint64_t fire_count;
  uint64_t scan_count;
  uint64_t scan_pass_count;
  uint64_t late_fire_count;
  uint64_t fire_lateness_ns_total;
  uint64_t max_fire_lateness_ns;
  uint64_t clockevent_late_count;
  uint64_t clockevent_lateness_ns_total;
  uint64_t max_clockevent_lateness_ns;
  uint64_t absolute_program_count;
  uint32_t queue_depth;
  uint32_t queue_high_water;
  uint32_t max_scan_per_pass;
  uint32_t max_fire_per_pass;
};

void OS1libkernel_time_init(void);
void OS1libkernel_timer_core_init(void);
uint64_t OS1libkernel_time_counter_read(void);
uint64_t OS1libkernel_time_monotonic_ns(void);
int OS1libkernel_time_deadline_after_ns(uint64_t delta_ns,
                                        uint64_t *deadline_ns);
bool OS1libkernel_time_deadline_reached(uint64_t deadline_ns);
uint64_t OS1libkernel_time_counts_to_ns(uint64_t counts);
uint64_t OS1libkernel_time_counter_frequency(void);
int OS1libkernel_time_snapshot(struct os1_time_snapshot_v1 *out);

uint64_t OS1libkernel_timer_percpu_arm(void);
uint64_t OS1libkernel_timer_percpu_tick(void);
void OS1libkernel_timer_reschedule_local(void);
hal_frame_t *OS1libkernel_timer_tick(hal_frame_t *frame);

void OS1libkernel_timer_setup(struct OS1libkernel_timer *timer,
                              OS1libkernel_timer_callback_t callback,
                              void *data);
void OS1libkernel_timer_arm_deadline(struct OS1libkernel_timer *timer,
                                     uint64_t deadline_ns);
void OS1libkernel_timer_cancel(struct OS1libkernel_timer *timer);
bool OS1libkernel_timer_pending(const struct OS1libkernel_timer *timer);
int OS1libkernel_timer_stats_snapshot(
    uint32_t cpu_id, struct OS1libkernel_timer_stats *out);
int OS1libkernel_timer_stats_export(
    struct os1_timer_stats_snapshot_v4 *out);

#endif /* OS1LIBKERNEL_TIMER_H */
