/*
 * kernel/include/kernel/vmm.h
 * Virtual Memory Manager
 *
 * AArch64 4-level page table management
 */
#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

#include <kernel/pmm.h>
#include <kernel/types.h>

/* Virtual Memory Map
 *
 * 0x0000_0000_0000_0000 - 0x0000_FFFF_FFFF_FFFF : User Space (256 TB)
 * 0xFFFF_0000_0000_0000 - 0xFFFF_FFFF_FFFF_FFFF : Kernel Space (256 TB)
 *
 * Kernel map:
 * 0xFFFF_0000_0000_0000 : Physical Memory Identity Map
 * 0xFFFF_8000_0000_0000 : Kernel Image & Data
 * 0xFFFF_FFFF_FE00_0000 : MMIO / Peripherals
 */

/* Page Table Entry (PTE) Flags */
#define PTE_VALID (1UL << 0)
#define PTE_TABLE (1UL << 1) /* For L0-L2 tables */
#define PTE_PAGE                                                               \
  (1UL << 1) /* For L3 pages (bit 1) - actually same bit as TABLE but context  \
                changes */
#define PTE_BLOCK (0UL << 1) /* For L1-L2 blocks (bit 1 = 0) */

/* Attribute Index (MAIR_EL1) */
#define PTE_ATTR_NORMAL 0UL /* Index 0 in MAIR */
#define PTE_ATTR_DEVICE 1UL /* Index 1 in MAIR */

#define PTE_ATTR_INDX(x) ((x) << 2)

/* Access Permission (AP) */
#define PTE_RW (0UL << 6)   /* Read-Write */
#define PTE_RO (2UL << 6)   /* Read-Only */
#define PTE_USER (1UL << 6) /* EL0 Access allowed (AP[1]=1) */
/* AP[2:1] = 00: EL1 RW, EL0 None
 * AP[2:1] = 01: EL1 RW, EL0 RW
 * AP[2:1] = 10: EL1 RO, EL0 None
 * AP[2:1] = 11: EL1 RO, EL0 RO
 */
#define PTE_AP_EL1_RW (0UL << 6)
#define PTE_AP_EL1_RO (2UL << 6)
#define PTE_AP_EL0_RW (1UL << 6)
#define PTE_AP_EL0_RO (3UL << 6)

/* Shareability */
#define PTE_NON_SHARE (0UL << 8)
#define PTE_OUTER_SHARE (2UL << 8)
#define PTE_INNER_SHARE (3UL << 8)

/* Access Flag */
#define PTE_AF (1UL << 10) /* Access Flag (must be 1 for validity) */

/* Execute Never (XN) */
#define PTE_UXN (1UL << 54) /* User Execute Never */
#define PTE_PXN (1UL << 53) /* Privileged Execute Never */

/* Standard Page Flags */
#define PAGE_KERNEL                                                            \
  (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE |   \
   PTE_AF | PTE_AP_EL1_RW | PTE_UXN | PTE_PXN)
#define PAGE_KERNEL_RO                                                         \
  (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE |   \
   PTE_AF | PTE_AP_EL1_RO | PTE_UXN | PTE_PXN)
#define PAGE_KERNEL_EXEC                                                       \
  (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE |   \
   PTE_AF | PTE_AP_EL1_RW | PTE_UXN) /* UXN set, but NOT PXN */
#define PAGE_DEVICE                                                            \
  (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_DEVICE) | PTE_INNER_SHARE |   \
   PTE_AF | PTE_AP_EL1_RW | PTE_UXN | PTE_PXN)
#define PAGE_USER                                                              \
  (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE |   \
   PTE_AF | PTE_AP_EL0_RW | PTE_PXN)

/* VMM Types */
typedef uint64_t gva_t; /* Guest Virtual Address */
typedef uint64_t gpa_t; /* Guest Physical Address */
typedef uint64_t pte_t;

/* Convert physical to virtual (identity map) */
static inline void *phys_to_virt(uint64_t phys) { return (void *)phys; }

static inline uint64_t virt_to_phys(void *virt) { return (uint64_t)virt; }

uint64_t *vmm_create_pgd(void);
void vmm_destroy_pgd(uint64_t *pgd);

void vmm_init(void);
int vmm_map_page(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t *pgd, uint64_t virt);

/* Thread-safe (locked) helpers */
struct process;
int vmm_map_page_locked(struct process *proc, uint64_t virt, uint64_t phys,
                        uint64_t flags);
void vmm_unmap_page_locked(struct process *proc, uint64_t virt);

/* Range mapping helper */
int vmm_map(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t size,
            uint64_t flags);
int vmm_check_range(uint64_t *pgd, uint64_t virt, uint64_t size,
                    uint64_t flags_mask);

/* Security: Check if address is in user range */
static inline bool vmm_is_user_addr(uint64_t addr) {
  /* User space: 0x0000_0000_0000_1000 to 0x0000_FFFF_FFFF_FFFF */
  /* We exclude NULL page and Kernel Space (top half) */
  /* Top half starts at 0xFFFF_0000_0000_0000 */
  /* In 48-bit VA, bit 47 must be 0 for user, 1 for kernel. */
  /* Check if bit 63 is 0 (User/Low) and not NULL page */
  return (addr >= 0x1000) && ((addr & 0xFFFF000000000000ULL) == 0);
}

/* User space access helpers */
int copy_from_user(void *dest, const void *src, size_t n);
int copy_to_user(void *dest, const void *src, size_t n);
int copy_string_from_user(char *dest, const char *src, size_t max_len);

#endif /* _KERNEL_VMM_H */
