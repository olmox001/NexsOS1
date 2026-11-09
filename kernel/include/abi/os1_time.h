#ifndef OS1_TIME_ABI_H
#define OS1_TIME_ABI_H

#include <stdint.h>

#define OS1_TIME_ABI_VERSION UINT32_C(1)
#define OS1_TIME_MAX_CPUS UINT32_C(64)

#define OS1_TIME_CPU_ONLINE UINT32_C(0x00000001)
#define OS1_TIME_CPU_ARMED UINT32_C(0x00000002)
#define OS1_TIME_CPU_SYNCHRONIZED UINT32_C(0x00000004)

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

_Static_assert(sizeof(struct os1_time_cpu_v1) == 56U,
               "OS1 time CPU ABI size mismatch");
_Static_assert(sizeof(struct os1_time_snapshot_v1) ==
                   48U + (56U * OS1_TIME_MAX_CPUS),
               "OS1 time snapshot ABI size mismatch");

#endif /* OS1_TIME_ABI_H */
