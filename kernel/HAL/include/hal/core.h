#ifndef NXHAL_CORE_H
#define NXHAL_CORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * NX-HAL architecture-neutral outbound primitive contract.
 *
 * This header is consumable by NX-CORE without including any provider/ISA
 * header. Implementations are real symbols in libnxhal.a; architecture
 * selection happens only while building the HAL provider.
 */

/* CPU lifecycle/control. */
uint32_t hal_cpu_id(void);
void hal_cpu_nop(void);
void hal_cpu_idle(void);
void hal_cpu_yield(void);
void hal_cpu_notify(void);
void hal_cpu_halt(void) __attribute__((noreturn));
void hal_reboot(void) __attribute__((noreturn));

/* Local interrupt state. */
void hal_irq_enable(void);
void hal_irq_disable(void);
uint64_t hal_irq_save_val(void);
void hal_irq_save(uint64_t *flags);
void hal_irq_restore(uint64_t flags);
void hal_irq_save_all(uint64_t *flags);
void hal_irq_restore_all(uint64_t flags);
void hal_irq_disable_all(void);

/* Ordering. */
void hal_isb(void);
void hal_mb(void);
void hal_rmb(void);
void hal_wmb(void);

/* Provider physical direct-map contract. end is exclusive. */
int hal_phys_range_valid(uint64_t base, uint64_t end);

/* Address-space roots and translation maintenance. */
void hal_vmm_set_pgd(uint64_t pgd);
uint64_t hal_vmm_get_pgd(void);
void hal_vmm_set_kernel_pgd(uint64_t pgd);
uint64_t hal_vmm_get_kernel_pgd(void);
void hal_tlb_flush_local(void);
void hal_tlb_flush_all(void);
void hal_tlb_flush_va(uintptr_t va);
void hal_tlb_shootdown_va(uintptr_t va);
void hal_tlb_shootdown_all(void);

/* Coherency. */
void hal_cache_clean(void *va, size_t size);
void hal_cache_invalidate(void *va, size_t size);
void hal_cache_sync_icache(void *va, size_t size);

/* Fault metadata and entropy. */
uint64_t hal_fault_address(void);
uint64_t hal_fault_status(void);
int hal_hw_random(uint64_t *out);

/* Monotonic hardware timer primitive. */
uint64_t hal_timer_get_freq(void);
uint64_t hal_timer_get_count(void);
void hal_timer_set_compare(uint64_t value);
void hal_timer_control(uint32_t value);

/* Lock primitive used by the generic spinlock policy. */
void hal_spin_lock(volatile uint32_t *lock);
void hal_spin_unlock(volatile uint32_t *lock);
int hal_spin_trylock(volatile uint32_t *lock);

/*
 * Legacy port-I/O transport.  Capability discovery is explicit so generic
 * drivers never have to infer an ISA from preprocessor symbols.
 */
int hal_ioport_available(void);
uint8_t hal_inb(uint16_t port);
uint16_t hal_inw(uint16_t port);
uint32_t hal_inl(uint16_t port);
void hal_outb(uint16_t port, uint8_t value);
void hal_outw(uint16_t port, uint16_t value);
void hal_outl(uint16_t port, uint32_t value);

#endif /* NXHAL_CORE_H */
