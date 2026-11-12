/*
 * kernel/include/kernel/memlayout.h
 * Kernel physical/virtual address translation (MM-VMM-02 / MM-PMM-07).
 *
 * KERNEL_VIRT_BASE is the offset of the kernel's linear ("direct") map:
 * every byte of physical address space the kernel touches (RAM, MMIO,
 * page-table pages, DMA buffers) is reachable at
 *
 *     VA = PA + KERNEL_VIRT_BASE
 *
 * and the kernel image itself is linked inside that window, so the same
 * single offset converts in both directions for ANY kernel virtual
 * address (image symbols, PMM allocations, MMIO pointers alike).
 *
 * The higher half is LIVE on both arches (aarch64: 0xFFFF000000000000 via
 * TTBR1; amd64: 0xFFFF800000000000, PML4 slot 256+).  The boot assembly
 * enables the MMU with boot tables before any C runs, so kernel code only
 * ever executes at its link address; user space owns the low half
 * exclusively.
 *
 * Contract for the rest of the tree (enforced by the Phase-B2 sweep):
 *   - uint64_t values named pa/phys/pgd_addr are PHYSICAL addresses;
 *     pointers are kernel VIRTUAL addresses.
 *   - pmm_alloc_*() return kernel virtual pointers; what hardware needs
 *     (PTE output addresses, virtqueue descriptors, DMA registers, CR3/
 *     TTBR values) must go through virt_to_phys().
 *   - MMIO register accesses translate their physical base through
 *     phys_to_virt() (see hal_device.h and the uart/gic/apic accessors).
 */
#ifndef _KERNEL_MEMLAYOUT_H
#define _KERNEL_MEMLAYOUT_H

#include <kernel/types.h>

#include <hal/config.h>

/* The selected HAL provider supplies the direct-map base.  Generic memory
 * code knows the mapping contract, never the architecture that implements it. */
#define KERNEL_VIRT_BASE HAL_DIRECT_MAP_BASE

/* phys_to_virt - kernel virtual pointer for a physical address. */
static inline void *phys_to_virt(uint64_t phys) {
  return (void *)(uintptr_t)(phys + KERNEL_VIRT_BASE);
}

/* virt_to_phys - physical address behind a kernel virtual pointer.
 * Valid for every kernel VA (image, PMM pages, MMIO) because the image
 * is linked inside the direct-map window. */
static inline uint64_t virt_to_phys(void *virt) {
  return (uint64_t)(uintptr_t)virt - KERNEL_VIRT_BASE;
}

#endif /* _KERNEL_MEMLAYOUT_H */
