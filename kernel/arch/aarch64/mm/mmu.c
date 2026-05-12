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

static uint64_t *get_next_table(uint64_t *table, uint64_t index, int alloc) {
  if (table[index] & PTE_VALID) {
    return (uint64_t *)(table[index] & PTE_ADDR_MASK);
  }

  if (!alloc) return NULL;

  void *page = pmm_alloc_page();
  if (!page) return NULL;

  memset(page, 0, 4096);
  arch_cache_clean_range(page, 4096);
  arch_mb();

  table[index] = (uint64_t)page | PTE_TABLE | PTE_VALID | PTE_AF | PTE_INNER_SHARE |
                 PTE_RW | PTE_AP_EL1_RW | PTE_PXN;

  arch_cache_clean_range(&table[index], 8);
  arch_mb();

  return page;
}

int arch_vmm_map(uint64_t pgd_addr, uint64_t va, uint64_t pa, uint64_t flags) {
  uint64_t *pgd = (uint64_t *)pgd_addr;
  uint64_t *pud, *pmd, *pt;

  pud = get_next_table(pgd, PGD_INDEX(va), 1);
  if (!pud) return -1;

  pmd = get_next_table(pud, PUD_INDEX(va), 1);
  if (!pmd) return -1;

  pt = get_next_table(pmd, PMD_INDEX(va), 1);
  if (!pt) return -1;

  uint64_t *pt_entry = &pt[PT_INDEX(va)];
  *pt_entry = pa | flags;
  
  arch_cache_clean_range(pt_entry, 8);
  arch_mb();
  arch_tlb_flush_va(va);
  arch_mb();
  arch_isb();

  return 0;
}

int arch_vmm_unmap(uint64_t pgd_addr, uint64_t va) {
  uint64_t *pgd = (uint64_t *)pgd_addr;
  uint64_t *pud, *pmd, *pt;

  pud = get_next_table(pgd, PGD_INDEX(va), 0);
  if (!pud) return 0;

  pmd = get_next_table(pud, PUD_INDEX(va), 0);
  if (!pmd) return 0;

  pt = get_next_table(pmd, PMD_INDEX(va), 0);
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
      uint64_t *pud = get_next_table(pgd_ptr, PGD_INDEX(v), 1);
      if (!pud) return -1;
      
      uint64_t *pmd = get_next_table(pud, PUD_INDEX(v), 1);
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
