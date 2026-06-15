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
 * PA/VA contract (kernel/memlayout.h):
 *   In this file `uint64_t *pgd` parameters and pmm_alloc_page() results
 *   are kernel VIRTUAL pointers; the arch_vmm_* layer takes the PGD as a
 *   PHYSICAL address, so every delegation below converts with
 *   virt_to_phys().  PTE output addresses are physical and are converted
 *   with phys_to_virt() before being dereferenced (get_next_table) or
 *   freed (vmm_destroy_pgd).  Identity while KERNEL_VIRT_BASE == 0.
 *
 * Known issues:
 *   MM-VMM-01  (FIXED)           W^X enforced via vmm_map_ram_wx(): kernel
 *                                 text RX, rodata RO+NX, all other RAM RW+NX.
 *   MM-VMM-02  RESOLVED (Phase B2): all walkers translate PTE physical
 *                                 addresses through phys_to_virt() and
 *                                 store via virt_to_phys(); the kernel now
 *                                 RUNS in the higher half on both arches
 *                                 (KERNEL_VIRT_BASE in memlayout.h).
 *   MM-VMM-03  (W2 REFINE/TODO)  vmm_dynamic_remap leaks the old PGD.
 *   MM-VMM-04  (W3 BUG)          vmm_destroy_pgd leaks user RAM frames and
 *                                 contains a dead empty loop.
 *   MM-VMM-05  RESOLVED (Phase B2): unmap paths now satisfy the cross-CPU
 *                                 shootdown contract — AArch64 in hardware
 *                                 (broadcast IS TLBI), amd64 via LAPIC IPI
 *                                 (kernel/arch/amd64/mm/tlb.c); teardown
 *                                 broadcasts one full shootdown before
 *                                 frames are recycled.
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
/* kernel_pgd - kernel VIRTUAL pointer to the current kernel page-global
 * directory (the hardware register gets virt_to_phys(kernel_pgd): TTBR1 on
 * aarch64, CR3 on amd64).  Updated atomically (pointer assignment) in
 * vmm_dynamic_remap() after the hardware root has been switched; secondary
 * CPUs read it after arch_mb(). */
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
 * PA/VA: entries hold PHYSICAL addresses; dereferences go through
 * phys_to_virt() and new tables are installed via virt_to_phys()
 * (MM-VMM-02 resolved — see kernel/memlayout.h).
 *
 * Locking: caller must hold the relevant PGD/process mm_lock; this helper
 * does not take any lock itself.
 */
static uint64_t *get_next_table(uint64_t *table, uint64_t index, int alloc) {
  if (table[index] & PTE_VALID) {
    /* Table entries store PHYSICAL addresses (48 bits supported); translate
     * through phys_to_virt() (direct map, MM-VMM-02). */
    uint64_t phys = table[index] & 0x0000FFFFFFFFF000UL;
    return (uint64_t *)phys_to_virt(phys);
  }

  if (!alloc)
    return NULL;

  /* Allocate a new page for the table.  pmm_alloc_page() returns a pointer
   * usable by the kernel; what gets STORED in the entry is its physical
   * address (virt_to_phys below). */
  void *page = pmm_alloc_page();
  if (!page)
    return NULL;

  /* Zero and Flush the new table page */
  memset(page, 0, 4096);
  arch_cache_clean_range(page, 4096);
  arch_mb();

  /* Table entry flags:
   * AArch64: Valid (bit 0), Table (bit 1), AF (bit 10), Inner Share (bits 8-9),
   *          AP EL0 RW (bit 6-7, usually ignored for tables but safe).
   * AMD64:   Present (bit 0), RW (bit 1), User (bit 2).
   */
  table[index] = virt_to_phys(page) | PTE_TABLE | PTE_VALID;

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
 *   pgd   - kernel virtual pointer to the PGD (converted to its PA for the
 *           arch layer via virt_to_phys).
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

  /* arch_vmm_map takes the PGD as a physical address. */
  return arch_vmm_map(virt_to_phys(pgd), virt, phys, flags);
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
 * Parameters:
 *   pgd        - PGD to walk (kernel virtual pointer).
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
 * implementation for details on the walk (blocks/large pages included).
 *
 * Returns the physical address, or 0 / arch-defined sentinel on unmapped VA.
 */
uint64_t vmm_get_phys(uint64_t *pgd, uint64_t virt) {
  return arch_vmm_get_physical(virt_to_phys(pgd), virt);
}

/*
 * Unmap a page
 *
 * vmm_unmap_page - remove a single page mapping from the given PGD.
 *
 * Delegates to arch_vmm_unmap(), which clears the PTE and performs an SMP
 * TLB shootdown (MM-VMM-05 resolved): on return, no online CPU still
 * translates 'virt' through the old entry — the caller may safely recycle
 * the backing frame.  AArch64 satisfies this with broadcast IS TLBI in
 * hardware; amd64 with a LAPIC IPI round (see arch/amd64/mm/tlb.c).
 */
void vmm_unmap_page(uint64_t *pgd, uint64_t virt) {
  arch_vmm_unmap(virt_to_phys(pgd), virt);
}

/*
 * vmm_protect - rewrite the attributes of existing mappings (AMMU-02).
 *
 * Thin contract wrapper over arch_vmm_protect(): 'flags' is the arch's
 * PAGE/PTE profile, frame addresses are preserved, large pages split for
 * 4KB precision, and a cross-CPU TLB shootdown runs before returning.
 * Returns 0, or -1 if the range contains an unmapped page (pages before
 * the hole keep the new attributes).
 */
int vmm_protect(uint64_t *pgd, uint64_t virt, uint64_t size, uint64_t flags) {
  return arch_vmm_protect(virt_to_phys(pgd), virt, size, flags);
}

/* Internal helper with locking */
/*
 * vmm_unmap_page_locked - vmm_unmap_page() wrapped with proc->mm_lock.
 *
 * Acquires proc->mm_lock with IRQ save/restore around the unmap call.
 * The shootdown inside runs with IRQs masked; on amd64 a peer spinning on
 * this same mm_lock cannot ack until it unmasks — the bounded ack wait in
 * tlb.c turns that worst case into a stall, never a deadlock.
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
 * vmm_map_ram_wx - map RAM at its direct-map VA with the W^X section split
 * (MM-VMM-01 / AMMU-01 resolved).
 *
 * The kernel image layout (kernel.ld, both arches) is:
 *   [__text_start, _etext)      text                                  → RX
 *   [_etext≈, __erodata)        rodata, .ktests, .ksyms               → RO+NX
 *   everything else              data/bss/heap/stacks/DTB/boot region  → RW+NX
 * Boundaries are page-rounded; the linker aligns sections to 4 KB so the
 * windows never overlap.  arch_vmm_map_range still uses 2MB blocks inside
 * each window where alignment permits.
 *
 * Enforcement: aarch64 PXN/UXN bits (always honoured at EL1);
 * amd64 PTE_NX (EFER.NXE set at boot) + CR0.WP for the read-only text.
 */
void vmm_map_ram_wx(uint64_t *pgd, uint64_t base, uint64_t size) {
  extern char __text_start[], _etext[], __erodata[];
  /* Section symbols are link (virtual) addresses; 'base'/'cur' iterate
   * PHYSICAL addresses — compare in the physical domain and map each
   * chunk at its direct-map VA (phys_to_virt).  The RX window starts at
   * __text_start, NOT __kernel_start: on amd64 the latter also covers the
   * low boot region (1..2MB), which is data once the kernel runs. */
  uint64_t tx_s = virt_to_phys(__text_start) & ~0xFFFUL;
  uint64_t tx_e = (virt_to_phys(_etext) + 4095) & ~0xFFFUL;
  uint64_t ro_e = (virt_to_phys(__erodata) + 4095) & ~0xFFFUL;
  uint64_t end = base + size;
  uint64_t cur = base;

  while (cur < end) {
    uint64_t flags;
    if (cur >= tx_s && cur < tx_e)
      flags = PAGE_KERNEL_RX;
    else if (cur >= tx_e && cur < ro_e)
      flags = PAGE_KERNEL_RO;
    else
      flags = PAGE_KERNEL;

    /* Map up to the next window boundary (or the range end). */
    uint64_t next = end;
    if (tx_s > cur && tx_s < next)
      next = tx_s;
    if (tx_e > cur && tx_e < next)
      next = tx_e;
    if (ro_e > cur && ro_e < next)
      next = ro_e;

    arch_vmm_map_range(virt_to_phys(pgd), (uint64_t)phys_to_virt(cur), cur,
                       next - cur, flags);
    cur = next;
  }
}

/*
 * Initialize VMM and Enable MMU
 *
 * vmm_init - phase-1 VMM bring-up: build the first real kernel PGD (128 MB
 *            bootstrap window) and install it as the kernel root.
 *
 * The MMU itself is already ON when this runs: the boot assembly (both
 * arches) enables it with boot tables so C executes at its higher-half
 * link address from the first instruction.
 *
 * Steps:
 *   1. Allocate and zero the kernel PGD from the PMM (sets global kernel_pgd).
 *   2. Map [ARCH_RAM_START, ARCH_RAM_START+128MB) at the direct-map VAs via
 *      vmm_map_ram_wx() (W^X section split).
 *   3. Map MMIO regions via arch_vmm_map_mmio().
 *   4. Install virt_to_phys(kernel_pgd) as the kernel root via
 *      arch_vmm_init_hw() (TTBR1 on aarch64, CR3 on amd64).
 *
 * After vmm_init() only the first 128 MB of RAM are mapped;
 * vmm_dynamic_remap() extends coverage to all detected RAM.
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

  /* 1. Map RAM (bootstrap) with the W^X section split: text RX, rodata RO+NX,
   * the rest RW+NX (MM-VMM-01).  Map at least 128MB, but always far enough to
   * cover the PMM metadata: arch_vmm_init_hw() switches the page table and then
   * touches page_array via pmm_alloc_page(), so the metadata must be mapped in
   * THIS pgd.  A boot module (release rootfs) can push the metadata well past
   * the kernel image, so a fixed 128MB window is not enough — extend to the
   * metadata top. */
  uint64_t ram_start = ARCH_RAM_START;
  uint64_t ram_size = 128UL * 1024 * 1024; /* Enough to boot */
  uint64_t meta_top = pmm_metadata_top();
  if (meta_top > ram_start) {
    uint64_t need = PAGE_ALIGN(meta_top - ram_start + 2UL * 1024 * 1024);
    if (need > ram_size)
      ram_size = need;
  }

  vmm_map_ram_wx(kernel_pgd, ram_start, ram_size);

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

  /* 1. Map all discovered memory regions with the W^X section split
   * (text RX, rodata RO+NX, all other RAM RW+NX — MM-VMM-01 resolved).
   * arch_vmm_map_range still uses 2MB blocks inside each window. */
  size_t count = 0;
  struct mem_region *regions = arch_platform_get_mem_regions(&count);
  for (size_t i = 0; i < count; i++) {
    if (regions[i].type == MEM_REGION_USABLE) {
      pr_info("VMM: Mapping region 0x%lx - 0x%lx\n",
              regions[i].base, regions[i].base + regions[i].size);
      vmm_map_ram_wx(new_pgd, regions[i].base, regions[i].size);
    }
  }

  /* 2. Re-map MMIO */
  arch_vmm_map_mmio(new_pgd);

  /* 3. Atomically switch to the new PGD (kernel root: TTBR1 on aarch64,
   * CR3 on amd64) */
  uint64_t new_phys = virt_to_phys(new_pgd);

  /* Swap the global kernel_pgd */
  uint64_t *old_pgd = kernel_pgd;
  kernel_pgd = new_pgd;

  arch_vmm_set_kernel_pgd(new_phys);
  arch_tlb_flush_local();
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
 * Returns: kernel virtual pointer to the new PGD (the arch layer returns
 * its physical address; converted here via phys_to_virt).
 */
uint64_t *vmm_create_pgd(void) {
  uint64_t pgd_phys = arch_vmm_create_process_pgd();
  if (!pgd_phys)
    return NULL;
  return (uint64_t *)phys_to_virt(pgd_phys);
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
 * Table physical addresses are dereferenced through phys_to_virt()
 * (MM-VMM-02 resolved).
 *
 * Locking: caller must ensure no other CPU holds or will use this PGD
 * (process fully stopped and de-scheduled; see the scheduler reaper).
 */
void vmm_destroy_pgd(uint64_t *pgd) {
  if (!pgd)
    return;

  /* Cross-CPU shootdown BEFORE freeing (MM-VMM-05): a peer that ran this
   * process earlier may still hold user translations into frames we are
   * about to recycle.  One full round here covers the whole address space;
   * the per-PTE walk below then frees without further TLB traffic. */
  arch_tlb_shootdown_all();

  uint64_t freed_frames = 0;
  uint64_t freed_tables = 0;

  uint64_t *pud0 =
      (pgd[0] & PTE_VALID) ? (uint64_t *)phys_to_virt(pgd[0] & PTE_ADDR_MASK) : NULL;
  uint64_t *k_pud0 = (kernel_pgd[0] & PTE_VALID)
                         ? (uint64_t *)phys_to_virt(kernel_pgd[0] & PTE_ADDR_MASK)
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

      uint64_t *pmd = (uint64_t *)phys_to_virt(pud_e & PTE_ADDR_MASK);
      uint64_t *k_pmd = NULL;
      if (k_pud0 && (k_pud0[i] & PTE_VALID) && PTE_IS_TABLE(k_pud0[i]))
        k_pmd = (uint64_t *)phys_to_virt(k_pud0[i] & PTE_ADDR_MASK);

      for (int j = 0; j < 512; j++) {
        uint64_t pmd_e = pmd[j];
        if (!(pmd_e & PTE_VALID))
          continue;
        if (k_pmd && pmd_e == k_pmd[j])
          continue; /* by-value copy of a kernel entry (2MB RAM block) */
        if (!PTE_IS_TABLE(pmd_e))
          continue; /* private 2MB block: not user (user maps 4KB only) */

        uint64_t *pt = (uint64_t *)phys_to_virt(pmd_e & PTE_ADDR_MASK);
        for (int k = 0; k < 512; k++) {
          uint64_t pte = pt[k];
          if ((pte & PTE_VALID) && (pte & PTE_USER)) {
            /* PTE carries the frame's PHYSICAL address; the PMM takes
             * direct-map pointers. */
            pmm_free_page(phys_to_virt(pte & PTE_ADDR_MASK));
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
