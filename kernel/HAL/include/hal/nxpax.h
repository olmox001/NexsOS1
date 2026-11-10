#ifndef NXHAL_NXPAX_H
#define NXHAL_NXPAX_H

#include <stdint.h>
#include <hal/limits.h>

/* A common bounded transport, not a second fault-policy implementation. */
#define NXHAL_NXPAX_RING_CAPACITY (2U * NXHAL_CPU_LIMIT)
#define NXHAL_NXPAX_KIND_HARDWARE UINT32_C(1)
#define NXHAL_NXPAX_KIND_USER_TERMINATED UINT32_C(2)
#define NXHAL_NXPAX_KIND_TERMINATION_FAILED UINT32_C(3)
#define NXHAL_NXPAX_KIND_KERNEL_FATAL UINT32_C(4)
#define NXHAL_NXPAX_FLAG_USER UINT32_C(1)
#define NXHAL_NXPAX_FLAG_UACCESS UINT32_C(2)

struct nxhal_nxpax_event {
  uint64_t sequence;
  uint64_t fault_pc;
  uint64_t fault_address;
  uint64_t syndrome;
  int32_t pid;
  uint32_t kind;
  uint32_t flags;
  int32_t cpu_id;
};

/* No locks, allocation, recursion, or unbounded waits in fault context. */
void nxhal_nxpax_note(const struct nxhal_nxpax_event *event);
void nxhal_nxpax_fault(uint64_t pc, uint64_t addr, uint64_t syndrome,
                       int user_mode, int32_t cpu_id);
uint64_t nxhal_nxpax_latest(void);
uint64_t nxhal_nxpax_dropped(void);
/* 1: consistent event; 0: absent/overwritten; -1: invalid/busy. */
int nxhal_nxpax_read(uint64_t sequence, struct nxhal_nxpax_event *event);

#endif
