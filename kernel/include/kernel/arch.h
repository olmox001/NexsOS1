#ifndef _KERNEL_ARCH_H
#define _KERNEL_ARCH_H

#include <arch/arch.h>

/* Generic interface for architecture-specific operations */

/* Interrupt control */
#define arch_local_irq_enable() __arch_local_irq_enable()
#define arch_local_irq_disable() __arch_local_irq_disable()
#define arch_local_irq_save(flags) __arch_local_irq_save(flags)
#define arch_local_irq_restore(flags) __arch_local_irq_restore(flags)
#define arch_local_irq_save_all(flags) __arch_local_irq_save_all(flags)
#define arch_local_irq_restore_all(flags) __arch_local_irq_restore_all(flags)

/* System operations */
#define arch_nop() __arch_nop()
#define arch_wfi() __arch_wfi()
#define arch_wfe() __arch_wfe()
#define arch_sev() __arch_sev()
#define arch_isb() __arch_isb()
#define arch_dsb() __arch_dsb()
#define arch_dmb() __arch_dmb()

/* Register access */
#define arch_get_cpu_id() __arch_get_cpu_id()
#define arch_get_esr() __arch_get_esr()
#define arch_get_far() __arch_get_far()
#define arch_get_vbar() __arch_get_vbar()
#define arch_set_vbar(v) __arch_set_vbar(v)
#define arch_get_cpacr() __arch_get_cpacr()
#define arch_set_cpacr(v) __arch_set_cpacr(v)

/* MMU / TLB */
#define arch_set_ttbr0(v) __arch_set_ttbr0(v)
#define arch_tlb_flush_local() __arch_tlb_flush_local()
#define arch_tlb_flush_all() __arch_tlb_flush_all()
#define arch_tlb_flush_va(va) __arch_tlb_flush_va(va)

/* Cache */
#define arch_clean_cache_va(va) __arch_clean_cache_va(va)
#define arch_clean_cache_va_pou(va) __arch_clean_cache_va_pou(va)
#define arch_clean_cache_range_va(va, s) __arch_clean_cache_range_va(va, s)

/* More Registers */
#define arch_get_ttbr0() __arch_get_ttbr0()
#define arch_set_mair(v) __arch_set_mair(v)
#define arch_set_tcr(v) __arch_set_tcr(v)
#define arch_get_sctlr() __arch_get_sctlr()
#define arch_set_sctlr(v) __arch_set_sctlr(v)

/* Spinlocks */
#define arch_spin_lock(lock) __arch_spin_lock(lock)
#define arch_spin_unlock(lock) __arch_spin_unlock(lock)
#define arch_spin_trylock(lock) __arch_spin_trylock(lock)

/* Timer */
#define arch_cntfrq_el0_read() __arch_cntfrq_el0_read()
#define arch_cntvct_el0_read() __arch_cntvct_el0_read()
#define arch_cntv_cval_el0_write(v) __arch_cntv_cval_el0_write(v)
#define arch_cntv_ctl_el0_write(v) __arch_cntv_ctl_el0_write(v)

#endif /* _KERNEL_ARCH_H */
