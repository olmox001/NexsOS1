#ifndef NXHAL_TIME_H
#define NXHAL_TIME_H

#include <stdint.h>

#define NXHAL_TIME_ABI_VERSION UINT32_C(1)

#define NXHAL_CLOCKSOURCE_CONTINUOUS UINT32_C(0x00000001)
#define NXHAL_CLOCKSOURCE_CALIBRATED UINT32_C(0x00000002)
#define NXHAL_CLOCKSOURCE_INVARIANT UINT32_C(0x00000004)
#define NXHAL_CLOCKSOURCE_MUST_VERIFY UINT32_C(0x00000008)
#define NXHAL_CLOCKSOURCE_GLOBAL UINT32_C(0x00000010)

#define NXHAL_CLOCKEVENT_PER_CPU UINT32_C(0x00000001)
#define NXHAL_CLOCKEVENT_PERIODIC UINT32_C(0x00000002)
#define NXHAL_CLOCKEVENT_ONESHOT UINT32_C(0x00000004)
#define NXHAL_CLOCKEVENT_ABSOLUTE UINT32_C(0x00000008)

enum nxhal_time_status {
  NXHAL_TIME_OK = 0,
  NXHAL_TIME_E_NULL = -1,
  NXHAL_TIME_E_NOT_READY = -2,
  NXHAL_TIME_E_UNSUPPORTED = -3,
  NXHAL_TIME_E_INVALID = -4,
  NXHAL_TIME_E_UNSTABLE = -5,
  NXHAL_TIME_E_OVERFLOW = -6
};

enum nxhal_clocksource_id {
  NXHAL_CLOCKSOURCE_ID_AMD64_TSC = 1,
  NXHAL_CLOCKSOURCE_ID_ARM64_VIRTUAL = 2
};

enum nxhal_clockevent_id {
  NXHAL_CLOCKEVENT_ID_AMD64_LAPIC = 1,
  NXHAL_CLOCKEVENT_ID_ARM64_VIRTUAL = 2
};

struct nxhal_clocksource_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t source_id;
  uint32_t flags;
  uint32_t counter_bits;
  uint32_t reserved0;
  uint64_t frequency_hz;
  uint64_t mask;
  uint64_t max_delta_counts;
};

struct nxhal_clockevent_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t event_id;
  uint32_t flags;
  uint32_t irq;
  uint32_t reserved0;
  uint64_t frequency_hz;
  uint64_t min_delta_counts;
  uint64_t max_delta_counts;
};

int nxhal_time_clocksource_info(struct nxhal_clocksource_v1 *out);
int nxhal_time_clockevent_info(struct nxhal_clockevent_v1 *out);
int nxhal_time_counter_read(uint64_t *out_count);
int nxhal_time_event_percpu_init(uint64_t frequency_hz);
int nxhal_time_event_program_absolute(uint64_t count);
int nxhal_time_event_program_periodic(uint64_t frequency_hz);
int nxhal_time_event_stop(void);

_Static_assert(sizeof(struct nxhal_clocksource_v1) == 48U,
               "NX-HAL clocksource ABI size");
_Static_assert(sizeof(struct nxhal_clockevent_v1) == 48U,
               "NX-HAL clockevent ABI size");

#endif /* NXHAL_TIME_H */
