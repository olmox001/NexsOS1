#ifndef NXHAL_CORE_PROTOCOL_H
#define NXHAL_CORE_PROTOCOL_H

#include <hal/frame.h>
#include <stdint.h>

#define NXHAL_CORE_PROTOCOL_ABI_VERSION UINT32_C(1)

enum nxhal_core_bind_status {
  NXHAL_CORE_BIND_OK = 0,
  NXHAL_CORE_BIND_E_NULL = -1,
  NXHAL_CORE_BIND_E_VERSION = -2,
  NXHAL_CORE_BIND_E_SIZE = -3,
  NXHAL_CORE_BIND_E_RESERVED = -4,
  NXHAL_CORE_BIND_E_MISSING = -5,
  NXHAL_CORE_BIND_E_STATE = -6
};

/*
 * HAL -> Core event protocol.
 *
 * Ownership:
 * - table storage belongs to the Core;
 * - NX-HAL validates then copies the complete v1 table before publishing it;
 * - callbacks are immutable after a successful bind;
 * - frames remain HAL-owned execution records. Core may mutate the supplied
 *   frame only through the public hal_frame_* contract and may return another
 *   frame selected for restore.
 *
 * No member exposes a kernel type or architecture type.
 */
struct nxhal_core_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;

  hal_frame_t *(*schedule)(hal_frame_t *frame);
  hal_frame_t *(*syscall)(hal_frame_t *frame);
  hal_frame_t *(*irq_entry)(hal_frame_t *frame);
  hal_frame_t *(*irq_dispatch)(uint32_t irq, hal_frame_t *frame);
  hal_frame_t *(*timer_tick)(hal_frame_t *frame);
  void (*secondary_entry)(void);
};

int nxhal_core_bind(const struct nxhal_core_protocol_v1 *protocol);
int nxhal_core_is_bound(void);

hal_frame_t *nxhal_core_schedule(hal_frame_t *frame);
hal_frame_t *nxhal_core_syscall(hal_frame_t *frame);
hal_frame_t *nxhal_core_irq_entry(hal_frame_t *frame);
hal_frame_t *nxhal_core_irq_dispatch(uint32_t irq, hal_frame_t *frame);
hal_frame_t *nxhal_core_timer_tick(hal_frame_t *frame);
void nxhal_core_secondary_entry(void) __attribute__((noreturn));

/*
 * Cold boot entry is the sole pre-bind Core symbol in the HAL ABI.  Boot
 * assembly calls it with the two provider handoff words.  Its implementation
 * belongs to NX-CORE and must bind this protocol before enabling event paths.
 */
void nxcore_hal_boot_entry(uint64_t boot_arg0, uint64_t boot_arg1)
    __attribute__((noreturn));

#endif /* NXHAL_CORE_PROTOCOL_H */
