#ifndef NXHAL_CPU_PROTOCOL_H
#define NXHAL_CPU_PROTOCOL_H

#include <hal/limits.h>
#include <stdint.h>

#define NXHAL_CPU_PROTOCOL_ABI_VERSION UINT32_C(1)
#define NXHAL_CPU_CONTEXT_HAS_USER_ROOT UINT32_C(1)

struct nxhal_cpu_context_switch {
  uint64_t kernel_stack_top;
  uint64_t user_root_phys;
  uint32_t asid;
  uint32_t flags;
};

enum nxhal_cpu_bind_status {
  NXHAL_CPU_BIND_OK = 0,
  NXHAL_CPU_BIND_E_NULL = -1,
  NXHAL_CPU_BIND_E_VERSION = -2,
  NXHAL_CPU_BIND_E_SIZE = -3,
  NXHAL_CPU_BIND_E_RESERVED = -4,
  NXHAL_CPU_BIND_E_MISSING = -5,
  NXHAL_CPU_BIND_E_STATE = -6
};

struct nxhal_cpu_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;

  int (*online)(uint32_t cpu_id);
  int (*is_online)(uint32_t cpu_id);
};

int nxhal_cpu_bind(const struct nxhal_cpu_protocol_v1 *protocol);
int nxhal_cpu_is_bound(void);
int nxhal_cpu_online(uint32_t cpu_id);
int nxhal_cpu_is_online(uint32_t cpu_id);

void hal_cpu_switch_context(const struct nxhal_cpu_context_switch *context);

#endif /* NXHAL_CPU_PROTOCOL_H */
