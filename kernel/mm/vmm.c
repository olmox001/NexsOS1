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

  uint64_t phys = (uint64_t)page;

  // Actually pmm_alloc_page returns physical address + offset if using early
  // mapping? Wait, pmm returns direct mapped address usually... Let's assume
  // PMM returns physical address for now as per previous impl? Checking pmm
  // implementation... PMM returns (MEMORY_BASE + pfn_to_phys) which is physical
  // address in QEMU space (0x4000...).

  // In our simplified model:
  // Physical address IS the pointer returned by pmm (since we are 1:1 mapped
  // currently) But table entries store PHYSICAL addresses.

  phys = (uint64_t)page; // This is actually physical in current setup

  /* Zero and Flush the new table page */
  memset(page, 0, 4096);
  arch_clean_cache_range_va(page, 4096);
  arch_dsb();

  table[index] = phys | PTE_TABLE | PTE_VALID | PTE_AF | PTE_INNER_SHARE |
                 PTE_RW | PTE_AP_EL1_RW | PTE_PXN;

  /* Flush the directory entry itself */
  arch_clean_cache_va(&table[index]);
  arch_dsb();

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
  arch_clean_cache_va(pt_entry);
  arch_dsb();

  /* Flush TLB for this address to ensure consistency */
  arch_tlb_flush_va(virt);
  arch_dsb();
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
  arch_clean_cache_va(pt_entry);
  arch_dsb();

  /* TLB invalidate for this page */
  arch_tlb_flush_va(virt);
  arch_dsb();
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
  arch_clean_cache_range_va(kernel_pgd, 4096);

  /* 1. Map RAM with correct permissions */
  extern char _etext[];
  uint64_t etext_addr = (uint64_t)_etext;
  uint64_t ram_start = 0x40000000UL;
  uint64_t ram_size = 1024UL * 1024 * 1024; /* 1GB */

  /* For simplicity, we still map RAM in 2MB blocks where possible,
   * but we use 4KB pages for the transition between .text and .data
   * to ensure exact protection boundaries.
   * For now, let's just map the first 128MB with 4KB pages to be precise,
   * and the rest with 2MB blocks as DATA.
   */

  /* Map 0-128MB (Kernel region) precisely */
  for (uint64_t addr = ram_start; addr < ram_start + (128UL * 1024 * 1024);
       addr += 4096) {
    uint64_t flags = (addr < etext_addr) ? PAGE_KERNEL_EXEC : PAGE_KERNEL;

    /* Identity Map */
    vmm_map_page(kernel_pgd, addr, addr, flags);

    /* Alias Map (Higher half) */
    vmm_map_page(kernel_pgd, addr + 0x40000000, addr, flags);
  }

  /* Map the rest (128MB - 1GB) as DATA using 2MB blocks for performance */
  for (uint64_t addr = ram_start + (128UL * 1024 * 1024);
       addr < ram_start + ram_size; addr += (2UL * 1024 * 1024)) {
    uint64_t *pud = get_next_table(kernel_pgd, PGD_INDEX(addr), 1);
    uint64_t *pmd = get_next_table(pud, PUD_INDEX(addr), 1);
    if (pmd) {
      pmd[PMD_INDEX(addr)] = addr | (PAGE_KERNEL & ~PTE_PAGE);

      /* Alias Map */
      uint64_t vaddr = addr + 0x40000000;
      uint64_t *v_pud = get_next_table(kernel_pgd, PGD_INDEX(vaddr), 1);
      uint64_t *v_pmd = get_next_table(v_pud, PUD_INDEX(vaddr), 1);
      if (v_pmd) {
        v_pmd[PMD_INDEX(vaddr)] = addr | (PAGE_KERNEL & ~PTE_PAGE);
      }
    }
  }
  arch_dsb(); /* Wait for flushes */

  /* 2. Identity Map MMIO (UART, GIC, VirtIO) */
  /* 0x08000000 to 0x0A000000 covers typical QEMU virt devices */
  for (uint64_t addr = 0x08000000UL; addr < 0x0A800000UL; addr += 4096) {
    vmm_map_page(kernel_pgd, addr, addr, PAGE_DEVICE);
  }

  /* 3. Setup MAIR_EL1 (Memory Attribute Indirection Register) */
  /* Index 0: Normal Memory, Inner/Outer Write-Back Non-transient,
   * Read-Allocate, Write-Allocate */
  /* Index 1: Device Memory, nGnRE (non-Gathering, non-Reordering, Early Write
   * Acknowledgement) */
  uint64_t mair = (0xFFUL << 0) | /* Index 0: Normal */
                  (0x04UL << 8);  /* Index 1: Device nGnRE */
  arch_set_mair(mair);

  /* 4. Setup TCR_EL1 (Translation Control Register) */
  /* TG0=0 (4KB), SH0=3 (Inner Shareable), ORGN0=1 (WB/WA), IRGN0=1 (WB/WA) */
  /* T0SZ=16 (48-bit VA, 64 - 48 = 16) */
  /* IPS=0 (32-bit PA) or 2 (40-bit PA). QEMU virt uses 40-bit or 32-bit? */
  /* Let's use IPS=2 (40-bit) to be safe */
  uint64_t tcr = (16UL << 0) | /* T0SZ */
                 (3UL << 12) | /* SH0 Inner Shareable */
                 (1UL << 10) | /* ORGN0 WB/WA */
                 (1UL << 8) |  /* IRGN0 WB/WA */
                 (2UL << 32);  /* IPS 40-bit */
  arch_set_tcr(tcr);

  /* 5. Set TTBR0_EL1 */
  arch_set_ttbr0((uint64_t)kernel_pgd);

  /* 6. Enable MMU in SCTLR_EL1 */
  uint64_t sctlr = arch_get_sctlr();
  sctlr |= (1UL << 0) |  /* M: MMU enable */
           (1UL << 12) | /* I: Instruction cache enable */
           (1UL << 2);   /* C: Data cache enable */
  arch_set_sctlr(sctlr);
  arch_isb();

  pr_info("VMM: MMU Enabled. Kernel PGD at %p\n", (void *)kernel_pgd);
}

/*
 * Create a new PGD
 */
/*
 * Create a new PGD
 */
uint64_t *vmm_create_pgd(void) {
  uint64_t *pgd = (uint64_t *)pmm_alloc_page();
  if (!pgd)
    return NULL;

  /* Zero out new PGD */
  memset(pgd, 0, 4096);
  arch_clean_cache_range_va(pgd, 4096);

  /* We must provide the kernel identity map to the new process.
   * Our identity map is in PGD index 0.
   * To avoid sharing the entire 512GB branch with user space,
   * we allocate a PRIVATE PUD for index 0 and clone only the kernel's entries.
   */
  uint64_t *src_pud = (uint64_t *)(kernel_pgd[0] & PTE_ADDR_MASK);
  if (src_pud) {
    uint64_t *dst_pud = (uint64_t *)pmm_alloc_page();
    if (dst_pud) {
      memset(dst_pud, 0, 4096);
      /* Clone ONLY kernel PUD entries:
       * Index 0: MMIO (0x0 to 0x3FFFFFFF approx)
       * Index 1: RAM (0x40000000 to 0x7FFFFFFF)
       * User Space starts at 0x80000000 (Index 2) - which we leave EMPTY here.
       */
      dst_pud[0] = src_pud[0];
      dst_pud[1] = src_pud[1];
      arch_clean_cache_range_va(dst_pud, 4096);
      pgd[0] = (uint64_t)dst_pud | (kernel_pgd[0] & ~PTE_ADDR_MASK);
      arch_clean_cache_va(&pgd[0]);
    }
  }

  /* Also copy other PGD entries if any (usually none for now) */
  for (int i = 1; i < 512; i++) {
    pgd[i] = kernel_pgd[i];
    if (pgd[i])
      arch_clean_cache_va(&pgd[i]);
  }
  arch_dsb();

  return pgd;
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
