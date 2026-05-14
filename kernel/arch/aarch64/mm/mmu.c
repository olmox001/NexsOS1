/*
 * kernel/arch/aarch64/mm/mmu.c
 * AArch64 4-level page table management
 */
#include <kernel/arch.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/string.h>

/* Page Table Indices */
#define PGD_INDEX(x) (((x) >> 39) & 0x1FF)
#define PUD_INDEX(x) (((x) >> 30) & 0x1FF)
#define PMD_INDEX(x) (((x) >> 21) & 0x1FF)
#define PT_INDEX(x) (((x) >> 12) & 0x1FF)

#define PTE_ADDR_MASK 0x0000FFFFFFFFF000UL

static uint64_t *get_next_table(uint64_t *table, uint64_t index, int alloc, int level) {
  uint64_t entry = table[index];

  if (entry & PTE_VALID) {
    /* Check if this is a Block Mapping (Level 1 or 2) */
    /* Bit 1 is 0 for Block, 1 for Table/Page */
    if (!(entry & 0x2)) {
      if (!alloc) return NULL;

      /* SPLIT BLOCK: Convert Block to Table */
      void *new_table = pmm_alloc_page();
      if (!new_table) return NULL;
      memset(new_table, 0, 4096);

      uint64_t block_pa = entry & PTE_ADDR_MASK;
      uint64_t block_flags = entry & ~PTE_ADDR_MASK;
      /* Convert Block flags to Page/Table flags (bit 1 becomes 1) */
      uint64_t sub_flags = block_flags | 0x2;

      uint64_t *sub_table = (uint64_t *)new_table;
      if (level == 1) {
          /* L1 Block (1GB) -> L2 Table (512 x 2MB blocks) */
          for (int i = 0; i < 512; i++) {
            sub_table[i] = (block_pa + (uint64_t)i * 0x200000) | (block_flags & ~0x2) | 0x1; // L2 Block
          }
      } else if (level == 2) {
          /* L2 Block (2MB) -> L3 Table (512 x 4KB pages) */
          for (int i = 0; i < 512; i++) {
            sub_table[i] = (block_pa + (uint64_t)i * 4096) | sub_flags;
          }
      }

      arch_cache_clean_range(sub_table, 4096);
      arch_mb();

      /* Update the parent table to point to the new sub-table */
      table[index] = (uint64_t)new_table | PTE_TABLE | PTE_VALID;
      arch_cache_clean_range(&table[index], 8);
      arch_mb();
      
      /* Note: We should flush TLB here if it's the active PGD, 
       * but we'll let the caller handle it or assume it's fine for now. */

      return new_table;
    }
    return (uint64_t *)(entry & PTE_ADDR_MASK);
  }

  if (!alloc) return NULL;

  void *page = pmm_alloc_page();
  if (!page) return NULL;

  memset(page, 0, 4096);
  arch_cache_clean_range(page, 4096);
  arch_mb();

  table[index] = (uint64_t)page | PTE_TABLE | PTE_VALID;

  arch_cache_clean_range(&table[index], 8);
  arch_mb();

  return (uint64_t *)page;
}

int arch_vmm_map(uint64_t pgd_addr, uint64_t va, uint64_t pa, uint64_t flags) {
  uint64_t *pgd = (uint64_t *)pgd_addr;
  uint64_t *pud, *pmd, *pt;

  pud = get_next_table(pgd, PGD_INDEX(va), 1, 1);
  if (!pud) return -1;

  pmd = get_next_table(pud, PUD_INDEX(va), 1, 2);
  if (!pmd) return -1;

  pt = get_next_table(pmd, PMD_INDEX(va), 1, 3);
  if (!pt) return -1;

  uint64_t *pt_entry = &pt[PT_INDEX(va)];
  *pt_entry = pa | flags;
  
  arch_cache_clean_range(pt_entry, 8);
  arch_mb();

  /* Only flush TLB if we are modifying the ACTIVE page table */
  if (pgd_addr == arch_impl_get_pgd()) {
    arch_tlb_flush_va(va);
    arch_mb();
    arch_isb();
  }

  return 0;
}

int arch_vmm_unmap(uint64_t pgd_addr, uint64_t va) {
  uint64_t *pgd = (uint64_t *)pgd_addr;
  uint64_t *pud, *pmd, *pt;

  pud = get_next_table(pgd, PGD_INDEX(va), 0, 1);
  if (!pud) return 0;

  pmd = get_next_table(pud, PUD_INDEX(va), 0, 2);
  if (!pmd) return 0;

  pt = get_next_table(pmd, PMD_INDEX(va), 0, 3);
  if (!pt) return 0;

  uint64_t *pt_entry = &pt[PT_INDEX(va)];
  *pt_entry = 0;

  arch_cache_clean_range(pt_entry, 8);
  arch_mb();
  arch_tlb_flush_va(va);
  arch_mb();
  arch_isb();

  return 0;
}

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

  return (entry & PTE_ADDR_MASK) | (va & 0xFFF);
}

int arch_vmm_map_range(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t size, uint64_t flags) {
  uint64_t v = va;
  uint64_t p = pa;
  uint64_t end = va + size;

  while (v < end) {
    uint64_t remaining = end - v;
    
    /* Can we use a 2MB block? 
     * 1. v and p must be 2MB aligned.
     * 2. remaining must be >= 2MB.
     */
    if ((v & 0x1FFFFF) == 0 && (p & 0x1FFFFF) == 0 && remaining >= 0x200000) {
      uint64_t *pgd_ptr = (uint64_t *)pgd;
      uint64_t *pud = get_next_table(pgd_ptr, PGD_INDEX(v), 1, 1);
      if (!pud) return -1;
      
      uint64_t *pmd = get_next_table(pud, PUD_INDEX(v), 1, 2);
      if (!pmd) return -1;
      
      /* Level 2 Block Mapping (2MB) */
      /* Note: PTE_TABLE is 0b11. Block is 0b01.
       * So we use (flags & ~0x2) | 0x1 if we wanted to be precise, 
       * but PAGE_KERNEL already has the right bits for a page. 
       * For AArch64: Page = 0x3, Block = 0x1.
       */
      pmd[PMD_INDEX(v)] = p | (flags & ~0x2) | 0x1;
      
      arch_cache_clean_range(&pmd[PMD_INDEX(v)], 8);
      v += 0x200000;
      p += 0x200000;
    } else {
      /* Fallback to 4KB page */
      if (arch_vmm_map(pgd, v, p, flags) != 0) return -1;
      v += 4096;
      p += 4096;
    }
  }
  return 0;
}
uint64_t arch_vmm_create_process_pgd(void) {
  uint64_t *pgd = (uint64_t *)pmm_alloc_page();
  if (!pgd) return 0;
  memset(pgd, 0, 4096);

  extern uint64_t *kernel_pgd;
  
  /* Copy kernel mappings (upper half) */
  for (int i = 256; i < 512; i++) {
    pgd[i] = kernel_pgd[i];
  }

  /* Clone kernel identity map (lower half, index 0) */
  uint64_t *src_pud = (uint64_t *)(kernel_pgd[0] & PTE_ADDR_MASK);
  if (src_pud && (kernel_pgd[0] & 0x2)) {
    uint64_t *dst_pud = (uint64_t *)pmm_alloc_page();
    if (dst_pud) {
      memset(dst_pud, 0, 4096);
      /* Clone the PUD entries for MMIO (0-1GB) and Kernel (1-2GB).
       * User-space typically starts at 2GB (PUD index 2), so we leave it free. */
      if (src_pud[0] & PTE_VALID) dst_pud[0] = src_pud[0];
      if (src_pud[1] & PTE_VALID) dst_pud[1] = src_pud[1];
      arch_cache_clean_range(dst_pud, 4096);
      pgd[0] = (uint64_t)dst_pud | (kernel_pgd[0] & ~PTE_ADDR_MASK);
    }
  }

  arch_cache_clean_range(pgd, 4096);
  arch_mb();
  return (uint64_t)pgd;
}
