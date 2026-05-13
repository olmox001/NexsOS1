/*
 * kernel/mm/vmm.c
 * Virtual Memory Manager
 *
 * AArch64 4-level page table management
 */
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>

/* Page Table Levels */
#define PGD_SHIFT 39
#define PUD_SHIFT 30
#define PMD_SHIFT 21
#define PT_SHIFT 12

#define PGD_INDEX(x) (((x) >> PGD_SHIFT) & 0x1FF)
#define PUD_INDEX(x) (((x) >> PUD_SHIFT) & 0x1FF)
#define PMD_INDEX(x) (((x) >> PMD_SHIFT) & 0x1FF)
#define PT_INDEX(x) (((x) >> PT_SHIFT) & 0x1FF)

/* Global Kernel PGD */
uint64_t *kernel_pgd;

#define PTE_ADDR_MASK 0x0000FFFFFFFFF000UL

/*
 * Get or create next level table
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
  uint64_t table_flags = PTE_TABLE | PTE_VALID;
#ifdef ARCH_AARCH64
  table_flags |= PTE_AF | PTE_INNER_SHARE | PTE_AP_EL0_RW;
#endif

  table[index] = (uint64_t)page | table_flags;

  /* Flush the directory entry itself */
  arch_cache_clean_range(&table[index], 8);
  arch_mb();

  return page;
}

/*
 * Map a page
 */
int vmm_map_page(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags) {
  // pr_info("VMM: Mapping 0x%lx -> 0x%lx (flags 0x%lx)\n", virt, phys, flags);
  if ((virt & 0xFFF) || (phys & 0xFFF)) {
    pr_err("VMM: Invalid alignment virt=%lx phys=%lx\n", virt, phys);
    return -1;
  }

  uint64_t *pud, *pmd, *pt;

  /* Level 0: PGD */
  pud = get_next_table(pgd, PGD_INDEX(virt), 1);
  if (!pud)
    return -1;

  /* Level 1: PUD */
  pmd = get_next_table(pud, PUD_INDEX(virt), 1);
  if (!pmd)
    return -1;

  /* Level 2: PMD */
  pt = get_next_table(pmd, PMD_INDEX(virt), 1);
  if (!pt)
    return -1;

  /* Level 3: PT */
  uint64_t *pt_entry = &pt[PT_INDEX(virt)];
  uint64_t current = *pt_entry;
  if (current & PTE_VALID) {
    if ((current & PTE_ADDR_MASK) != (phys & PTE_ADDR_MASK)) {
      pr_warn("VMM: Remapping occupied VA %lx (Phys: %lx -> %lx)\n", virt,
              current & PTE_ADDR_MASK, phys);
    }
  }

  *pt_entry = phys | flags;
  arch_cache_clean_range(pt_entry, 8);
  arch_mb();

  /* Flush TLB for this address to ensure consistency */
  arch_tlb_flush_va(virt);
  arch_mb();
  arch_isb();

  return 0;
}

/* Internal helper with locking */
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
 * Unmap a page
 */
void vmm_unmap_page(uint64_t *pgd, uint64_t virt) {
  uint64_t *pud, *pmd, *pt;

  pud = get_next_table(pgd, PGD_INDEX(virt), 0);
  if (!pud)
    return;

  pmd = get_next_table(pud, PUD_INDEX(virt), 0);
  if (!pmd)
    return;

  pt = get_next_table(pmd, PMD_INDEX(virt), 0);
  if (!pt)
    return;

  uint64_t *pt_entry = &pt[PT_INDEX(virt)];
  *pt_entry = 0;
  arch_cache_clean_range(pt_entry, 8);
  arch_mb();

  /* TLB Invalidation */
  arch_tlb_flush_va(virt);
  arch_mb();
  arch_isb();
}

/* Internal helper with locking */
void vmm_unmap_page_locked(struct process *proc, uint64_t virt) {
  uint64_t lock_flags;
  spin_lock_irqsave(&proc->mm_lock, &lock_flags);
  vmm_unmap_page(proc->page_table, virt);
  spin_unlock_irqrestore(&proc->mm_lock, lock_flags);
}

/*
 * Map a range of memory
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
 */
void vmm_init(void) {
  pr_info("%s", "VMM: Initializing MMU...\n");

  /* Allocate Kernel PGD */
  kernel_pgd = pmm_alloc_page();
  if (!kernel_pgd) {
    panic("VMM: Failed to allocate kernel PGD\n");
  }
  memset(kernel_pgd, 0, 4096);
  arch_cache_clean_range(kernel_pgd, 4096);

  /* 1. Map RAM with correct permissions */
  extern char _etext[];
  uint64_t etext_addr = (uint64_t)_etext;
  uint64_t ram_start = ARCH_RAM_START;
  uint64_t ram_size = ARCH_RAM_SIZE;
  uint64_t alias_offset = ARCH_ALIAS_OFFSET;

  /* Map kernel image precisely with 4KB pages */
  uint64_t kernel_map_end = ram_start + (128UL * 1024 * 1024); /* First 128MB */
  if (kernel_map_end > ram_start + ram_size) kernel_map_end = ram_start + ram_size;

  for (uint64_t addr = ram_start; addr < kernel_map_end; addr += 4096) {
    uint64_t flags = (addr < etext_addr) ? PAGE_KERNEL_EXEC : PAGE_KERNEL;

    /* Identity Map */
    vmm_map_page(kernel_pgd, addr, addr, flags);

    /* Alias Map (Higher half) */
    if (alias_offset != 0) {
      vmm_map_page(kernel_pgd, addr + alias_offset, addr, flags);
    }
  }

  /* Map the rest of RAM using optimized range mapping */
  if (ram_start + ram_size > kernel_map_end) {
    uint64_t remaining_size = (ram_start + ram_size) - kernel_map_end;
    
    /* Identity Map */
    arch_vmm_map_range((uint64_t)kernel_pgd, kernel_map_end, kernel_map_end, 
                       remaining_size, PAGE_KERNEL);
    
    /* Alias Map (Higher half) */
    if (alias_offset != 0) {
      arch_vmm_map_range((uint64_t)kernel_pgd, kernel_map_end + alias_offset, 
                         kernel_map_end, remaining_size, PAGE_KERNEL);
    }
  }

  arch_mb(); /* Wait for flushes */

  /* 2. Platform-specific MMIO Identity Mapping */
  arch_vmm_map_mmio(kernel_pgd);

  /* 3. Platform-specific hardware initialization (Enable MMU/Paging) */
  arch_vmm_init_hw((uint64_t)kernel_pgd);
}

/*
 * Create a new PGD
 */
uint64_t *vmm_create_pgd(void) {
  return (uint64_t *)arch_vmm_create_process_pgd();
}

/*
 * Destroy a PGD and all mapped user-space page tables
 */
void vmm_destroy_pgd(uint64_t *pgd) {
  if (!pgd)
    return;

  /* We only need to free user space tables (indices 256-511 for higher half,
   * but our user space is 0x0... which is 0-255)
   * WAIT: Our user map is 0x0 - 0x0000_FFFF_FFFF_FFFF.
   * That corresponds to indices 0 to 255 in PGD.
   * However, index 0 is SHARED (with private PUD).
   */

  /* 1. Handle Private PUD at Index 0 */
  uint64_t *pud0 = (uint64_t *)(pgd[0] & PTE_ADDR_MASK);
  uint64_t *k_pud0 = (uint64_t *)(kernel_pgd[0] & PTE_ADDR_MASK);
  if (pud0 && pud0 != k_pud0) {
    /* Recursively free PUD0 entries starting from index 2 (User space) */
    for (int i = 2; i < 512; i++) {
      if (pud0[i] & PTE_VALID) {
        uint64_t *pmd = (uint64_t *)(pud0[i] & PTE_ADDR_MASK);
        /* Free PMD and its tables */
        for (int j = 0; j < 512; j++) {
          if (pmd[j] & PTE_VALID) {
            if (!(pmd[j] & PTE_TABLE))
              continue; // Skip blocks
            uint64_t *pte = (uint64_t *)(pmd[j] & PTE_ADDR_MASK);
            /* Free L3 PTE pages (frame pointers)? No, we don't free the actual
               RAM frames here as they might be shared. But we MUST free the
               table pages. */
            pmm_free_page(pte);
          }
        }
        pmm_free_page(pmd);
      }
    }
    pmm_free_page(pud0);
  }

  /* 2. Free other PGD entries (if any were used for user space) */
  for (int i = 1; i < 512; i++) {
    /* If we used higher PGD indices for user space, they would be here.
       In our current model, only index 0's PUD is cloned. */
  }

  pmm_free_page((void *)pgd);
}
