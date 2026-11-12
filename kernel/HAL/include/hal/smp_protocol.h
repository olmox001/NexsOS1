#ifndef NXHAL_SMP_PROTOCOL_H
#define NXHAL_SMP_PROTOCOL_H

#include <stdint.h>

#define NXHAL_SMP_PROTOCOL_ABI_VERSION UINT32_C(1)

enum nxhal_smp_bind_status {
  NXHAL_SMP_BIND_OK = 0,
  NXHAL_SMP_BIND_E_NULL = -1,
  NXHAL_SMP_BIND_E_VERSION = -2,
  NXHAL_SMP_BIND_E_SIZE = -3,
  NXHAL_SMP_BIND_E_RESERVED = -4,
  NXHAL_SMP_BIND_E_MISSING = -5,
  NXHAL_SMP_BIND_E_STATE = -6
};

struct nxhal_smp_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;
  int (*bringup_secondary)(uint32_t cpu_id, void *stack_top);
};

int nxhal_smp_bind(const struct nxhal_smp_protocol_v1 *protocol);
int nxhal_smp_is_bound(void);
int nxhal_smp_bringup_secondary(uint32_t cpu_id, void *stack_top);

#endif /* NXHAL_SMP_PROTOCOL_H */
