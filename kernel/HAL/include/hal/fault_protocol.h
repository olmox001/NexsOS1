#ifndef NXHAL_FAULT_PROTOCOL_H
#define NXHAL_FAULT_PROTOCOL_H

#include <hal/frame.h>
#include <stdint.h>

#define NXHAL_FAULT_PROTOCOL_ABI_VERSION UINT32_C(1)

enum nxhal_fault_bind_status {
  NXHAL_FAULT_BIND_OK = 0,
  NXHAL_FAULT_BIND_E_NULL = -1,
  NXHAL_FAULT_BIND_E_VERSION = -2,
  NXHAL_FAULT_BIND_E_SIZE = -3,
  NXHAL_FAULT_BIND_E_RESERVED = -4,
  NXHAL_FAULT_BIND_E_MISSING = -5,
  NXHAL_FAULT_BIND_E_STATE = -6
};

/*
 * HAL -> Core fault-decision protocol.
 *
 * Architecture providers retain decoding of native syndrome/register state.
 * The Core owns policy: recursion accounting, process-vs-kernel disposition,
 * symbolized backtrace and recovery scheduling.  This keeps architecture
 * decoding out of NX-CORE without giving NX-HAL authority over process policy.
 */
struct nxhal_fault_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;

  unsigned (*enter)(void);
  void (*exit)(void);
  hal_frame_t *(*handle)(hal_frame_t *frame, int user_mode,
                         uint64_t fault_addr, uint64_t fault_pc,
                         const char *description, uint64_t syndrome);
  void (*backtrace_regs)(uint64_t pc, uint64_t fp);
  void (*backtrace_scan)(uint64_t sp);
};

int nxhal_fault_bind(const struct nxhal_fault_protocol_v1 *protocol);
int nxhal_fault_is_bound(void);

unsigned nxhal_fault_enter(void);
void nxhal_fault_exit(void);
hal_frame_t *nxhal_fault_handle(hal_frame_t *frame, int user_mode,
                                uint64_t fault_addr, uint64_t fault_pc,
                                const char *description, uint64_t syndrome);
void nxhal_fault_backtrace_regs(uint64_t pc, uint64_t fp);
void nxhal_fault_backtrace_scan(uint64_t sp);

#endif /* NXHAL_FAULT_PROTOCOL_H */
