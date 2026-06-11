/*
 * kernel/mm/vmm.c
 * Virtual Memory Manager
 *
 * AArch64 4-level page table management
 *
 * NOTE(MM-VMM-07): The file header says "AArch64" but this file is compiled
 * for both AArch64 and AMD64.  The page-table index arithmetic (PGD/PUD/PMD/PT
 * at 39/30/21/12 bit) matches 4-level tables on both architectures; the heavy
 * lifting is delegated to arch_vmm_* hooks which differ per arch.
 *
 * Role:
 *   This file provides two distinct services:
 *   1. Page-table construction helpers (vmm_map_page, vmm_map, vmm_unmap_page,
 *      vmm_check_range) that call arch_vmm_map/unmap under per-process mm_lock.
 *   2. MMU lifecycle (vmm_init -> vmm_dynamic_remap) and per-process PGD
 *      management (vmm_create_pgd / vmm_destroy_pgd).
 *
 * Central invariant (IDENTITY MAP):
 *   The kernel runs identity-mapped (VA == PA for the RAM window).
 *   phys_to_virt() and virt_to_phys() in vmm.h are identity casts.
 *   get_next_table() casts a PTE physical address directly to a pointer;
 *   vmm_map/vmm_init use pmm_alloc_page() results directly as pointers.
 *   This model works under QEMU -kernel but contradicts the higher-half map
 *   described in the vmm.h comment block.  See MM-VMM-02 and S.4 of the
 *   subsystem analysis.
 *
 * Known issues:
 *   MM-VMM-01  (W3 SECURITY)     All RAM mapped PAGE_KERNEL_EXEC; no W^X.
 *   MM-VMM-02  (W3 WRONG-DESIGN) PTE physical addresses cast to pointers;
 *                                 only valid under identity map.
 *   MM-VMM-03  (W2 REFINE/TODO)  vmm_dynamic_remap leaks the old PGD.
 *   MM-VMM-04  (W3 BUG)          vmm_destroy_pgd leaks user RAM frames and
 *                                 contains a dead empty loop.
 *   MM-VMM-05  (W3 BUG/SECURITY) No cross-CPU TLB shootdown on unmap/remap.
 *   MM-VMM-06  (W2 REFINE)       Generic map path is 4KB-only; 2MB blocks
 *                                 only via arch_vmm_map_range.
 *   MM-VMM-07  (W1 DOC)          File header mentions only AArch64.
 */
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <kernel/platform.h>
#include <stdint.h>

/* Page Table Levels */
/* 4-level walk (48-bit VA, 4KB granule on both arches). */
#define PGD_SHIFT 39
#define PUD_SHIFT 30
#define PMD_SHIFT 21
#define PT_SHIFT 12

#define PGD_INDEX(x) (((x) >> PGD_SHIFT) & 0x1FF)
#define PUD_INDEX(x) (((x) >> PUD_SHIFT) & 0x1FF)
#define PMD_INDEX(x) (((x) >> PMD_SHIFT) & 0x1FF)
#define PT_INDEX(x) (((x) >> PT_SHIFT) & 0x1FF)

/* Global Kernel PGD */
/* kernel_pgd - physical address of the current kernel page-global directory.
 * Under the identity-map invariant this value is also a valid C pointer.
 * Updated atomically (pointer assignment) in vmm_dynamic_remap() after the
 * hardware TTBR/CR3 has been switched; secondary CPUs read it after arch_mb(). */
uint64_t *kernel_pgd;

#define PTE_ADDR_MASK 0x0000FFFFFFFFF000UL

/*
 * Get or create next level table
 *
 * get_next_table - walk one level of the page table, optionally allocating.
 *
 * Parameters:
 *   table - pointer to the current-level page-table page (512 uint64_t entries).
 *   index - 9-bit index into 'table' for the next level.
 *   alloc - if non-zero and the entry is not present, allocate a new table page.
 *
 * Returns: pointer to the next-level table, or NULL if not present and
 *          alloc==0, or NULL if pmm_alloc_page() fails.
 *
 * NOTE(MM-VMM-02): When an existing entry is present, the physical address is
 * extracted from bits [47:12] of the PTE and cast DIRECTLY to a uint64_t *
 * pointer.  This is only correct under the identity-map invariant (VA==PA).
 * On a higher-half or KASLR kernel this would dereference a wrong address.
 *
 * NOTE(MM-VMM-02): When allocating a new table, pmm_alloc_page() returns a
 * value that is simultaneously the physical address stored in the PTE AND the
 * C pointer used to clear the page.  The in-code comments at lines 54-63
 * of the original source acknowledge this confusion explicitly.
 *
 * Locking: caller must hold the relevant PGD/process mm_lock; this helper
 * does not take any lock itself.
 */
static uint64_t *get_next_table(uint64_t *table, uint64_t index, int alloc) {
  if (table[index] & PTE_VALID) {
    /* Extract physical address mask (48 bits supported) */
    uint64_t phys = table[index] & 0x0000FFFFFFFFF000UL;
    /* In full OS, this should be mapped. For early boot, assuming
     * identity/linear map */
    return (uint64_t *)phys;
  }

  if (!alloc)
    return NULL;

  // Allocate new page for table
  void *page = pmm_alloc_page();
  if (!page)
    return NULL;

  // Actually pmm_alloc_page returns physical address + offset if using early
  // mapping? Wait, pmm returns direct mapped address usually... Let's assume
  // PMM returns physical address for now as per previous impl? Checking pmm
  // implementation... PMM returns (MEMORY_BASE + pfn_to_phys) which is physical
  // address in QEMU space (0x4000...).

  // In our simplified model:
  // Physical address IS the pointer returned by pmm (since we are 1:1 mapped
  // currently) But table entries store PHYSICAL addresses.



  /* Zero and Flush the new table page */
  memset(page, 0, 4096);
  arch_cache_clean_range(page, 4096);
  arch_mb();

  /* Table entry flags:
   * AArch64: Valid (bit 0), Table (bit 1), AF (bit 10), Inner Share (bits 8-9),
   *          AP EL0 RW (bit 6-7, usually ignored for tables but safe).
   * AMD64:   Present (bit 0), RW (bit 1), User (bit 2).
   */
  table[index] = (uint64_t)page | PTE_TABLE | PTE_VALID;

  /* Flush the directory entry itself */
  arch_cache_clean_range(&table[index], 8);
  arch_mb();

  return page;
}

/*
 * Map a page
 *
 * vmm_map_page - map a single 4KB page in the given page table.
 *
 * Validates page-size alignment of both 'virt' and 'phys', then delegates to
 * arch_vmm_map() which performs the actual PTE write.
 *
 * NOTE(MM-VMM-06): This function always maps 4KB pages.  2MB huge-page
 * mappings are only used through arch_vmm_map_range() called from
 * vmm_dynamic_remap(); the generic path is inconsistent and slower for
 * large contiguous ranges.
 *
 * Parameters:
 *   pgd   - physical address of the PGD (also a valid pointer under identity map).
 *   virt  - virtual address to map; must be 4KB-aligned.
 *   phys  - physical address to map to; must be 4KB-aligned.
 *   flags - PTE attribute flags (PAGE_KERNEL_EXEC, PAGE_USER, etc.).
 *
 * Returns: 0 on success, -1 on alignment error.
 * Locking: caller must hold any required mm_lock.
 */
int vmm_map_page(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags) {
  if ((virt & 0xFFF) || (phys & 0xFFF)) {
    pr_err("VMM: Invalid alignment virt=%lx phys=%lx\n", virt, phys);
    return -1;
  }

  /* Extract existing mapping for warning if necessary */
  /* (Optional: we can keep the warning logic here if arch_vmm_map doesn't warn) */

  return arch_vmm_map((uint64_t)pgd, virt, phys, flags);
}

/* Internal helper with locking */
/*
 * vmm_map_page_locked - vmm_map_page() wrapped with proc->mm_lock.
 *
 * Acquires proc->mm_lock with IRQ save before calling vmm_map_page(), then
 * releases it.  Use this variant when the caller does not already hold mm_lock.
 *
 * NOTE(MM-VMM-05): Even with mm_lock held, there is no TLB shootdown IPI sent
 * to other CPUs after mapping.  On SMP, other CPUs may continue to use a
 * stale TLB entry for this virtual address until their next context switch.
 */
int vmm_map_page_locked(struct process *proc, uint64_t virt, uint64_t phys,
                        uint64_t flags) {
  uint64_t lock_flags;
  spin_lock_irqsave(&proc->mm_lock, &lock_flags);
  int ret = vmm_map_page(proc->page_table, virt, phys, flags);
  spin_unlock_irqrestore(&proc->mm_lock, lock_flags);
  return ret;
}

/*
 * Check if a range is fully mapped and has required flags
 * Returns 0 if OK, -1 if any page is missing
 *
 * vmm_check_range - verify every page in [virt, virt+size) is mapped with
 *                   all bits of 'flags_mask' set.
 *
 * Walks the four-level page table using get_next_table() with alloc=0.
 * Returns 0 if every PTE is present and satisfies the flags_mask, or -1 as
 * soon as any level is missing or a PTE fails the mask check.
 *
 * NOTE(MM-VMM-02): Internally calls get_next_table(), which casts PTE
 * physical addresses to pointers under the identity-map assumption.
 *
 * Parameters:
 *   pgd        - PGD to walk (physical address / identity-mapped pointer).
 *   virt       - start virtual address (rounded down to 4KB boundary).
 *   size       - length in bytes (rounded up to 4KB boundary).
 *   flags_mask - set of PTE bits that must all be present; 0 skips flag check.
 *
 * Returns: 0 if fully mapped, -1 on first missing or non-conforming page.
 * Locking: caller must ensure the page table is not concurrently modified.
 */
int vmm_check_range(uint64_t *pgd, uint64_t virt, uint64_t size,
                    uint64_t flags_mask) {
  uint64_t v = virt & ~0xFFFUL;
  uint64_t e = (virt + size + 4095) & ~0xFFFUL;

  while (v < e) {
    uint64_t *pud, *pmd, *pt;

    pud = get_next_table(pgd, PGD_INDEX(v), 0);
    if (!pud)
      return -1;

    pmd = get_next_table(pud, PUD_INDEX(v), 0);
    if (!pmd)
      return -1;

    pt = get_next_table(pmd, PMD_INDEX(v), 0);
    if (!pt)
      return -1;

    uint64_t entry = pt[PT_INDEX(v)];
    if (!(entry & PTE_VALID))
      return -1;

    if (flags_mask && (entry & flags_mask) != flags_mask)
      return -1;

    v += 4096;
  }
  return 0;
}

/*
 * vmm_get_phys - translate a virtual address to its mapped physical address.
 *
 * Delegates entirely to arch_vmm_get_physical(); refer to the arch
 * implementation for details on the walk and the identity-map assumptions.
 *
 * Returns the physical address, or 0 / arch-defined sentinel on unmapped VA.
 */
uint64_t vmm_get_phys(uint64_t *pgd, uint64_t virt) {
  return arch_vmm_get_physical((uint64_t)pgd, virt);
}

/*
 * Unmap a page
 *
 * vmm_unmap_page - remove a single page mapping from the given PGD.
 *
 * Delegates to arch_vmm_unmap(), which clears the PTE and performs a local
 * TLB invalidation.
 *
 * NOTE(MM-VMM-05): Only the calling CPU's TLB is flushed.  No IPI is sent to
 * other CPUs to invalidate their TLB entries for this VA.  On SMP, other cores
 * may continue to use stale translations, creating a correctness and security
 * hazard (stale translation could point to a page now assigned to another
 * process).
 */
void vmm_unmap_page(uint64_t *pgd, uint64_t virt) {
  arch_vmm_unmap((uint64_t)pgd, virt);
}

/* Internal helper with locking */
/*
 * vmm_unmap_page_locked - vmm_unmap_page() wrapped with proc->mm_lock.
 *
 * Acquires proc->mm_lock with IRQ save/restore around the unmap call.
 * NOTE(MM-VMM-05): TLB shootdown is still absent even with the lock held.
 */
void vmm_unmap_page_locked(struct process *proc, uint64_t virt) {
  uint64_t lock_flags;
  spin_lock_irqsave(&proc->mm_lock, &lock_flags);
  vmm_unmap_page(proc->page_table, virt);
  spin_unlock_irqrestore(&proc->mm_lock, lock_flags);
}

/*
 * Map a range of memory
 *
 * vmm_map - map a contiguous virtual range [virt, virt+size) to [phys, phys+size).
 *
 * Both 'virt' and 'phys' are rounded down to 4KB; 'size' is rounded up.
 * Calls vmm_map_page() once per 4KB page, stopping and returning -1 on the
 * first failure.  Partial mappings are not cleaned up on error.
 *
 * NOTE(MM-VMM-06): Always maps 4KB pages regardless of alignment or size.
 *
 * Returns: 0 on success, -1 if any page mapping fails.
 * Locking: caller must hold any required mm_lock.
 */
int vmm_map(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t size,
            uint64_t flags) {
  uint64_t v = virt & ~0xFFFUL;
  uint64_t p = phys & ~0xFFFUL;
  uint64_t e = (virt + size + 4095) & ~0xFFFUL;

  while (v < e) {
    if (vmm_map_page(pgd, v, p, flags) != 0) {
      return -1;
    }
    v += 4096;
    p += 4096;
  }
  return 0;
}

/*
 * Initialize VMM and Enable MMU
 *
 * vmm_init - phase-1 MMU bring-up: map a 128 MB bootstrap window and enable
 *            the MMU.
 *
 * Steps:
 *   1. Allocate and zero the kernel PGD from the PMM (sets global kernel_pgd).
 *   2. Map [ARCH_RAM_START, ARCH_RAM_START+128MB) with PAGE_KERNEL_EXEC using
 *      vmm_map() (4KB pages).
 *   3. Map MMIO regions via arch_vmm_map_mmio().
 *   4. Enable the MMU by passing virt_to_phys(kernel_pgd) to arch_vmm_init_hw().
 *      Under identity map, virt_to_phys is a no-op cast.
 *
 * After vmm_init() the kernel executes with the MMU active.  Only the first
 * 128 MB of RAM are accessible; vmm_dynamic_remap() extends coverage.
 *
 * NOTE(MM-VMM-01): The bootstrap window is mapped PAGE_KERNEL_EXEC, making
 * the kernel heap, stack, and data pages executable.  There is no W^X split.
 *
 * Locking: must be called single-threaded before SMP bring-up.
 */
void vmm_init(void) {
  pr_info("%s", "VMM: Initializing MMU (Phase 1: Bootstrap)...\n");

  /* Allocate Kernel PGD */
  kernel_pgd = pmm_alloc_page();
  if (!kernel_pgd) {
    panic("VMM: Failed to allocate kernel PGD\n");
  }
  memset(kernel_pgd, 0, 4096);
  arch_cache_clean_range(kernel_pgd, 4096);

  /* 1. Map RAM with correct permissions (Bootstrap 128MB) */
  uint64_t ram_start = ARCH_RAM_START;
  uint64_t ram_size = 128UL * 1024 * 1024; /* Enough to boot */
  
  vmm_map(kernel_pgd, ram_start, ram_start, ram_size, PAGE_KERNEL_EXEC);

  /* 2. Map MMIO */
  arch_vmm_map_mmio(kernel_pgd);

  /* 3. Enable MMU */
  arch_vmm_init_hw(virt_to_phys(kernel_pgd));

  pr_info("%s", "VMM: Bootstrap complete.\n");
}

/*
 * Dynamic remapping of all discovered RAM
 *
 * vmm_dynamic_remap - phase-2 mapping: remap all discovered RAM into a new PGD.
 *
 * Builds a new PGD from scratch, using arch_vmm_map_range() (which uses 2MB
 * block entries where possible) to map every MEM_REGION_USABLE region returned
 * by arch_platform_get_mem_regions().  Re-maps MMIO.  Then atomically switches
 * the hardware to the new PGD with arch_vmm_set_pgd() and updates kernel_pgd.
 *
 * NOTE(MM-VMM-01): All usable regions are mapped PAGE_KERNEL_EXEC.  The entire
 * kernel heap, stacks, and PMM metadata are therefore executable.
 *
 * NOTE(MM-VMM-03): The old PGD (bootstrap) is stored in 'old_pgd' and then
 * abandoned -- it is never freed.  A correct implementation would broadcast an
 * IPI to all secondary CPUs, wait for them to switch, and only then return the
 * old PGD pages to the PMM.  The (void)old_pgd suppresses the unused-variable
 * warning.
 *
 * NOTE(MM-VMM-05): arch_vmm_set_pgd() flushes the local TLB; no IPI is sent
 * to secondary CPUs.  If any secondary is running at this point it may access
 * the old (now leaked) PGD's tables.
 *
 * Locking: must be called before SMP bring-up completes on secondary CPUs.
 */
void vmm_dynamic_remap(void) {
  pr_info("%s", "VMM: Performing RAM-aware dynamic remapping...\n");

  /* Allocate a temporary PGD to build the new map */
  uint64_t *new_pgd = pmm_alloc_page();
  if (!new_pgd) panic("VMM: Failed to allocate new dynamic PGD");
  memset(new_pgd, 0, 4096);

  /* 1. Map all discovered memory regions */
  size_t count = 0;
  struct mem_region *regions = arch_platform_get_mem_regions(&count);
  for (size_t i = 0; i < count; i++) {
    if (regions[i].type == MEM_REGION_USABLE) {
      pr_info("VMM: Mapping region 0x%lx - 0x%lx\n", 
              regions[i].base, regions[i].base + regions[i].size);
      
      /* Use the optimized arch_vmm_map_range which supports 2MB pages (Zero-Copy Table Linking)
       * Now safe as get_next_table handles block-splitting. */
      arch_vmm_map_range((uint64_t)new_pgd, regions[i].base, regions[i].base, 
                         regions[i].size, PAGE_KERNEL_EXEC);
    }
  }

  /* 2. Re-map MMIO */
  arch_vmm_map_mmio(new_pgd);

  /* 3. Atomically switch to the new PGD */
  uint64_t new_phys = virt_to_phys(new_pgd);
  
  /* Swap the global kernel_pgd */
  uint64_t *old_pgd = kernel_pgd;
  kernel_pgd = new_pgd;
  
  arch_vmm_set_pgd(new_phys);
  arch_mb();
  arch_isb();

  /* 4. Cleanup old table - currently deferred until all CPUs have switched to new PGD */
  /* TODO: Implement proper synchronization (IPI broadcast) to safely free old_pgd
   *       after all secondary CPUs have transitioned to the new kernel_pgd.
   *       This requires arch_send_ipi and a TLB flush callback on all CPUs. */
  (void)old_pgd;

  pr_info("%s", "VMM: Dynamic remapping successful. All discovered RAM is now accessible.\n");
}

/*
 * Create a new PGD
 *
 * vmm_create_pgd - allocate a new process PGD with the kernel half pre-filled.
 *
 * Delegates to arch_vmm_create_process_pgd() which allocates a page, copies
 * the upper-half (kernel) PGD entries from kernel_pgd by reference (not
 * deep-copied), and leaves the lower-half zero for user-space mappings.
 *
 * Returns: physical address of the new PGD (also usable as a pointer).
 */
uint64_t *vmm_create_pgd(void) {
  return (uint64_t *)arch_vmm_create_process_pgd();
}

/*
 * Destroy a PGD: free private page-table pages AND user RAM frames
 *
 * vmm_destroy_pgd - tear down the process-private half of a PGD
 * (MM-VMM-04 + AMMU-03 resolved).
 *
 * Ownership rules (what may be freed):
 *   - Only the subtree under PGD/PML4 index 0 is walked: that is the only
 *     entry arch_vmm_create_process_pgd() builds privately; every other
 *     populated index (aarch64 256-511, amd64 1-255 MMIO + 256-511) is a
 *     by-value copy of a kernel_pgd entry and must not be touched.
 *   - Inside the walk, any directory entry whose VALUE equals the kernel's
 *     entry at the same position is shared (the create path copies kernel
 *     entries by value: aarch64 MMIO/RAM PUD slots and 2MB RAM blocks in
 *     the deep-copied PMD; amd64 PDPT[0]).  Shared entries are skipped.
 *   - Block entries (PTE_IS_TABLE false) are never descended: user mappings
 *     are always 4KB pages, so a differing block is still kernel memory
 *     (e.g. the kernel remapped after the by-value copy).
 *   - Leaf frames are freed only when the PTE carries PTE_USER: a private
 *     PT may also hold kernel PTEs (a kernel 2MB block split around the ELF
 *     header page at 0x7ffff000) whose frames belong to the kernel.
 *   - Differing table pages (PMD/PT/PUD) were allocated for this process by
 *     get_next_table()/the arch splitter and are freed unconditionally.
 *
 * There is no frame refcounting: user frames are never shared between
 * processes today (ELF loader and sbrk map freshly allocated frames; IPC,
 * blits and font loads copy by value).  Revisit when shared mappings arrive.
 *
 * NOTE(MM-VMM-02): table physical addresses are cast to pointers under the
 * identity-map invariant.
 *
 * Locking: caller must ensure no other CPU holds or will use this PGD
 * (process fully stopped and de-scheduled; see the scheduler reaper).
 */
void vmm_destroy_pgd(uint64_t *pgd) {
  if (!pgd)
    return;

  uint64_t freed_frames = 0;
  uint64_t freed_tables = 0;

  uint64_t *pud0 =
      (pgd[0] & PTE_VALID) ? (uint64_t *)(pgd[0] & PTE_ADDR_MASK) : NULL;
  uint64_t *k_pud0 = (kernel_pgd[0] & PTE_VALID)
                         ? (uint64_t *)(kernel_pgd[0] & PTE_ADDR_MASK)
                         : NULL;

  if (pud0 && pud0 != k_pud0) {
    for (int i = 0; i < 512; i++) {
      uint64_t pud_e = pud0[i];
      if (!(pud_e & PTE_VALID))
        continue;
      if (k_pud0 && pud_e == k_pud0[i])
        continue; /* shared kernel entry (MMIO block, kernel RAM table) */
      if (!PTE_IS_TABLE(pud_e))
        continue; /* 1GB block: never a user mapping */

      uint64_t *pmd = (uint64_t *)(pud_e & PTE_ADDR_MASK);
      uint64_t *k_pmd = NULL;
      if (k_pud0 && (k_pud0[i] & PTE_VALID) && PTE_IS_TABLE(k_pud0[i]))
        k_pmd = (uint64_t *)(k_pud0[i] & PTE_ADDR_MASK);

      for (int j = 0; j < 512; j++) {
        uint64_t pmd_e = pmd[j];
        if (!(pmd_e & PTE_VALID))
          continue;
        if (k_pmd && pmd_e == k_pmd[j])
          continue; /* by-value copy of a kernel entry (2MB RAM block) */
        if (!PTE_IS_TABLE(pmd_e))
          continue; /* private 2MB block: not user (user maps 4KB only) */

        uint64_t *pt = (uint64_t *)(pmd_e & PTE_ADDR_MASK);
        for (int k = 0; k < 512; k++) {
          uint64_t pte = pt[k];
          if ((pte & PTE_VALID) && (pte & PTE_USER)) {
            pmm_free_page((void *)(pte & PTE_ADDR_MASK));
            freed_frames++;
          }
        }
        pmm_free_page(pt);
        freed_tables++;
      }
      pmm_free_page(pmd);
      freed_tables++;
    }
    pmm_free_page(pud0);
    freed_tables++;
  }

  pmm_free_page((void *)pgd);
  freed_tables++;

  pr_info("VMM: PGD %p destroyed: freed %lu user frames, %lu table pages "
          "(free now %lu)\n",
          (void *)pgd, freed_frames, freed_tables, pmm_get_free_pages());
}
