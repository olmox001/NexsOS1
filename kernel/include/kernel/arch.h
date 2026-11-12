#ifndef _KERNEL_ARCH_H
#define _KERNEL_ARCH_H

#include <hal/config.h>
#include <hal/core.h>
#include <kernel/nx_contract.h>
#include <kernel/types.h>

#ifndef __ASSEMBLER__

/*
 * Transitional provider operations not yet moved to the final HAL protocol.
 * Pure ISA primitives no longer live here: they are exported by libnxhal.a
 * through <hal/core.h>.  Every declaration below is tracked for conversion to
 * an outbound HAL operation or an inbound HAL->kernel protocol.
 */
struct process;

void arch_smp_init(void);
void arch_smp_setup_stacks(uint32_t cpu_count);
void arch_cpu_yield(void);

void arch_enter_user_mode(uint64_t entry, uint64_t stack, uint64_t ksp);
void *arch_get_kernel_stack(uint32_t cpu_id);
uint64_t arch_get_boot_info(void);

void arch_vmm_init_hw(uint64_t kernel_pgd);
void arch_vmm_map_mmio(uint64_t *pgd);
int arch_vmm_map(uint64_t pgd, uint64_t va, uint64_t pa,
                 uint64_t flags) NX_MUST_USE;
int arch_vmm_map_range(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t size,
                       uint64_t flags) NX_MUST_USE;
int arch_vmm_unmap(uint64_t pgd, uint64_t va) NX_MUST_USE;
int arch_vmm_protect(uint64_t pgd, uint64_t va, uint64_t size,
                     uint64_t flags) NX_MUST_USE;
uint64_t arch_vmm_get_physical(uint64_t pgd, uint64_t va);
void arch_vmm_set_secondary_pgd(uint64_t pgd);

void arch_set_vector_table(uintptr_t vbar);

uint32_t arch_virtio_read32(uintptr_t base, uint32_t offset);
void arch_virtio_write32(uintptr_t base, uint32_t offset, uint32_t val);
int arch_virtio_probe(uint32_t device_id, uintptr_t *out_base,
                      uint32_t *out_irq) NX_MUST_USE;

#endif /* __ASSEMBLER__ */

#endif /* _KERNEL_ARCH_H */
