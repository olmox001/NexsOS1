#ifndef NXHAL_PROVIDER_H
#define NXHAL_PROVIDER_H

#include <stdint.h>
#include <hal/cpu_protocol.h>
#include <hal/uaccess.h>

/*
 * NX-HAL provider-private lifecycle surface.
 *
 * These symbols connect HAL/core/core.c to the selected provider. NX-CORE must
 * never include this header or call them directly.
 */
void nxhal_provider_cpu_local_init(void);
void nxhal_provider_bus_scan(void);
void nxhal_provider_irq_init(void);
void nxhal_provider_timer_global_init(void);
void nxhal_provider_timer_percpu_init(void);
int nxhal_provider_vmm_map_device(uint64_t base, uint64_t size);
int32_t nxhal_provider_cpu_id_fault_safe(void);
void nxhal_provider_cpu_switch_context(
    const struct nxhal_cpu_context_switch *context);
void nxhal_provider_uaccess_enter(uint64_t user_root_phys,
                                  struct nxhal_uaccess_hw_state *state);
void nxhal_provider_uaccess_exit(
    const struct nxhal_uaccess_hw_state *state);
void nxhal_provider_uaccess_fault_abort(void);

#endif /* NXHAL_PROVIDER_H */
