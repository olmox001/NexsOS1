/*
 * kernel/include/kernel/vmm.h
 * Virtual Memory Manager
 *
 * Defines the public API of the VMM and all PTE flag constants for both
 * AArch64 and AMD64.  The constants are mutually exclusive between arches
 * via the provider-selected <hal/vmm_format.h> contract.
 *
 * Central invariant (DIRECT MAP, see kernel/memlayout.h):
 *   phys_to_virt()/virt_to_phys() are a single-offset translation
 *   (KERNEL_VIRT_BASE; 0 == identity while the higher-half flip is staged).
 *
 *   MM-VMM-02 + MM-PMM-07 (contract sweep, Phase B2): every PA-to-pointer
 *   crossing in the tree goes through that pair — page-table walkers,
 *   PMM returns/frees, process PGD loads (TTBR0/CR3), user-frame maps,
 *   virtio DMA addresses, MMIO accessors.  Flipping KERNEL_VIRT_BASE plus
 *   the boot/linker changes is all that remains for the higher half.
 *
 * Known issues (see docs/review/analysis/01-mm-memory-management.md):
 *   MM-VMM-01 through MM-VMM-07.
 */
#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

#include <kernel/nx_contract.h>
#include <kernel/memlayout.h>
#include <kernel/pmm.h>
#include <kernel/types.h>

/* Virtual Memory Map
 *
 * 0x0000_0000_0000_0000 - 0x0000_FFFF_FFFF_FFFF : User Space (256 TB)
 * 0xFFFF_0000_0000_0000 - 0xFFFF_FFFF_FFFF_FFFF : Kernel Space (256 TB)
 *
 * MM-VMM-02 RESOLVED: this layout is now LIVE on both arches.  The kernel
 * image and the direct map of all RAM/MMIO live at KERNEL_VIRT_BASE + PA
 * (memlayout.h); user space owns the low half exclusively (aarch64: TTBR0
 * per process; amd64: PML4 entries 0..255 private per process).
 */

/* PTE flag constants -- selected by arch at compile time. */
#include <hal/vmm_format.h>

/* --- Architecture-Neutral Attribute Definitions --- */
/* PTE_ATTR_INDX selects MAIR_EL1 slot on AArch64; unused on AMD64. */
#define PTE_ATTR_NORMAL 0UL
#define PTE_ATTR_DEVICE 1UL
#define PTE_ATTR_INDX(x) ((uint64_t)(x) << 2)

/* Execute-Never Bits (Mapped by Arch MMU) */
/* AArch64 only; encoded in bits 54/53 of PTE.
 * On AMD64, PTE_NX plays this role instead. */
#define PTE_UXN (1UL << 54)
#define PTE_PXN (1UL << 53)

/* VMM Types */
typedef uint64_t gva_t;
typedef uint64_t gpa_t;
typedef uint64_t pte_t;
/* typedef aliases for documentation clarity:
 * gva_t = guest/kernel virtual address
 * gpa_t = guest/kernel physical address
 * pte_t = raw page table entry value */

/* Address Translation: phys_to_virt()/virt_to_phys() live in
 * kernel/memlayout.h (single KERNEL_VIRT_BASE offset, identity while the
 * offset is 0).  Included above so all existing users keep compiling. */

/* vmm_create_pgd: allocate a new process PGD with kernel half pre-filled. */
uint64_t *vmm_create_pgd(void);
uint64_t arch_vmm_create_process_pgd(void);
/* vmm_destroy_pgd: free the process-private page-table pages AND the user
 * RAM frames (PTEs carrying PTE_USER) of 'pgd' (MM-VMM-04 resolved).
 * Entries shared with kernel_pgd (compared by value) are never touched. */
void vmm_destroy_pgd(uint64_t *pgd);

/* vmm_init: phase-1 MMU bring-up; maps 128 MB bootstrap window; enables MMU. */
void vmm_init(void);
/* vmm_dynamic_remap: phase-2; maps all detected RAM; switches to new PGD.
 * NOTE(MM-VMM-03): leaks bootstrap PGD; no IPI to secondary CPUs.
 * NOTE(MM-VMM-01): maps all RAM PAGE_KERNEL_EXEC; no W^X. */
void vmm_dynamic_remap(void);
/* vmm_map_page: map one 4KB page; delegates to arch_vmm_map(). */
int vmm_map_page(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags) NX_MUST_USE;
/* vmm_unmap_page: unmap one page with cross-CPU TLB shootdown (MM-VMM-05
 * resolved): on return no online CPU translates 'virt' via the old entry. */
void vmm_unmap_page(uint64_t *pgd, uint64_t virt);
/* vmm_protect: rewrite the attributes of existing mappings in [virt,
 * virt+size) to the given PAGE/PTE profile (AMMU-02 resolved).  Frame
 * addresses are preserved; large pages are split for 4KB precision; ends
 * with a cross-CPU TLB shootdown.  Returns 0, or -1 on a hole in range. */
int vmm_protect(uint64_t *pgd, uint64_t virt, uint64_t size, uint64_t flags) NX_MUST_USE;
uint64_t vmm_get_phys(uint64_t *pgd, uint64_t virt);

struct process;
/* vmm_map_page_locked: vmm_map_page wrapped with proc->mm_lock. */
int vmm_map_page_locked(struct process *proc, uint64_t virt, uint64_t phys, uint64_t flags) NX_MUST_USE;
/* vmm_unmap_page_locked: vmm_unmap_page wrapped with proc->mm_lock. */
void vmm_unmap_page_locked(struct process *proc, uint64_t virt);

/* vmm_map: map a contiguous range; 4KB pages only; partial on error (no rollback). */
int vmm_map(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) NX_MUST_USE;
/* vmm_map_ram_wx: map a usable RAM range at its direct-map VA with the W^X
 * split: kernel text RX, rodata RO+NX, everything else RW+NX
 * (MM-VMM-01/AMMU-01). */
void vmm_map_ram_wx(uint64_t *pgd, uint64_t base, uint64_t size);
/* vmm_check_range: verify all pages in range are mapped with required flags. */
int vmm_check_range(uint64_t *pgd, uint64_t virt, uint64_t size, uint64_t flags_mask) NX_MUST_USE;

/*
 * vmm_is_user_addr - return true if 'addr' falls in the user virtual range.
 *
 * Checks that addr >= 0x1000 (excludes NULL page) and that the top 16 bits
 * are zero (canonical user-space addresses in a 48-bit VA space).
 * Used by syscall handlers to validate user pointers before access.
 */
static inline bool vmm_is_user_addr(uint64_t addr) {
  return (addr >= 0x1000) && ((addr & 0xFFFF000000000000ULL) == 0);
}

#include <kernel/arch.h>
#include <kernel/hal_uaccess.h>
#define vmm_copy_from_user(dest, src, n) hal_copy_from_user(dest, src, n)
#define vmm_copy_to_user(dest, src, n) hal_copy_to_user(dest, src, n)
/* TOLERANT copy: truncation is accepted.  Where a shortened string would be a
 * DIFFERENT string (paths, registry keys/values, environment), use
 * hal_copy_string_from_user_strict() instead — see kernel/hal_uaccess.h. */
#define vmm_copy_string_from_user(dest, src, max_len) hal_copy_string_from_user(dest, src, max_len)
#define vmm_copy_string_from_user_strict(dest, src, max_len) hal_copy_string_from_user_strict(dest, src, max_len)

#endif /* _KERNEL_VMM_H */
