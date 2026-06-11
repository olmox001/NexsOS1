/*
 * kernel/include/kernel/vmm.h
 * Virtual Memory Manager
 *
 * Defines the public API of the VMM and all PTE flag constants for both
 * AArch64 and AMD64.  The constants are mutually exclusive between arches
 * via the ARCH_AARCH64 / ARCH_AMD64 preprocessor guards.
 *
 * Central invariant (IDENTITY MAP):
 *   phys_to_virt() and virt_to_phys() below are identity casts -- they return
 *   their argument unchanged.  This correctly models the current runtime where
 *   kernel VA == PA for the RAM window.
 *   NOTE(MM-VMM-02): any code that calls these and relies on them being no-ops
 *   will break if a higher-half or offset-mapped kernel is ever introduced.
 *
 * Known issues (see docs/review/analysis/01-mm-memory-management.md):
 *   MM-VMM-01 through MM-VMM-07.
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
 * NOTE(MM-VMM-02): This higher-half layout is the INTENDED future design.
 * The kernel currently runs IDENTITY-MAPPED (kernel VA == PA), so the
 * 0xFFFF_... kernel-space range is NOT used today.  phys_to_virt() and
 * virt_to_phys() are identity casts, not the offset translations that a
 * real higher-half split would require.
 */

/* PTE flag constants -- selected by arch at compile time. */
#ifdef ARCH_AARCH64
/* --- AArch64 Page Table Entry (PTE) Flags --- */
/* AArch64 descriptor format and bit details:
 * AArch64 uses descriptor format: bits [1:0] = 0b11 for page, 0b01 for block.
 * Attribute index (bits [4:2]) selects the MAIR_EL1 slot (Normal vs Device).
 * Shareability: bits [9:8]; Inner Shareable = 0b11.
 * Access flag (AF, bit 10): must be set or a permission fault occurs on first access.
 * AP bits [7:6]: EL0 read/write access control.
 * UXN (bit 54): unprivileged execute-never; PXN (bit 53): privileged execute-never. */
#define PTE_VALID (1UL << 0)
#define PTE_PAGE  (1UL << 1)
#define PTE_BLOCK (0UL << 1)

#define PTE_AP_EL1_RW (0UL << 6)
#define PTE_AP_EL1_RO (2UL << 6)
#define PTE_AP_EL0_RW (1UL << 6)
#define PTE_AP_EL0_RO (3UL << 6)

#define PTE_RW       PTE_AP_EL1_RW
#define PTE_USER     PTE_AP_EL0_RW
#define PTE_RO       PTE_AP_EL1_RO

#define PTE_TABLE    PTE_PAGE

/* PTE_IS_TABLE: does this VALID directory entry point to a next-level table?
 * AArch64: bits[1:0] == 0b11 → table; 0b01 → block. */
#define PTE_IS_TABLE(e) (((e) & 0x3UL) == 0x3UL)

#define PTE_INNER_SHARE (3UL << 8)
#define PTE_AF          (1UL << 10)

/* PAGE_KERNEL: kernel RW, not executable (PXN+UXN set).  This is the W^X
 * default for all RAM (MM-VMM-01 resolved); only the kernel text window is
 * mapped PAGE_KERNEL_RX by vmm_map_ram_wx(). */
#define PAGE_KERNEL \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN | PTE_PXN)
/* PAGE_KERNEL_EXEC: kernel RW + executable.  W^X-violating by definition;
 * kept only for exceptional uses — no RAM-wide mapping uses it anymore. */
#define PAGE_KERNEL_EXEC \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN)
/* PAGE_KERNEL_RX: kernel text — read-only at EL1, executable at EL1 only. */
#define PAGE_KERNEL_RX \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RO | PTE_UXN)
/* PAGE_KERNEL_RO: kernel rodata — read-only, never executable. */
#define PAGE_KERNEL_RO \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RO | PTE_UXN | PTE_PXN)
/* PAGE_DEVICE: strongly-ordered Device-nGnRnE memory, not executable. */
#define PAGE_DEVICE \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_DEVICE) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN | PTE_PXN)
/* PAGE_USER: EL0 (user) RW, not executable at EL1.  EL0-executable: use for
 * ELF code segments only. */
#define PAGE_USER \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL0_RW | PTE_PXN)
/* PAGE_USER_DATA: user RW data (stack/heap) — never executable (W^X). */
#define PAGE_USER_DATA (PAGE_USER | PTE_UXN)

#elif defined(ARCH_AMD64)
/* --- AMD64 Page Table Entry (PTE) Flags --- */
/* AMD64 bit layout details:
 * AMD64 uses a simpler flat bit layout; no MAIR equivalent.
 * PTE_NX (bit 63) requires IA32_EFER.NXE=1 (set by arch init).
 * NOTE(MM-VMM-01): PAGE_KERNEL_EXEC == PAGE_KERNEL on AMD64 (no NX bit set),
 * so no W^X enforcement exists on this arch either. */
#define PTE_VALID (1UL << 0) 
#define PTE_RW    (1UL << 1) 
#define PTE_USER  (1UL << 2) 
#define PTE_PWT   (1UL << 3) 
#define PTE_PCD   (1UL << 4) 
#define PTE_AF    (1UL << 5) 
#define PTE_DIRTY (1UL << 6) 
#define PTE_PS    (1UL << 7) 
#define PTE_NX    (1ULL << 63)

#define PTE_PAGE  (0UL)
#define PTE_RO    (0UL)

#define PTE_TABLE (PTE_RW | PTE_USER)

/* PTE_IS_TABLE: does this VALID directory entry point to a next-level table?
 * AMD64: PS (bit 7) clear → table; PS set → 2MB/1GB page.  (PTE_TABLE above
 * is NOT usable for this test — it is an RW|US flag combo for new tables.) */
#define PTE_IS_TABLE(e) (!((e) & PTE_PS))

/* W^X page profiles (MM-VMM-01/AMMU-01 resolved).  PTE_NX requires
 * IA32_EFER.NXE=1, set next to LME in start.S (BSP) and trampoline.S (APs);
 * CR0.WP is set at boot so the kernel honours read-only PTEs. */
#define PAGE_KERNEL      (PTE_VALID | PTE_RW | PTE_NX)
#define PAGE_KERNEL_EXEC (PTE_VALID | PTE_RW) /* W+X: no RAM-wide user left */
#define PAGE_KERNEL_RX   (PTE_VALID)               /* text: RO + executable */
#define PAGE_KERNEL_RO   (PTE_VALID | PTE_NX)      /* rodata: RO, no exec */
#define PAGE_DEVICE      (PTE_VALID | PTE_RW | PTE_PCD | PTE_PWT | PTE_NX)
#define PAGE_USER        (PTE_VALID | PTE_RW | PTE_USER)
/* PAGE_USER_DATA: user RW data (stack/heap) — never executable (W^X). */
#define PAGE_USER_DATA   (PAGE_USER | PTE_NX)

#endif

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

/* Address Translation */
/*
 * Address Translation helpers.
 *
 * NOTE(MM-VMM-02): Both functions are identity casts; they return their
 * argument unchanged.  This is correct only under the identity-map invariant
 * (kernel VA == PA).  They do NOT implement a higher-half offset.  Any future
 * shift to a non-identity map must replace these with real translations.
 */
static inline void *phys_to_virt(uint64_t phys) { return (void *)phys; }
static inline uint64_t virt_to_phys(void *virt) { return (uint64_t)virt; }

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
int vmm_map_page(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags);
/* vmm_unmap_page: unmap one page; local TLB flush only.
 * NOTE(MM-VMM-05): no cross-CPU TLB shootdown. */
void vmm_unmap_page(uint64_t *pgd, uint64_t virt);
uint64_t vmm_get_phys(uint64_t *pgd, uint64_t virt);

struct process;
/* vmm_map_page_locked: vmm_map_page wrapped with proc->mm_lock. */
int vmm_map_page_locked(struct process *proc, uint64_t virt, uint64_t phys, uint64_t flags);
/* vmm_unmap_page_locked: vmm_unmap_page wrapped with proc->mm_lock. */
void vmm_unmap_page_locked(struct process *proc, uint64_t virt);

/* vmm_map: map a contiguous range; 4KB pages only; partial on error (no rollback). */
int vmm_map(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);
/* vmm_map_ram_wx: identity-map a usable RAM range with the W^X section split:
 * kernel text RX, rodata RO+NX, everything else RW+NX (MM-VMM-01/AMMU-01). */
void vmm_map_ram_wx(uint64_t *pgd, uint64_t base, uint64_t size);
/* vmm_check_range: verify all pages in range are mapped with required flags. */
int vmm_check_range(uint64_t *pgd, uint64_t virt, uint64_t size, uint64_t flags_mask);

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
#define vmm_copy_from_user(dest, src, n) arch_copy_from_user(dest, src, n)
#define vmm_copy_to_user(dest, src, n) arch_copy_to_user(dest, src, n)
#define vmm_copy_string_from_user(dest, src, max_len) arch_copy_string_from_user(dest, src, max_len)

#endif /* _KERNEL_VMM_H */
