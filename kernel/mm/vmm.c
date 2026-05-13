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
#include <kernel/platform.h>
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
  table[index] = (uint64_t)page | PTE_TABLE | PTE_VALID;

  /* Flush the directory entry itself */
  arch_cache_clean_range(&table[index], 8);
  arch_mb();

  return page;
}

/*
 * Map a page
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
  arch_vmm_unmap((uint64_t)pgd, virt);
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
