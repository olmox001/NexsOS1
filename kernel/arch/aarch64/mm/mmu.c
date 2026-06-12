/*
 * kernel/arch/aarch64/mm/mmu.c
 * AArch64 4-level (48-bit VA, 4KB granule) page table management.
 *
 * Role:
 *   Provides the arch_vmm_* interface required by the generic VMM layer
 *   (kernel/mm/vmm.c) and by the kernel boot path (cpu.c:arch_vmm_init_hw).
 *   PGD arguments are PHYSICAL addresses; table memory is dereferenced
 *   through phys_to_virt() (higher-half direct map, kernel/memlayout.h).
 *   The kernel half lives in TTBR1_EL1; per-process user tables in TTBR0.
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
 *   AMMU-01 RESOLVED (Phase B2, b745a74): PAGE_KERNEL is RW+UXN+PXN; kernel
 *            text is mapped PAGE_KERNEL_RX and rodata PAGE_KERNEL_RO by
 *            vmm_map_ram_wx() — W^X holds for all RAM.
 *   AMMU-08 RESOLVED (Phase B2): not by new code but by the ISA — the TLB
 *            flush primitives in arch/arch.h use the inner-shareable TLBI
 *            variants (vaae1is/vmalle1is), which the hardware DVM broadcasts
 *            to every PE; DSB ISH waits for global completion.  The older
 *            "local-only" notes in this file were factually wrong.
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
 *   entry & PTE_VALID && entry & 0x2 → return (uint64_t *)phys_to_virt(entry & PTE_ADDR_MASK).
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
 *   TLB invalidation for the specific VA being modified (broadcast IS TLBI,
 *   so siblings are covered — AMMU-08 resolved).
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
      /* 'level' is the level of the table being RETURNED (call sites pass
       * 1/2/3 for the L0→L1, L1→L2, L2→L3 steps).  A block found at the
       * L1→L2 step (level==2) is a 1GB L1 block; one found at the L2→L3
       * step (level==3) is a 2MB L2 block.  FIX: the branches previously
       * tested level==1/level==2, so a 2MB block was "split" into an EMPTY
       * L3 table — silently unmapping the other 511 pages of the block (the
       * ELF header-page map at 0x7ffff000 hit exactly this; benign only
       * because no PMM frame from that physical range was ever handed out). */
      if (level == 2) {
          /* L1 Block (1GB) -> L2 Table (512 x 2MB blocks).
           * Each child is a 2MB block, so bit 1 stays 0 (block); bit 0 stays 1. */
          for (int i = 0; i < 512; i++) {
            sub_table[i] = (block_pa + (uint64_t)i * 0x200000) | (block_flags & ~0x2) | 0x1;
          }
      } else if (level == 3) {
          /* L2 Block (2MB) -> L3 Table (512 x 4KB pages).
           * Each child is an L3 page descriptor (bit 1 = 1). */
          for (int i = 0; i < 512; i++) {
            sub_table[i] = (block_pa + (uint64_t)i * 4096) | sub_flags;
          }
      } else {
          /* L0 entries are architecturally always table descriptors; a
           * "block" here means corruption — refuse instead of fabricating
           * an empty table. */
          pmm_free_page(new_table);
          return NULL;
      }

      arch_cache_clean_range(sub_table, 4096); /* write back new sub-table to DRAM */
      arch_mb(); /* DSB: ensure sub-table is visible before parent entry update */

      /* Update the parent table to point to the new sub-table (table descriptor). */
      table[index] = virt_to_phys(new_table) | PTE_TABLE | PTE_VALID;
      arch_cache_clean_range(&table[index], 8); /* write back parent entry */
      arch_mb();

      /* The caller's TLBI for the VA being mapped is the broadcast (IS)
       * variant; the block-split itself preserves the old translations
       * (same PA, same attributes at finer grain), so no extra flush is
       * required here. */

      return new_table;
    }
    /* Existing table descriptor: extract next-level table pointer from OA field. */
    return (uint64_t *)phys_to_virt(entry & PTE_ADDR_MASK);
  }

  /* Entry is absent (not valid); allocate a new table page if permitted. */
  if (!alloc) return NULL;

  void *page = pmm_alloc_page();
  if (!page) return NULL;

  memset(page, 0, 4096);
  arch_cache_clean_range(page, 4096);
  arch_mb();

  /* Install the new table as a table descriptor (PTE_TABLE | PTE_VALID). */
  table[index] = virt_to_phys(page) | PTE_TABLE | PTE_VALID;

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
 *   arch_tlb_flush_va is TLBI VAAE1IS: broadcast to every PE in the
 *   inner-shareable domain by hardware (AMMU-08 resolved — no IPI needed).
 */
int arch_vmm_map(uint64_t pgd_addr, uint64_t va, uint64_t pa, uint64_t flags) {
  uint64_t *pgd = (uint64_t *)phys_to_virt(pgd_addr);
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

  /* Flush TLB for this VA only if this PGD is currently live — in TTBR0
   * (a process half) or TTBR1 (the kernel half).  The TLBI is the
   * broadcast (IS) variant: siblings are covered too. */
  if (pgd_addr == arch_impl_get_pgd() ||
      pgd_addr == arch_impl_get_kernel_pgd()) {
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
 * arch_tlb_flush_va is TLBI VAAE1IS + DSB ISH: the invalidation is broadcast
 * to all PEs and awaited globally (AMMU-08 resolved) — when this returns no
 * sibling CPU still translates 'va' through the cleared entry.
 */
int arch_vmm_unmap(uint64_t pgd_addr, uint64_t va) {
  uint64_t *pgd = (uint64_t *)phys_to_virt(pgd_addr);
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
 * arch_vmm_protect - rewrite the attributes of existing 4KB mappings (AMMU-02).
 *
 * Parameters:
 *   pgd_addr  Physical address of the L0 page table.
 *   va, size  Range to change; rounded outward to page boundaries.
 *   flags     FULL aarch64 page profile (PAGE_KERNEL / PAGE_KERNEL_RO /
 *             PAGE_USER_DATA ... — the same vocabulary arch_vmm_map takes):
 *             every attribute bit of each leaf PTE is replaced; only the
 *             output address is preserved.
 *
 * A 1GB/2MB block covering part of the range is first split into the next
 * finer granularity (get_next_table with alloc=1) so the change applies with
 * 4KB precision; translations outside the requested range are preserved
 * bit-identically by the split.
 *
 * Returns 0 on success; -1 if any page in the range is unmapped or a split
 * allocation fails (pages BEFORE the failure keep the new attributes — the
 * TLB flush below runs on both paths so the PTEs already rewritten are
 * never left visible only in stale TLB copies).
 *
 * TLB: one broadcast invalidate-all (TLBI VMALLE1IS via arch_tlb_flush_all)
 * after the loop — cross-CPU by hardware, no IPI needed.
 */
int arch_vmm_protect(uint64_t pgd_addr, uint64_t va, uint64_t size, uint64_t flags) {
  uint64_t *pgd = (uint64_t *)phys_to_virt(pgd_addr);
  uint64_t v = va & ~0xFFFUL;
  uint64_t end = (va + size + 0xFFFUL) & ~0xFFFUL;
  int rc = 0;

  for (; v < end; v += 4096) {
    uint64_t pgde = pgd[PGD_INDEX(v)];
    if (!(pgde & PTE_VALID)) { rc = -1; break; }
    uint64_t *pud = (uint64_t *)phys_to_virt(pgde & PTE_ADDR_MASK); /* L0: always a table */

    uint64_t pude = pud[PUD_INDEX(v)];
    if (!(pude & PTE_VALID)) { rc = -1; break; }
    uint64_t *pmd;
    if (!(pude & 0x2)) {
      pmd = get_next_table(pud, PUD_INDEX(v), 1, 2); /* split 1GB block */
      if (!pmd) { rc = -1; break; }
    } else {
      pmd = (uint64_t *)phys_to_virt(pude & PTE_ADDR_MASK);
    }

    uint64_t pmde = pmd[PMD_INDEX(v)];
    if (!(pmde & PTE_VALID)) { rc = -1; break; }
    uint64_t *pt;
    if (!(pmde & 0x2)) {
      pt = get_next_table(pmd, PMD_INDEX(v), 1, 3); /* split 2MB block */
      if (!pt) { rc = -1; break; }
    } else {
      pt = (uint64_t *)phys_to_virt(pmde & PTE_ADDR_MASK);
    }

    uint64_t *pte = &pt[PT_INDEX(v)];
    if (!(*pte & PTE_VALID)) { rc = -1; break; }
    *pte = (*pte & PTE_ADDR_MASK) | flags;
    arch_cache_clean_range(pte, 8); /* visible to the hardware walker */
  }

  arch_mb();            /* DSB ISH: PTE writes complete before TLBI */
  arch_tlb_flush_all(); /* TLBI VMALLE1IS: broadcast to all PEs */
  arch_isb();
  return rc;
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
 * Handles 1GB (L1) and 2MB (L2) block descriptors by combining the block
 * base OA with the in-block offset of 'va' — previously a block anywhere on
 * the walk made the function return 0 (get_next_table with alloc=0 refuses
 * blocks), so lookups inside the 2MB-block RAM identity map always failed.
 */
uint64_t arch_vmm_get_physical(uint64_t pgd_addr, uint64_t va) {
  uint64_t *pgd = (uint64_t *)phys_to_virt(pgd_addr);

  uint64_t pgde = pgd[PGD_INDEX(va)];
  if (!(pgde & PTE_VALID)) return 0;
  uint64_t *pud = (uint64_t *)phys_to_virt(pgde & PTE_ADDR_MASK); /* L0: always a table */

  uint64_t pude = pud[PUD_INDEX(va)];
  if (!(pude & PTE_VALID)) return 0;
  if (!(pude & 0x2)) /* 1GB L1 block */
    return ((pude & PTE_ADDR_MASK) & ~0x3FFFFFFFUL) | (va & 0x3FFFFFFFUL);
  uint64_t *pmd = (uint64_t *)phys_to_virt(pude & PTE_ADDR_MASK);

  uint64_t pmde = pmd[PMD_INDEX(va)];
  if (!(pmde & PTE_VALID)) return 0;
  if (!(pmde & 0x2)) /* 2MB L2 block */
    return ((pmde & PTE_ADDR_MASK) & ~0x1FFFFFUL) | (va & 0x1FFFFFUL);
  uint64_t *pt = (uint64_t *)phys_to_virt(pmde & PTE_ADDR_MASK);

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
      uint64_t *pgd_ptr = (uint64_t *)phys_to_virt(pgd);
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
 * arch_vmm_create_process_pgd - allocate a new per-process (TTBR0) PGD.
 *
 * Returns: physical address of the new L0 page table, or 0 on PMM exhaustion.
 *
 * Higher-half model: the kernel lives entirely in TTBR1 (image, direct map
 * and MMIO at KERNEL_VIRT_BASE + PA — see memlayout.h), which the hardware
 * uses for every VA whose top bits are set.  A process PGD therefore
 * contains ONLY user mappings: it starts out completely empty and the ELF
 * loader / sbrk fill it via arch_vmm_map().  Nothing is shared with
 * kernel_pgd, which makes vmm_destroy_pgd's ownership rules trivial — every
 * table page reachable from this PGD belongs to the process.
 */
uint64_t arch_vmm_create_process_pgd(void) {
  uint64_t *pgd = (uint64_t *)pmm_alloc_page();
  if (!pgd) return 0;
  memset(pgd, 0, 4096);

  arch_cache_clean_range(pgd, 4096);
  arch_mb();
  /* Contract: the arch layer returns the PGD's PHYSICAL address;
   * vmm_create_pgd() converts back to a pointer. */
  return virt_to_phys(pgd);
}
