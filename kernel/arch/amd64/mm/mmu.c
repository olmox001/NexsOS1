/*
 * kernel/arch/amd64/mm/mmu.c
 * x86-64 Paging Implementation (PML4)
 */
#include <arch/amd64_internal.h>
#include <arch/arch.h>
#include <kernel/pmm.h>
#include <kernel/platform.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include "../../../include/kernel/pmm.h"

#ifndef PAGE_MASK
#endif /* PAGE_MASK */
/* Page Table Entry Flags */
#define X86_PTE_P 0x001         /* Present */
#define X86_PTE_RW 0x002        /* Read/Write */
#define X86_PTE_US 0x004        /* User/Supervisor */
#define X86_PTE_PWT 0x008       /* Page-level write-through */
#define X86_PTE_PCD 0x010       /* Page-level cache disable */
#define X86_PTE_A 0x020         /* Accessed */
#define X86_PTE_D 0x040         /* Dirty */
#define X86_PTE_PAT 0x080       /* PAT */
#define X86_PTE_G 0x100         /* Global */
#define X86_PTE_NX (1ULL << 63) /* No Execute */

/* Masks */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Indices */
#define PML4_INDEX(a) (((a) >> 39) & 0x1FF)
#define PDPT_INDEX(a) (((a) >> 30) & 0x1FF)
#define PD_INDEX(a) (((a) >> 21) & 0x1FF)
#define PT_INDEX(a) (((a) >> 12) & 0x1FF)

int arch_vmm_map(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t flags);
int arch_vmm_unmap(uint64_t pgd, uint64_t va);
uint64_t arch_vmm_get_physical(uint64_t pgd, uint64_t va);

extern uint64_t boot_pml4[];

void arch_vmm_init_hw(uint64_t kernel_pgd) {
  /* Switch to the new kernel PML4 */
  arch_vmm_set_pgd(kernel_pgd);
  pr_info("AMD64 VMM: Switched to kernel PGD at %p\n", (void *)kernel_pgd);

  /* Identity map all detected RAM regions */
  size_t count = 0;
  struct mem_region *regions = arch_platform_get_mem_regions(&count);

  /* Map low 1MB for SMP trampolines and legacy bios data */
  arch_vmm_map_range(kernel_pgd, 0, 0, 0x100000, PTE_RW);

  for (size_t i = 0; i < count; i++) {
    if (regions[i].type == MEM_REGION_USABLE) {
      pr_info("AMD64 VMM: Identity mapping RAM 0x%lx - 0x%lx\n", 
              regions[i].base, regions[i].base + regions[i].size);
      arch_vmm_map_range(kernel_pgd, regions[i].base, regions[i].base, 
                         regions[i].size, PTE_RW); /* Kernel RW */
    }
  }

  /* Identity map MMIO regions (LAPIC, VirtIO) */
  arch_vmm_map_mmio((uint64_t *)kernel_pgd);
}

void arch_vmm_map_mmio(uint64_t *pgd) {
  /* 1. Identity Map PCI MMIO and System MMIO (0xFE000000 to 0xFFFFFFFF) */
  /* This covers PCI devices, LAPIC, IOAPIC, and BIOS ranges */
  for (uint64_t addr = 0xFE000000UL; addr < 0xFFFFFFFFUL; addr += 4096) {
    arch_vmm_map((uint64_t)pgd, addr, addr, X86_PTE_P | X86_PTE_RW | X86_PTE_PCD | X86_PTE_PWT);
  }
}

void arch_vmm_init(void) {
  /* Boot PML4 is already set up with identity map by boot.S */
  pr_info("AMD64 VMM initialized (PML4 @ %p)\n", (void *)boot_pml4);
}

/* Map a virtual page to a physical page */
int arch_vmm_map(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t flags) {
  uint64_t *pml4 = (uint64_t *)pgd;

  /* Convert abstract AArch64 flags to x86-64 PTE flags */
  uint64_t x86_flags = X86_PTE_P;
  if (flags & PTE_USER)
    x86_flags |= X86_PTE_US;

  /* AArch64 AP[2] bit (PTE_RO) determines read-only vs read-write */
  if (!(flags & PTE_RO))
    x86_flags |= X86_PTE_RW;

  /* AArch64 uses PTE_UXN / PTE_PXN. If both are set, it's not executable. */
  if ((flags & PTE_UXN) && (flags & PTE_PXN))
    x86_flags |= X86_PTE_NX;

  /* Handle Device attribute (uncached/write-through) */
  if (((flags >> 2) & 0x7) == PTE_ATTR_DEVICE) {
    x86_flags |= X86_PTE_PCD | X86_PTE_PWT;
  }

  /* PML4 -> PDPT */
  int pml4_idx = PML4_INDEX(va);
  if (!(pml4[pml4_idx] & X86_PTE_P)) {
    uint64_t new_pdpt = (uint64_t)pmm_alloc_page();
    if (!new_pdpt)
      return -1;
    memset((void *)new_pdpt, 0, PAGE_SIZE);
    pml4[pml4_idx] = new_pdpt | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
  }
  uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_ADDR_MASK);

  /* PDPT -> PD */
  int pdpt_idx = PDPT_INDEX(va);
  if (!(pdpt[pdpt_idx] & X86_PTE_P)) {
    uint64_t new_pd = (uint64_t)pmm_alloc_page();
    if (!new_pd)
      return -1;
    memset((void *)new_pd, 0, PAGE_SIZE);
    pdpt[pdpt_idx] = new_pd | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
  }
  uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_ADDR_MASK);

  /* PD -> PT */
  int pd_idx = PD_INDEX(va);
  if (!(pd[pd_idx] & X86_PTE_P)) {
    uint64_t new_pt = (uint64_t)pmm_alloc_page();
    if (!new_pt)
      return -1;
    memset((void *)new_pt, 0, PAGE_SIZE);
    pd[pd_idx] = new_pt | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
  }
  uint64_t *pt = (uint64_t *)(pd[pd_idx] & PTE_ADDR_MASK);

  /* Set PT entry */
  int pt_idx = PT_INDEX(va);
  pt[pt_idx] = (pa & PTE_ADDR_MASK) | x86_flags;

  arch_tlb_flush_va(va);
  return 0;
}

int arch_vmm_unmap(uint64_t pgd, uint64_t va) {
  uint64_t *pml4 = (uint64_t *)pgd;

  if (!(pml4[PML4_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pdpt = (uint64_t *)(pml4[PML4_INDEX(va)] & PTE_ADDR_MASK);

  if (!(pdpt[PDPT_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pd = (uint64_t *)(pdpt[PDPT_INDEX(va)] & PTE_ADDR_MASK);

  if (!(pd[PD_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pt = (uint64_t *)(pd[PD_INDEX(va)] & PTE_ADDR_MASK);

  pt[PT_INDEX(va)] = 0;
  arch_tlb_flush_va(va);
  return 0;
}

uint64_t arch_vmm_get_physical(uint64_t pgd, uint64_t va) {
  uint64_t *pml4 = (uint64_t *)pgd;

  if (!(pml4[PML4_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pdpt = (uint64_t *)(pml4[PML4_INDEX(va)] & PTE_ADDR_MASK);

  if (!(pdpt[PDPT_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pd = (uint64_t *)(pdpt[PDPT_INDEX(va)] & PTE_ADDR_MASK);

  if (!(pd[PD_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pt = (uint64_t *)(pd[PD_INDEX(va)] & PTE_ADDR_MASK);

  if (!(pt[PT_INDEX(va)] & X86_PTE_P))
    return 0;

  return (pt[PT_INDEX(va)] & PTE_ADDR_MASK) | (va & 0xFFF);
}

uint64_t arch_vmm_create_process_pgd(void);
void arch_vmm_destroy_process_pgd(uint64_t pgd);

/* Clone kernel mappings for a new process PML4 */
uint64_t arch_vmm_create_process_pgd(void) {
  uint64_t new_pml4_phys = (uint64_t)pmm_alloc_page();
  if (!new_pml4_phys)
    return 0;

  uint64_t *new_pml4 = (uint64_t *)new_pml4_phys;
  memset(new_pml4, 0, PAGE_SIZE);

  /* 
   * AMD64 Paging Architecture:
   * PML4 index 0 to 255: Lower Half (User Space + Identity Map Kernel)
   * PML4 index 256 to 511: Higher Half (Kernel space / Alias)
   */

  extern uint64_t *kernel_pgd;
  
  /* 1. Copy Higher Half (Indices 256-511) - These are fully shared kernel tables */
  for (int i = 256; i < 512; i++) {
    new_pml4[i] = kernel_pgd[i];
  }

  /* 
   * 2. Handle Index 0 (Identity Map + User Space)
   * We CANNOT share the PDPT at index 0 because it will be modified for user space.
   * We must allocate a private PDPT for the new process and only copy the 
   * kernel-specific identity mappings into it.
   */
  uint64_t new_pdpt_phys = (uint64_t)pmm_alloc_page();
  if (!new_pdpt_phys) {
    pmm_free_page((void *)new_pml4_phys);
    return 0;
  }
  uint64_t *new_pdpt = (uint64_t *)new_pdpt_phys;
  memset(new_pdpt, 0, PAGE_SIZE);
  
  /* Link new PDPT to index 0 of PML4 */
  new_pml4[0] = new_pdpt_phys | X86_PTE_P | X86_PTE_RW | X86_PTE_US;

  /* 3. Copy all RAM Identity Mappings from the kernel's PDPT index 0.
   * This covers the entire range discovered during boot.
   */
  uint64_t *kern_pud0 = (uint64_t *)(kernel_pgd[0] & PTE_ADDR_MASK);
  if (kern_pud0) {
    for (int i = 0; i < 512; i++) {
        if (kern_pud0[i] & X86_PTE_P) {
            new_pdpt[i] = kern_pud0[i];
        }
    }
  }

  /* 4. Map MMIO ranges manually into the new PGD to ensure they have their own
   * private path if they share a PDPT with user space.
   */
  arch_vmm_map_mmio(new_pml4);

  return new_pml4_phys;
}

void arch_vmm_destroy_process_pgd(uint64_t pgd) {
  if (!pgd)
    return;

  /* Only free user space mappings (lower half). BUT wait, we shared the first
   * GB. If we recursively free, we will free kernel pages! So we should ONLY
   * free user pages (from PMM list tracked by process, which we do in
   * process.c). Here we just free the PML4 table itself. A full tree-walk free
   * needs to carefully avoid kernel PDs.
   */
  pmm_free_page((void *)pgd);
}

int arch_vmm_protect(uint64_t pgd, uint64_t va, uint64_t size, uint64_t flags);
int arch_vmm_protect(uint64_t pgd, uint64_t va, uint64_t size, uint64_t flags) {
  /* Simple stub: real OS would update PTE flags */
  (void)pgd;
  (void)va;
  (void)size;
  (void)flags;
  return 0;
}

int arch_vmm_map_range(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t size, uint64_t flags) {
  uint64_t v = va;
  uint64_t p = pa;
  uint64_t end = va + size;

  while (v < end) {
    uint64_t remaining = end - v;
    
    if ((v & 0x1FFFFF) == 0 && (p & 0x1FFFFF) == 0 && remaining >= 0x200000) {
      uint64_t *pml4 = (uint64_t *)pgd;
      
      int pml4_idx = PML4_INDEX(v);
      if (!(pml4[pml4_idx] & X86_PTE_P)) {
        uint64_t new_pdpt = (uint64_t)pmm_alloc_page();
        if (!new_pdpt) return -1;
        memset((void *)new_pdpt, 0, PAGE_SIZE);
        pml4[pml4_idx] = new_pdpt | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
      }
      uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & PTE_ADDR_MASK);

      int pdpt_idx = PDPT_INDEX(v);
      if (!(pdpt[pdpt_idx] & X86_PTE_P)) {
        uint64_t new_pd = (uint64_t)pmm_alloc_page();
        if (!new_pd) return -1;
        memset((void *)new_pd, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = new_pd | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
      }
      uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & PTE_ADDR_MASK);

      /* Level 2 2MB Page Mapping */
      uint64_t x86_flags = X86_PTE_P | 0x080; /* PS bit for 2MB */
      if (flags & PTE_USER) x86_flags |= X86_PTE_US;
      if (!(flags & PTE_RO)) x86_flags |= X86_PTE_RW;

      pd[PD_INDEX(v)] = (p & PTE_ADDR_MASK) | x86_flags;
      v += 0x200000;
      p += 0x200000;
    } else {
      if (arch_vmm_map(pgd, v, p, flags) != 0) return -1;
      v += 4096;
      p += 4096;
    }
  }
  arch_tlb_flush_all();
  return 0;
}
