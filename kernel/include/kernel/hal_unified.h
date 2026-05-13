#ifndef _KERNEL_HAL_UNIFIED_H
#define _KERNEL_HAL_UNIFIED_H

#include <kernel/types.h>
#include <arch/arch.h>

#ifndef __ASSEMBLER__

/* 
 * HAL Unified Interface
 * 
 * This header defines the standard hardware abstraction layer for the NEXS kernel.
 * All core kernel code should use these hal_* primitives instead of arch-specific ones.
 */

/* --- CPU Control --- */

/** Halt the current CPU permanently (masked IRQs) */
static inline void hal_cpu_halt(void) {
    arch_impl_irq_disable_all();
    while (1) arch_impl_idle();
}

/** Enter low-power idle state until next interrupt */
static inline void hal_cpu_idle(void) {
    arch_impl_idle();
}

/** Hint to the CPU that we are in a busy-wait loop */
static inline void hal_cpu_yield(void) {
    arch_impl_yield();
}

/** NOP instruction */
static inline void hal_cpu_nop(void) {
    arch_impl_nop();
}

/** Notify other CPUs (SEV on ARM, NOP on x86) */
static inline void hal_cpu_notify(void) {
    arch_impl_cpu_notify();
}

/** Get the unique physical ID of the current CPU */
static inline uint32_t hal_cpu_id(void) {
    return arch_impl_get_cpu_id();
}

/* --- Local Interrupt Control --- */

/** Enable interrupts on the local CPU */
static inline void hal_irq_enable(void) {
    arch_impl_irq_enable();
}

/** Disable interrupts on the local CPU */
static inline void hal_irq_disable(void) {
    arch_impl_irq_disable();
}

/** Save current interrupt state and disable them (returns flags) */
static inline uint64_t hal_irq_save_val(void) {
    uint64_t flags;
    arch_impl_irq_save(&flags);
    return flags;
}

/** Save current interrupt state and disable them */
static inline void hal_irq_save(uint64_t *flags) {
    arch_impl_irq_save(flags);
}

/** Restore interrupt state from previously saved flags */
static inline void hal_irq_restore(uint64_t flags) {
    arch_impl_irq_restore(flags);
}

/** Save ALL interrupt state (including FIQ on ARM) and disable them */
static inline void hal_irq_save_all(uint64_t *flags) {
    arch_impl_irq_save_all(flags);
}

/** Restore ALL interrupt state */
static inline void hal_irq_restore_all(uint64_t flags) {
    arch_impl_irq_restore_all(flags);
}

/** Disable ALL interrupts */
static inline void hal_irq_disable_all(void) {
    arch_impl_irq_disable_all();
}

/* --- Memory Barriers --- */

/** Instruction Synchronization Barrier */
static inline void hal_isb(void) {
    arch_impl_isb();
}

/** Full Data Memory Barrier */
static inline void hal_mb(void) {
    arch_impl_mb();
}

/** Read Memory Barrier */
static inline void hal_rmb(void) {
    arch_impl_rmb();
}

/** Write Memory Barrier */
static inline void hal_wmb(void) {
    arch_impl_wmb();
}

/* --- Memory Management (VMM/TLB) --- */

/** Set the current Page Directory / Translation Table Base */
static inline void hal_vmm_set_pgd(uint64_t pgd) {
    arch_impl_set_pgd(pgd);
}

/** Get the current Page Directory / Translation Table Base */
static inline uint64_t hal_vmm_get_pgd(void) {
    return arch_impl_get_pgd();
}

/** Flush local TLB (all entries) */
static inline void hal_tlb_flush_local(void) {
    arch_impl_tlb_flush_local();
}

/** Flush TLB across all CPUs */
static inline void hal_tlb_flush_all(void) {
    arch_impl_tlb_flush_all();
}

/** Flush TLB for a specific virtual address */
static inline void hal_tlb_flush_va(uintptr_t va) {
    arch_impl_tlb_flush_va(va);
}

/* --- Cache Control --- */

/** Clean D-cache for a specific address range */
static inline void hal_cache_clean(void *va, size_t size) {
    arch_impl_cache_clean_range(va, size);
}

/** Sync I-cache for a specific address range */
static inline void hal_cache_sync_icache(void *va, size_t size) {
    arch_impl_cache_sync_icache(va, size);
}

/* --- Spinlocks --- */

static inline void hal_spin_lock(volatile uint32_t *lock) {
    arch_impl_spin_lock(lock);
}

static inline void hal_spin_unlock(volatile uint32_t *lock) {
    arch_impl_spin_unlock(lock);
}

static inline int hal_spin_trylock(volatile uint32_t *lock) {
    return arch_impl_spin_trylock(lock);
}

/* --- Port I/O (Architecture Specific or NOP) --- */

#ifdef ARCH_AMD64
static inline uint8_t hal_inb(uint16_t port) { return inb(port); }
static inline void hal_outb(uint16_t port, uint8_t val) { outb(port, val); }
static inline uint16_t hal_inw(uint16_t port) { return inw(port); }
static inline void hal_outw(uint16_t port, uint16_t val) { outw(port, val); }
static inline uint32_t hal_inl(uint16_t port) { return inl(port); }
static inline void hal_outl(uint16_t port, uint32_t val) { outl(port, val); }
#else
/* Port I/O is NOP on non-x86 architectures */
static inline uint8_t hal_inb(uint16_t port) { (void)port; return 0; }
static inline void hal_outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint16_t hal_inw(uint16_t port) { (void)port; return 0; }
static inline void hal_outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline uint32_t hal_inl(uint16_t port) { (void)port; return 0; }
static inline void hal_outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
#endif

#endif /* __ASSEMBLER__ */

#endif /* _KERNEL_HAL_UNIFIED_H */
