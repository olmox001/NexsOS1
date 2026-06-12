#ifndef _KERNEL_ARCH_H
#define _KERNEL_ARCH_H

#include <kernel/types.h>

/* Architecture-specific primitives are included here.
 * They must define the arch_impl_* functions or macros. */
#include <arch/arch.h>
#include <kernel/hal_unified.h>


#ifndef __ASSEMBLER__

/* --- CPU and Interrupt HAL --- */
struct process;

/* Basic CPU operations */
void arch_cpu_init(void);
void arch_smp_init(void);
void arch_smp_setup_stacks(uint32_t cpu_count);
int arch_cpu_wake_secondary(uint64_t cpu_id, void (*entry)(void), void *stack);
void arch_cpu_switch_context(struct process *next);

static inline uint32_t arch_get_cpu_id(void) { return arch_impl_get_cpu_id(); }
static inline void arch_nop(void) { arch_impl_nop(); }
static inline void arch_yield(void) { arch_impl_yield(); }
static inline void arch_idle(void) { arch_impl_idle(); }
static inline void arch_cpu_notify(void) { arch_impl_cpu_notify(); }

/* Context Switching & User Mode */
void arch_enter_user_mode(uint64_t entry, uint64_t stack, uint64_t ksp);
void *arch_get_kernel_stack(uint32_t cpu_id);
uint64_t arch_get_boot_info(void);

/* Interrupt Control */
static inline void arch_local_irq_enable(void) { arch_impl_irq_enable(); }
static inline void arch_local_irq_disable(void) { arch_impl_irq_disable(); }
static inline void arch_local_irq_save(uint64_t *flags) { arch_impl_irq_save(flags); }
static inline void arch_local_irq_restore(uint64_t flags) { arch_impl_irq_restore(flags); }

static inline uint64_t arch_local_irq_save_val(void) {
    uint64_t flags;
    arch_local_irq_save(&flags);
    return flags;
}

static inline void arch_local_irq_disable_all(void) { arch_impl_irq_disable_all(); }
static inline void arch_local_irq_save_all(uint64_t *flags) { arch_impl_irq_save_all(flags); }
static inline void arch_local_irq_restore_all(uint64_t flags) { arch_impl_irq_restore_all(flags); }

static inline void arch_cpu_halt(void) __noreturn;
static inline void arch_cpu_halt(void) {
    arch_local_irq_disable_all();
    while (1) arch_idle();
}

/* --- Memory Access HAL --- */
int arch_copy_from_user(void *dest, const void *src, size_t n);
int arch_copy_to_user(void *dest, const void *src, size_t n);
int arch_copy_string_from_user(char *dest, const char *src, size_t max_len);

/* --- Memory Management (VMM/TLB/Cache) --- */
void arch_vmm_init_hw(uint64_t kernel_pgd);
void arch_vmm_map_mmio(uint64_t *pgd);
int arch_vmm_map(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t flags);
int arch_vmm_map_range(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t size, uint64_t flags);
int arch_vmm_unmap(uint64_t pgd, uint64_t va);
/* arch_vmm_protect: rewrite the attributes of existing 4KB mappings in
 * [va, va+size).  'flags' is the arch's PAGE/PTE profile (same vocabulary
 * as arch_vmm_map); the frame address is preserved, every attribute bit is
 * replaced.  Large pages covering the range are split first.  Ends with a
 * cross-CPU TLB shootdown.  Returns 0, or -1 on a hole in the range
 * (already-rewritten pages keep the new attributes). */
int arch_vmm_protect(uint64_t pgd, uint64_t va, uint64_t size, uint64_t flags);
uint64_t arch_vmm_get_physical(uint64_t pgd, uint64_t va);
void arch_vmm_set_secondary_pgd(uint64_t pgd);

static inline void arch_vmm_set_pgd(uint64_t pgd) { arch_impl_set_pgd(pgd); }
static inline uint64_t arch_vmm_get_pgd(void) { return arch_impl_get_pgd(); }

/* Kernel address-space root: TTBR1 on aarch64 (separate from the per-
 * process TTBR0), CR3 alias on amd64 (kernel half shared via PML4 high
 * entries).  Used by vmm_init/vmm_dynamic_remap when installing the
 * kernel PGD. */
static inline void arch_vmm_set_kernel_pgd(uint64_t pgd) { arch_impl_set_kernel_pgd(pgd); }
static inline uint64_t arch_vmm_get_kernel_pgd(void) { return arch_impl_get_kernel_pgd(); }

/* TLB and Cache Control */
static inline void arch_tlb_flush_local(void) { arch_impl_tlb_flush_local(); }
static inline void arch_tlb_flush_all(void) { arch_impl_tlb_flush_all(); }
static inline void arch_tlb_flush_va(uintptr_t va) { arch_impl_tlb_flush_va(va); }

/* SMP TLB shootdown (MM-VMM-05/AMMU-08 resolved).  Contract: when these
 * return, NO online CPU still holds a stale translation for the target
 * (single VA, or the whole address space for _all).  AArch64 satisfies it in
 * hardware (inner-shareable broadcast TLBI + DSB ISH); amd64 sends a
 * fixed-vector LAPIC IPI and waits (bounded) for peer acknowledgements —
 * a peer with IRQs masked flushes as soon as it unmasks (the IPI stays
 * pending in its LAPIC). */
static inline void arch_tlb_shootdown_va(uintptr_t va) { arch_impl_tlb_shootdown_va(va); }
static inline void arch_tlb_shootdown_all(void) { arch_impl_tlb_shootdown_all(); }

static inline void arch_cache_clean_range(void *va, size_t size) { arch_impl_cache_clean_range(va, size); }
static inline void arch_cache_sync_icache(void *va, size_t size) { arch_impl_cache_sync_icache(va, size); }

/* --- Memory & Execution Barriers --- */
static inline void arch_isb(void) { arch_impl_isb(); }
static inline void arch_mb(void) { arch_impl_mb(); }
static inline void arch_rmb(void) { arch_impl_rmb(); }
static inline void arch_wmb(void) { arch_impl_wmb(); }

/* --- System State & Debug --- */
static inline uint64_t arch_get_fault_address(void) { return arch_impl_get_fault_address(); }
static inline uint64_t arch_get_fault_status(void) { return arch_impl_get_fault_status(); }
void arch_set_vector_table(uintptr_t vbar);

/* --- Timer HAL --- */
static inline uint64_t arch_timer_get_freq(void) { return arch_impl_timer_get_freq(); }
static inline uint64_t arch_timer_get_count(void) { return arch_impl_timer_get_count(); }
static inline void arch_timer_set_compare(uint64_t val) { arch_impl_timer_set_compare(val); }
static inline void arch_timer_control(uint32_t val) { arch_impl_timer_control(val); }

/* --- Spinlocks --- */
static inline void arch_spin_lock(volatile uint32_t *lock) { arch_impl_spin_lock(lock); }
static inline void arch_spin_unlock(volatile uint32_t *lock) { arch_impl_spin_unlock(lock); }
static inline int arch_spin_trylock(volatile uint32_t *lock) { return arch_impl_spin_trylock(lock); }

/* --- VirtIO Bus HAL --- */
uint32_t arch_virtio_read32(uintptr_t base, uint32_t offset);
void arch_virtio_write32(uintptr_t base, uint32_t offset, uint32_t val);
int arch_virtio_probe(uint32_t device_id, uintptr_t *out_base, uint32_t *out_irq);

#endif /* __ASSEMBLER__ */

/* Platform constants */
#define ARCH_RAM_START      HAL_RAM_START
#define ARCH_RAM_SIZE       HAL_RAM_SIZE
#define ARCH_ALIAS_OFFSET   HAL_ALIAS_OFFSET

#endif /* _KERNEL_ARCH_H */
