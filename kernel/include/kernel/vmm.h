/*
 * kernel/include/kernel/vmm.h
 * Virtual Memory Manager
 */
#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

#include <kernel/pmm.h>
#include <kernel/types.h>

/* Virtual Memory Map
 *
 * 0x0000_0000_0000_0000 - 0x0000_FFFF_FFFF_FFFF : User Space (256 TB)
 * 0xFFFF_0000_0000_0000 - 0xFFFF_FFFF_FFFF_FFFF : Kernel Space (256 TB)
 */

#ifdef ARCH_AARCH64
/* --- AArch64 Page Table Entry (PTE) Flags --- */
#define PTE_VALID (1UL << 0)
#define PTE_TABLE (1UL << 1) 
#define PTE_PAGE  (1UL << 1)
#define PTE_BLOCK (0UL << 1)

#define PTE_AP_EL1_RW (0UL << 6)
#define PTE_AP_EL1_RO (2UL << 6)
#define PTE_AP_EL0_RW (1UL << 6)
#define PTE_AP_EL0_RO (3UL << 6)

#define PTE_RW       PTE_AP_EL1_RW
#define PTE_USER     PTE_AP_EL0_RW
#define PTE_RO       PTE_AP_EL1_RO

#define PTE_INNER_SHARE (3UL << 8)
#define PTE_AF          (1UL << 10)

#define PAGE_KERNEL \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN | PTE_PXN)
#define PAGE_KERNEL_EXEC \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN)
#define PAGE_DEVICE \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_DEVICE) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN | PTE_PXN)
#define PAGE_USER \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL0_RW | PTE_PXN)

#elif defined(ARCH_AMD64)
/* --- AMD64 Page Table Entry (PTE) Flags --- */
#define PTE_VALID (1UL << 0) 
#define PTE_RW    (1UL << 1) 
#define PTE_USER  (1UL << 2) 
#define PTE_PWT   (1UL << 3) 
#define PTE_PCD   (1UL << 4) 
#define PTE_AF    (1UL << 5) 
#define PTE_DIRTY (1UL << 6) 
#define PTE_PS    (1UL << 7) 
#define PTE_NX    (1ULL << 63)

#define PTE_TABLE (PTE_RW | PTE_USER)
#define PTE_PAGE  (0UL) 
#define PTE_RO    (0UL) 

#define PAGE_KERNEL      (PTE_VALID | PTE_RW)
#define PAGE_KERNEL_EXEC (PTE_VALID | PTE_RW)
#define PAGE_DEVICE      (PTE_VALID | PTE_RW | PTE_PCD | PTE_PWT)
#define PAGE_USER        (PTE_VALID | PTE_RW | PTE_USER)

#endif

/* --- Architecture-Neutral Attribute Definitions --- */
#define PTE_ATTR_NORMAL 0UL
#define PTE_ATTR_DEVICE 1UL
#define PTE_ATTR_INDX(x) ((uint64_t)(x) << 2)

/* Execute-Never Bits (Mapped by Arch MMU) */
#define PTE_UXN (1UL << 54)
#define PTE_PXN (1UL << 53)

/* VMM Types */
typedef uint64_t gva_t;
typedef uint64_t gpa_t;
typedef uint64_t pte_t;

/* Address Translation */
static inline void *phys_to_virt(uint64_t phys) { return (void *)phys; }
static inline uint64_t virt_to_phys(void *virt) { return (uint64_t)virt; }

uint64_t *vmm_create_pgd(void);
uint64_t arch_vmm_create_process_pgd(void);
void vmm_destroy_pgd(uint64_t *pgd);

void vmm_init(void);
int vmm_map_page(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t *pgd, uint64_t virt);

struct process;
int vmm_map_page_locked(struct process *proc, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page_locked(struct process *proc, uint64_t virt);

int vmm_map(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);
int vmm_check_range(uint64_t *pgd, uint64_t virt, uint64_t size, uint64_t flags_mask);

static inline bool vmm_is_user_addr(uint64_t addr) {
  return (addr >= 0x1000) && ((addr & 0xFFFF000000000000ULL) == 0);
}

#include <kernel/arch.h>
#define vmm_copy_from_user(dest, src, n) arch_copy_from_user(dest, src, n)
#define vmm_copy_to_user(dest, src, n) arch_copy_to_user(dest, src, n)
#define vmm_copy_string_from_user(dest, src, max_len) arch_copy_string_from_user(dest, src, max_len)

#endif /* _KERNEL_VMM_H */
