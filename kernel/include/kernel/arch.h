#ifndef _KERNEL_ARCH_H
#define _KERNEL_ARCH_H

#include <arch/arch.h>

#ifndef __ASSEMBLER__
/* Generic interface for architecture-specific operations */
#include <kernel/types.h>

/* --- CPU and Interrupt HAL --- */
#define arch_get_cpu_id() __arch_get_cpu_id()
struct process;
void arch_cpu_init(void);
int arch_cpu_wake_secondary(uint64_t cpu_id, void (*entry)(void), void *stack);
void arch_enter_user_mode(uint64_t entry, uint64_t stack, uint64_t ksp);
uint64_t arch_get_boot_info(void);
void *arch_get_kernel_stack(uint32_t cpu_id);
void arch_cpu_switch_context(struct process *next);
void arch_vmm_set_secondary_pgd(uint64_t pgd);
#endif

#define arch_local_irq_enable() __arch_local_irq_enable()
#define arch_local_irq_disable() __arch_local_irq_disable()
#define arch_local_irq_save(flags) __arch_local_irq_save(flags)
#define arch_local_irq_restore(flags) __arch_local_irq_restore(flags)

#ifndef __ASSEMBLER__
static inline uint64_t __arch_local_irq_save_val(void) {
    uint64_t flags;
    __arch_local_irq_save(&flags);
    return flags;
}

/* --- Memory Access HAL --- */
int arch_copy_from_user(void *dest, const void *src, size_t n);
int arch_copy_to_user(void *dest, const void *src, size_t n);
int arch_copy_string_from_user(char *dest, const char *src, size_t max_len);
#endif

/* Disable ALL exceptions/interrupts (daifset 0xf on ARM) */
#define arch_local_irq_disable_all() __arch_local_irq_disable_all()
#define arch_local_irq_save_all(flags) __arch_local_irq_save_all(flags)
#define arch_local_irq_restore_all(flags) __arch_local_irq_restore_all(flags)

/* --- CPU Control --- */
#define arch_nop() __arch_nop()
#define arch_idle() __arch_wfi() /* Wait for Interrupt / HLT */
#define arch_yield() __arch_yield()
#define arch_cpu_halt() __arch_cpu_halt()

/* --- Memory & Execution Barriers --- */
#define arch_isb() __arch_isb()
#define arch_mb()  __arch_mb()
#define arch_rmb() __arch_rmb()
#define arch_wmb() __arch_wmb()
#define arch_instr_barrier() arch_isb()
#define arch_data_barrier()  arch_mb()

#ifndef __ASSEMBLER__
/* --- Memory Management (VMM/TLB/Cache) --- */
void arch_vmm_init_hw(uint64_t kernel_pgd);
void arch_vmm_map_mmio(uint64_t *pgd);
int arch_vmm_map(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t flags);
int arch_vmm_map_range(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t size, uint64_t flags);
int arch_vmm_unmap(uint64_t pgd, uint64_t va);
#endif

#define ARCH_RAM_START __ARCH_RAM_START
#define ARCH_RAM_SIZE  __ARCH_RAM_SIZE
#define ARCH_ALIAS_OFFSET __ARCH_ALIAS_OFFSET
/* Set Page Directory Base (CR3 on x86, TTBR0/1 on ARM) */
#define arch_vmm_set_pgd(v) __arch_set_ttbr0(v)
#define arch_vmm_get_pgd() __arch_get_ttbr0()

#define arch_tlb_flush_local() __arch_tlb_flush_local()
#define arch_tlb_flush_all() __arch_tlb_flush_all()
#define arch_tlb_flush_va(va) __arch_tlb_flush_va(va)

#define arch_cache_clean_va(va) __arch_clean_cache_va(va)
#define arch_cache_clean_pou(va) __arch_clean_cache_va_pou(va)
#define arch_cache_clean_range(va, s) __arch_clean_cache_range_va(va, s)

/* --- Exception/Syscall Handling --- */
#define arch_set_vector_table(v) __arch_set_vbar(v)
#define arch_get_vector_table() __arch_get_vbar()

/* --- Hardware Abstraction --- */
/* Generic Timer Access */
#define arch_timer_get_freq() __arch_cntfrq_el0_read()
#define arch_timer_get_count() __arch_cntvct_el0_read()
#define arch_timer_set_compare(v) __arch_cntv_cval_el0_write(v)
#define arch_timer_control(v) __arch_cntv_ctl_el0_write(v)

/* --- Spinlocks (Arch-specific optimizations) --- */
#define arch_spin_lock(lock) __arch_spin_lock(lock)
#define arch_spin_unlock(lock) __arch_spin_unlock(lock)
#define arch_spin_trylock(lock) __arch_spin_trylock(lock)

/* --- System Control Registers (Arch-Specific but exposed via HAL) --- */
#define arch_get_esr() __arch_get_esr()
#define arch_get_far() __arch_get_far()
#define arch_get_cpacr() __arch_get_cpacr()
#define arch_set_cpacr(v) __arch_set_cpacr(v)
#define arch_set_mair(v) __arch_set_mair(v)
#define arch_set_tcr(v) __arch_set_tcr(v)
#define arch_get_sctlr() __arch_get_sctlr()
#define arch_set_sctlr(v) __arch_set_sctlr(v)

#ifndef __ASSEMBLER__
/* --- VirtIO HAL --- */
uint32_t arch_virtio_read32(uintptr_t base, uint32_t offset);
void arch_virtio_write32(uintptr_t base, uint32_t offset, uint32_t val);
int arch_virtio_probe(uint32_t device_id, uintptr_t *out_base, uint32_t *out_irq);
#endif

/* --- Compatibility Aliases --- */
#define arch_wfi() arch_idle()
#define arch_sev() __arch_sev()
#define arch_wfe() __arch_wfe()

#endif /* _KERNEL_ARCH_H */
