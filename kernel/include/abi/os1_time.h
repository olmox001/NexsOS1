/* Generated from kernel/contracts/time-v1.json. Do not edit. */
#ifndef OS1_TIME_ABI_H
#define OS1_TIME_ABI_H

#include <stdint.h>

#define OS1_TIME_ABI_VERSION UINT32_C(1)
#define OS1_TIMER_STATS_ABI_VERSION UINT32_C(3)
#define OS1_TIME_MAX_CPUS UINT32_C(64)
#define OS1_TIME_CPU_ONLINE UINT32_C(0x00000001)
#define OS1_TIME_CPU_ARMED UINT32_C(0x00000002)
#define OS1_TIME_CPU_SYNCHRONIZED UINT32_C(0x00000004)
#define OS1_TIME_OBJECT_SYSTEM "system"
#define OS1_TIME_OBJECT_STATS "stats"

struct os1_time_cpu_v1 {
  uint32_t cpu_id;
  uint32_t flags;
  uint64_t sequence;
  uint64_t counter;
  uint64_t monotonic_ns;
  uint64_t next_deadline_ns;
  uint64_t tick_count;
  uint64_t late_tick_count;
};

struct os1_time_snapshot_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t cpu_count;
  uint32_t cpu_record_size;
  uint64_t sequence;
  uint64_t monotonic_ns;
  uint64_t counter_frequency_hz;
  uint64_t scheduler_tick_hz;
  struct os1_time_cpu_v1 cpu[OS1_TIME_MAX_CPUS];
};

struct os1_timer_cpu_stats_v3 {
  uint32_t cpu_id;
  uint32_t reserved;
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
};

struct os1_timer_stats_snapshot_v3 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t cpu_count;
  uint32_t cpu_record_size;
  uint64_t sequence;
  uint64_t monotonic_ns;
  struct os1_timer_cpu_stats_v3 cpu[OS1_TIME_MAX_CPUS];
};

#endif /* OS1_TIME_ABI_H */
