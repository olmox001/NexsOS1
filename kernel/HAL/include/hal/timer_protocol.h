#ifndef NXHAL_TIMER_PROTOCOL_H
#define NXHAL_TIMER_PROTOCOL_H

#include <stdint.h>

#define NXHAL_TIMER_PROTOCOL_ABI_VERSION UINT32_C(1)

enum nxhal_timer_bind_status {
  NXHAL_TIMER_BIND_OK = 0,
  NXHAL_TIMER_BIND_E_NULL = -1,
  NXHAL_TIMER_BIND_E_VERSION = -2,
  NXHAL_TIMER_BIND_E_SIZE = -3,
  NXHAL_TIMER_BIND_E_RESERVED = -4,
  NXHAL_TIMER_BIND_E_MISSING = -5,
  NXHAL_TIMER_BIND_E_STATE = -6
};

struct nxhal_timer_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;
  uint64_t (*arm_current)(void);
  uint64_t (*tick_current)(void);
};

int nxhal_timer_bind(const struct nxhal_timer_protocol_v1 *protocol);
int nxhal_timer_is_bound(void);
uint64_t nxhal_timer_arm_current(void);
uint64_t nxhal_timer_tick_current(void);

#endif /* NXHAL_TIMER_PROTOCOL_H */
