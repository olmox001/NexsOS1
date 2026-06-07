/*
 * kernel/arch/aarch64/mm/mmu.c
 * AArch64 4-level (48-bit VA, 4KB granule) page table management.
 *
 * Role:
 *   Provides the arch_vmm_* interface required by the generic VMM layer
 *   (kernel/mm/vmm.c) and by the kernel boot path (cpu.c:arch_vmm_init_hw).
 *   All operations are in terms of physical addresses because the kernel
 *   identity-maps itself via TTBR0_EL1 for the low 4GB.
 *
 * Page table layout (ARMv8-A 48-bit VA, 4KB granule, TCR.T0SZ=16):
 *   Level 0 (PGD) — 9 bits [47:39] — table descriptor only (512 entries × 8B).
 *   Level 1 (PUD) — 9 bits [38:30] — table OR 1GB block descriptors.
 *   Level 2 (PMD) — 9 bits [29:21] — table OR 2MB block descriptors.
 *   Level 3 (PT)  — 9 bits [20:12] — 4KB page descriptors.
 *
 * AArch64 descriptor bit format (simplified):
 *   [1:0]  = 0b11 for a table/page entry (PTE_TABLE | PTE_VALID).
 *   [1:0]  = 0b01 for a block entry (PTE_VALID but bit 1 clear).
 *   [47:12] = output physical address (OA).
 *   Upper/lower attribute bits encode MAIR index, AP, SH, UXN, PXN, AF, nG, etc.
 *
 * Block splitting (get_next_table with alloc=1 on a block entry):
 *   When a 1GB (L1) or 2MB (L2) block entry exists at a level and a finer
 *   granularity is needed (e.g. to map a single 4KB page inside a 2MB block),
 *   the block is split into a new sub-table whose entries reproduce the same
 *   physical mapping at the next level.
 *
 * Known issues:
 *   AMMU-01 (W3 SECURITY) Normal RAM is mapped executable (no W^X): PAGE_KERNEL
 *            does not set UXN/PXN for RAM pages; only device pages have UXN set.
 *            A kernel code injection via a write to RAM is not prevented. [static]
 *   AMMU-08 (W2 BUG) arch_vmm_map issues a local TLB flush only (arch_tlb_flush_va);
 *            no SMP IPI shootdown is sent to sibling CPUs.  Stale TLB entries on
 *            other CPUs may cause them to use invalidated mappings. [static]
 */
#include <kernel/arch.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/string.h>

/*
 * VA-to-table-index macros (4KB granule, 48-bit VA, TCR.T0SZ=16).
 * Each level uses 9 bits of the virtual address:
 *   PGD_INDEX: bits [47:39]
 *   PUD_INDEX: bits [38:30]
 *   PMD_INDEX: bits [29:21]
 *   PT_INDEX:  bits [20:12]
 * Result is 0..511 (the index into a 512-entry page table).
 */
#define PGD_INDEX(x) (((x) >> 39) & 0x1FF)
#define PUD_INDEX(x) (((x) >> 30) & 0x1FF)
#define PMD_INDEX(x) (((x) >> 21) & 0x1FF)
#define PT_INDEX(x)  (((x) >> 12) & 0x1FF)

/*
 * PTE_ADDR_MASK - bits that hold the output physical address in a PTE.
 * Bits [47:12] are the OA for 4KB granule.  Bits [11:0] are attribute/control
 * bits; bits [63:48] are reserved (or IGNORED/UXN/PXN in upper attributes).
 */
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000UL

/*
 * get_next_table - walk or allocate the next-level page table at a given index.
 *
 * Parameters:
 *   table  Pointer to the current-level page table (512 × uint64_t).
 *   index  Index into table (0..511).
 *   alloc  If non-zero, allocate a new sub-table (or split a block) when the
 *          entry is absent or is a block entry.  If zero, return NULL for absent
 *          or block entries without allocation.
 *   level  Current table level: 1 = PGD→PUD walk (L0→L1), 2 = PUD→PMD (L1→L2),
 *          3 = PMD→PT (L2→L3).  Used only during block-split to determine which
 *          block size is being split.
 *
 * Returns: pointer to the next-level table, or NULL on failure / absent entry
 *          when alloc==0.
 *
 * Fast path (table entry already a table descriptor):
 *   entry & PTE_VALID && entry & 0x2 → return (uint64_t *)(entry & PTE_ADDR_MASK).
 *
 * Block-split path (entry valid but bit 1 == 0, i.e. a block descriptor):
 *   Called when arch_vmm_map needs to place a 4KB page inside an existing
 *   1GB (L1) or 2MB (L2) block mapping.
 *   Steps:
 *     1. Allocate a new 4KB sub-table (pmm_alloc_page, zeroed).
 *     2. Populate each sub-table entry to reproduce the same physical mapping
 *        at the finer granularity:
 *          L1 block (1GB, level==1): 512 × 2MB L2 block entries, each offset by
 *            i × 2MB from the original block base PA.
 *          L2 block (2MB, level==2): 512 × 4KB L3 page entries, each offset by
 *            i × 4KB from the original block base PA.
 *     3. Cache-clean the new sub-table and the parent entry; DSB via arch_mb().
 *     4. Write the parent entry as a table descriptor pointing to the new sub-table.
 *
 *   NOTE: No TLB flush is issued here; the caller (arch_vmm_map) handles the
 *   TLB invalidation for the specific VA being modified, but sibling CPUs are
 *   not shot down (AMMU-08).
 *
 * Fresh-allocation path (entry == 0, alloc == 1):
 *   Allocate and zero a new 4KB page; write as a table descriptor; cache-clean.
 *
 * Attribute notes for block-split:
 *   block_flags = entry & ~PTE_ADDR_MASK preserves upper/lower attribute bits.
 *   sub_flags   = block_flags | 0x2: sets bit 1 (the "table/page" discriminator)
 *                 for L3 page entries.
 *   L2 block entries use (block_flags & ~0x2) | 0x1: bit 1 = 0 (block), bit 0 = 1.
 */
static uint64_t *get_next_table(uint64_t *table, uint64_t index, int alloc, int level) {
  uint64_t entry = table[index];

  if (entry & PTE_VALID) {
    /* AArch64 descriptor bit 1: 0 = block descriptor, 1 = table/page descriptor.
     * At L3 all valid entries are page descriptors (bit 1 must be 1).
     * At L0 all valid entries must be table descriptors (no block at L0). */
    if (!(entry & 0x2)) {
      /* Block mapping at L1 (1GB) or L2 (2MB) — must split to allow finer map. */
      if (!alloc) return NULL; /* caller did not request allocation; fail cleanly */

      /* SPLIT BLOCK: Convert Block to Table */
      void *new_table = pmm_alloc_page();
      if (!new_table) return NULL;
      memset(new_table, 0, 4096);

      uint64_t block_pa    = entry & PTE_ADDR_MASK;  /* physical base of block */
      uint64_t block_flags = entry & ~PTE_ADDR_MASK; /* all attribute bits */
      /* sub_flags: page-level version of the same attributes (bit 1 set). */
      uint64_t sub_flags = block_flags | 0x2;

      uint64_t *sub_table = (uint64_t *)new_table;
      if (level == 1) {
          /* L1 Block (1GB) -> L2 Table (512 x 2MB blocks).
           * Each child is a 2MB block, so bit 1 stays 0 (block); bit 0 stays 1. */
          for (int i = 0; i < 512; i++) {
            sub_table[i] = (block_pa + (uint64_t)i * 0x200000) | (block_flags & ~0x2) | 0x1;
          }
      } else if (level == 2) {
          /* L2 Block (2MB) -> L3 Table (512 x 4KB pages).
           * Each child is an L3 page descriptor (bit 1 = 1). */
          for (int i = 0; i < 512; i++) {
            sub_table[i] = (block_pa + (uint64_t)i * 4096) | sub_flags;
          }
      }

      arch_cache_clean_range(sub_table, 4096); /* write back new sub-table to DRAM */
      arch_mb(); /* DSB: ensure sub-table is visible before parent entry update */

      /* Update the parent table to point to the new sub-table (table descriptor). */
      table[index] = (uint64_t)new_table | PTE_TABLE | PTE_VALID;
      arch_cache_clean_range(&table[index], 8); /* write back parent entry */
      arch_mb();

      /* NOTE(AMMU-08): TLB for any VA covered by the old block is now stale on
       * CPUs other than this one.  The caller issues a local TLB flush for the
       * specific VA being mapped but does not broadcast an SMP shootdown. */

      return new_table;
    }
    /* Existing table descriptor: extract next-level table pointer from OA field. */
    return (uint64_t *)(entry & PTE_ADDR_MASK);
  }

  /* Entry is absent (not valid); allocate a new table page if permitted. */
  if (!alloc) return NULL;

  void *page = pmm_alloc_page();
  if (!page) return NULL;

  memset(page, 0, 4096);
  arch_cache_clean_range(page, 4096);
  arch_mb();

  /* Install the new table as a table descriptor (PTE_TABLE | PTE_VALID). */
  table[index] = (uint64_t)page | PTE_TABLE | PTE_VALID;

  arch_cache_clean_range(&table[index], 8);
  arch_mb();

  return (uint64_t *)page;
}

/*
 * arch_vmm_map - create a single 4KB page mapping (VA → PA) in the given PGD.
 *
 * Parameters:
 *   pgd_addr  Physical address of the L0 page table.
 *   va        Virtual address to map (must be 4KB aligned; low 12 bits ignored
 *             in the PTE but should be 0 for correctness).
 *   pa        Physical address of the target page (4KB aligned).
 *   flags     PTE attribute bits (PTE_VALID | PTE_TABLE | MAIR index | AP | SH |
 *             UXN | PXN | AF | nG ...).  Combined with pa via bitwise OR.
 * Returns: 0 on success, -1 on PMM exhaustion.
 *
 * Walks three levels (PGD → PUD → PMD → PT) via get_next_table() with alloc=1,
 * allocating new sub-tables or splitting existing block mappings as needed.
 * Then writes the L3 page descriptor pa|flags into PT[PT_INDEX(va)].
 *
 * Cache coherency: pt_entry is cleaned to PoC (arch_cache_clean_range) so the
 * hardware page-table walker (which may cache-bypass to main memory) sees the
 * new entry.  arch_mb() (DSB ISH) ensures the write is globally visible before
 * the TLB invalidation.
 *
 * TLB maintenance:
 *   A local TLB flush for the specific VA is issued only when pgd_addr matches
 *   the currently loaded TTBR0_EL1 (i.e., this is the active address space).
 *   Mappings in inactive PGDs do not require a TLB flush at map time because
 *   they are not cached in any CPU's TLB yet.
 *   NOTE(AMMU-08): The flush is LOCAL only (arch_tlb_flush_va = TLBI VAE1 or
 *   equivalent); no SMP shootdown IPI is issued. [static]
 */
int arch_vmm_map(uint64_t pgd_addr, uint64_t va, uint64_t pa, uint64_t flags) {
  uint64_t *pgd = (uint64_t *)pgd_addr;
  uint64_t *pud, *pmd, *pt;

  pud = get_next_table(pgd, PGD_INDEX(va), 1, 1); /* L0 → L1 */
  if (!pud) return -1;

  pmd = get_next_table(pud, PUD_INDEX(va), 1, 2); /* L1 → L2 */
  if (!pmd) return -1;

  pt = get_next_table(pmd, PMD_INDEX(va), 1, 3);  /* L2 → L3 */
  if (!pt) return -1;

  /* Write the L3 page descriptor.  flags must include PTE_VALID (bit 0) and
   * PTE_TABLE (bit 1) for a page entry; pa provides the OA at bits [47:12]. */
  uint64_t *pt_entry = &pt[PT_INDEX(va)];
  *pt_entry = pa | flags;

  arch_cache_clean_range(pt_entry, 8); /* flush PT entry to PoC for page walker */
  arch_mb(); /* DSB ISH: ensure entry visible to all observers before TLB op */

  /* Flush TLB for this VA only if this PGD is currently loaded in TTBR0_EL1.
   * NOTE(AMMU-08): local-only TLBI; sibling CPUs may still hold stale entries. */
  if (pgd_addr == arch_impl_get_pgd()) {
    arch_tlb_flush_va(va);
    arch_mb();
    arch_isb();
  }

  return 0;
}

/*
 * arch_vmm_unmap - remove the 4KB page mapping for va from the given PGD.
 *
 * Parameters:
 *   pgd_addr  Physical address of the L0 page table.
 *   va        Virtual address of the page to unmap.
 * Returns: 0 always (idempotent if the mapping did not exist).
 *
 * Walks without allocating (alloc=0): if any level is absent, returns 0
 * immediately (mapping was never created; nothing to do).
 *
 * Clears the PT entry to 0 (invalid), cleans it to PoC, then issues a full
 * DSB + TLBI VAE1 (via arch_tlb_flush_va) + ISB sequence.
 *
 * NOTE: The page table sub-levels (PT, PMD, PUD) are NOT freed even when they
 * become entirely empty after the unmap.  This means unmapping many individual
 * pages from a large process VA space accumulates empty page-table pages.
 *
 * NOTE(AMMU-08): arch_tlb_flush_va is a local-only TLBI.  Sibling CPUs may
 * continue to use a stale TLB entry for va until their next context switch or
 * explicit TLBI broadcast. [static]
 */
int arch_vmm_unmap(uint64_t pgd_addr, uint64_t va) {
  uint64_t *pgd = (uint64_t *)pgd_addr;
  uint64_t *pud, *pmd, *pt;

  pud = get_next_table(pgd, PGD_INDEX(va), 0, 1); /* no alloc */
  if (!pud) return 0; /* not mapped at this level; nothing to do */

  pmd = get_next_table(pud, PUD_INDEX(va), 0, 2);
  if (!pmd) return 0;

  pt = get_next_table(pmd, PMD_INDEX(va), 0, 3);
  if (!pt) return 0;

  uint64_t *pt_entry = &pt[PT_INDEX(va)];
  *pt_entry = 0; /* clear: PTE_VALID bit 0 = 0 → entry is invalid */

  arch_cache_clean_range(pt_entry, 8); /* writeback cleared entry for page walker */
  arch_mb();                            /* DSB ISH before TLBI */
  arch_tlb_flush_va(va);               /* TLBI VAE1 for this VA on local CPU */
  arch_mb();
  arch_isb();                           /* pipeline flush after TLBI */

  return 0;
}

/*
 * arch_vmm_get_physical - translate a virtual address to its physical address.
 *
 * Parameters:
 *   pgd_addr  Physical address of the L0 page table.
 *   va        Virtual address to translate.
 * Returns: physical address with page offset, or 0 if not mapped / not valid.
 *
 * Walks all four levels without allocating (alloc=0 at each level).  If any
 * level table is absent, or the final L3 entry has PTE_VALID clear, returns 0.
 *
 * The physical address is reconstructed as:
 *   (entry & PTE_ADDR_MASK) | (va & 0xFFF)
 * where PTE_ADDR_MASK extracts the OA [47:12] and (va & 0xFFF) restores the
 * 12-bit page offset from the original virtual address.
 *
 * Note: This function does not handle block mappings (L1/L2 blocks).  If a
 * block mapping exists at L1 or L2, get_next_table with alloc=0 returns NULL
 * for the "is this a block?" check, and the function returns 0. [static, known
 * limitation — see get_next_table for block handling when alloc=1]
 */
uint64_t arch_vmm_get_physical(uint64_t pgd_addr, uint64_t va) {
  uint64_t *pgd = (uint64_t *)pgd_addr;
  uint64_t *pud, *pmd, *pt;

  pud = get_next_table(pgd, PGD_INDEX(va), 0, 1);
  if (!pud) return 0;

  pmd = get_next_table(pud, PUD_INDEX(va), 0, 2);
  if (!pmd) return 0;

  pt = get_next_table(pmd, PMD_INDEX(va), 0, 3);
  if (!pt) return 0;

  uint64_t entry = pt[PT_INDEX(va)];
  if (!(entry & PTE_VALID)) return 0;

  /* Reconstruct PA: OA from PTE [47:12] | page offset from VA [11:0]. */
  return (entry & PTE_ADDR_MASK) | (va & 0xFFF);
}

/*
 * arch_vmm_map_range - map a contiguous VA range to a contiguous PA range.
 *
 * Parameters:
 *   pgd    Physical address of the L0 page table.
 *   va     Start virtual address (must be page-aligned).
 *   pa     Start physical address (must be page-aligned).
 *   size   Length of the mapping in bytes (must be a multiple of PAGE_SIZE).
 *   flags  PTE attribute bits, interpreted as for arch_vmm_map.
 * Returns: 0 on success, -1 on PMM exhaustion or table walk failure.
 *
 * Performance optimisation: uses 2MB block descriptors (L2 blocks) when both
 * v and p are 2MB-aligned AND at least 2MB remains to be mapped.  This reduces
 * the number of page-table allocations and TLB entries significantly for large
 * contiguous regions (e.g., kernel .text/.data, RAM identity map).
 *
 * 2MB block descriptor format:
 *   bits[1:0] = 0b01 (block, not table/page): PTE_VALID but bit 1 clear.
 *   (flags & ~0x2): clear bit 1 from the flags, which are otherwise page-level
 *     flags (0b11); for a block the hardware requires bit 1 = 0.
 *   Result: p | (flags & ~0x2) | 0x1.
 *
 * 4KB page fallback (arch_vmm_map):
 *   Used when alignment or remaining size prevents a 2MB block.  This path also
 *   handles the final sub-2MB tail of a region.
 *
 * NOTE: arch_mb / TLB flush after 2MB block writes are omitted; the function
 * relies on the caller to issue a global TLB flush (e.g., arch_vmm_init_hw)
 * after the full table is built.  Inserting per-entry TLBIs here would be
 * correct but very slow for large ranges.
 */
int arch_vmm_map_range(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t size, uint64_t flags) {
  uint64_t v = va;
  uint64_t p = pa;
  uint64_t end = va + size;

  while (v < end) {
    uint64_t remaining = end - v;

    /* 2MB block optimisation: use an L2 block descriptor when both VA and PA
     * are 2MB-aligned and at least 2MB remains unmapped. */
    if ((v & 0x1FFFFF) == 0 && (p & 0x1FFFFF) == 0 && remaining >= 0x200000) {
      uint64_t *pgd_ptr = (uint64_t *)pgd;
      uint64_t *pud = get_next_table(pgd_ptr, PGD_INDEX(v), 1, 1);
      if (!pud) return -1;

      uint64_t *pmd = get_next_table(pud, PUD_INDEX(v), 1, 2);
      if (!pmd) return -1;

      /* Write a 2MB L2 block descriptor:
       *   AArch64 block entry: [1:0] = 0b01 (PTE_VALID=1, table-bit=0).
       *   (flags & ~0x2): strip the page-level table-bit from flags so the
       *   hardware interprets this as a block, not a table pointer.
       *   | 0x1: set PTE_VALID.
       * Result: hardware-correct 2MB block OA=p with flags' attributes. */
      pmd[PMD_INDEX(v)] = p | (flags & ~0x2) | 0x1;

      arch_cache_clean_range(&pmd[PMD_INDEX(v)], 8); /* flush entry to PoC */
      v += 0x200000;
      p += 0x200000;
    } else {
      /* Fallback to 4KB page via the full 4-level walk. */
      if (arch_vmm_map(pgd, v, p, flags) != 0) return -1;
      v += 4096;
      p += 4096;
    }
  }
  return 0;
}
/*
 * arch_vmm_create_process_pgd - allocate and populate a new per-process PGD.
 *
 * Returns: physical address of the new L0 page table, or 0 on PMM exhaustion.
 *
 * A process PGD is constructed by:
 *   1. Allocating a fresh, zeroed 4KB page as the L0 table.
 *   2. Copying kernel mappings (PGD entries 256..511, the "upper half"):
 *      In a 48-bit VA layout with T0SZ=16, the kernel lives above
 *      0x0000_8000_0000_0000 (bit 47 set), which maps to PGD indices 256–511.
 *      These entries are shared (not copied by value of the sub-tables; the same
 *      L1 table pointers are placed in the new PGD), so all processes share the
 *      kernel virtual mapping.
 *   3. Cloning the lower-half identity map (PGD index 0):
 *      The kernel's L0[0] points to a PUD covering the low 512GB (0..511 GB).
 *      To avoid sharing the user-space portion of that PUD, a new PUD is
 *      allocated and only entries 0 and 1 (covering 0..2GB: MMIO at 0..1GB,
 *      kernel RAM identity-map at 1..2GB) are copied from the kernel PUD.
 *      PUD index 2 and above (covering 2GB..512GB, the user VA region starting
 *      at 0x80000000) are left as zero, so each process gets its own private
 *      user address space.
 *
 * After this function returns, user-space mappings are installed into the new
 * PGD via arch_vmm_map() / arch_vmm_map_range() with the process's own PGD addr.
 *
 * NOTE: The upper-half kernel PGD entries (256..511) are copied by value, meaning
 * each process's L0 table directly points to the same L1 tables as kernel_pgd.
 * Any new kernel L1/L2/L3 entries added after process creation will automatically
 * be visible to all processes (desired behaviour for kernel mappings).
 */
uint64_t arch_vmm_create_process_pgd(void) {
  uint64_t *pgd = (uint64_t *)pmm_alloc_page();
  if (!pgd) return 0;
  memset(pgd, 0, 4096);

  extern uint64_t *kernel_pgd;

  /* Copy kernel mappings (upper half: PGD indices 256..511).
   * PGD index i covers VA [i * 512GB .. (i+1) * 512GB).
   * Indices 256..511 cover VA ≥ 0x8000_0000_0000 — the kernel virtual range. */
  for (int i = 256; i < 512; i++) {
    pgd[i] = kernel_pgd[i];
  }

  /* Clone kernel identity map (lower half, PGD index 0).
   * kernel_pgd[0] is a table descriptor pointing to a PUD.
   * We allocate a NEW PUD for the process and selectively copy only:
   *   PUD[0]: MMIO region (0x0000_0000 .. 0x4000_0000, 1GB).
   *   PUD[1]: Kernel RAM identity map (0x4000_0000 .. 0x8000_0000, 1GB).
   * PUD[2..511] (user VA from 0x80000000 upward) are left at zero (private). */
  uint64_t *src_pud = (uint64_t *)(kernel_pgd[0] & PTE_ADDR_MASK);
  if (src_pud && (kernel_pgd[0] & 0x2)) { /* verify kernel_pgd[0] is a table descriptor */
    uint64_t *dst_pud = (uint64_t *)pmm_alloc_page();
    if (dst_pud) {
      memset(dst_pud, 0, 4096);
      /* Clone the PUD entries for MMIO (0-1GB) and Kernel (1-2GB).
       * User-space typically starts at 2GB (PUD index 2), so we leave it free. */
      if (src_pud[0] & PTE_VALID) dst_pud[0] = src_pud[0]; /* MMIO 1GB block */
      if (src_pud[1] & PTE_VALID) dst_pud[1] = src_pud[1]; /* kernel RAM 1GB block */
      arch_cache_clean_range(dst_pud, 4096);
      /* Install new PUD as table descriptor; preserve attribute bits from kernel_pgd[0]. */
      pgd[0] = (uint64_t)dst_pud | (kernel_pgd[0] & ~PTE_ADDR_MASK);
    }
  }

  arch_cache_clean_range(pgd, 4096);
  arch_mb();
  return (uint64_t)pgd;
}
