/*
 * kernel/arch/amd64/mm/mmu.c
 * x86-64 Four-Level Paging (PML4 / PDPT / PD / PT) Implementation
 *
 * Responsibilities:
 *   - arch_vmm_init_hw: switch to the kernel PML4, direct-map all usable
 *     RAM regions reported by the platform, and map the MMIO window.
 *   - arch_vmm_map / arch_vmm_unmap / arch_vmm_get_physical: single-page
 *     4KB operations that walk/allocate/free PML4→PDPT→PD→PT chains.
 *   - arch_vmm_map_range: bulk mapping using 2MB large pages where alignment
 *     and size permit, falling back to 4KB pages.
 *   - arch_vmm_create_process_pgd: process address-space creation (PML4
 *     clone; teardown is the generic vmm_destroy_pgd in kernel/mm/vmm.c).
 *   - arch_vmm_protect: 4KB-precise attribute rewrite of existing mappings
 *     (splits large pages as needed; ends with an SMP TLB shootdown).
 *   - arch_vmm_map_mmio: maps the PCI/LAPIC/MMIO window 0xFE000000–
 *     0xFFFFFFFF at its direct-map VA + the low-2MB identity window for
 *     the SMP trampoline and the low-linked boot sections.
 *
 * Invariants (higher-half model, see kernel/memlayout.h):
 *   - Kernel VA = PA + KERNEL_VIRT_BASE (0xFFFF800000000000) for the image,
 *     all RAM and MMIO; PA<->pointer crossings go through
 *     phys_to_virt()/virt_to_phys().  The kernel half lives in PML4
 *     entries 256..511, shared with processes by value-copy at creation.
 *   - Boot PML4 (boot_pml4, identity + higher-half alias of PA 0..1GB):
 *     established by start.S; the kernel switches to the dynamic PML4 in
 *     arch_vmm_init_hw; APs still boot on boot_pml4 and adopt kernel_pgd
 *     in arch_cpu_init.
 *   - Intermediate page-table pages are allocated from pmm_alloc_page() and
 *     are never freed; the page-table tree only grows.
 *
 * Known issues:
 *   AMMU-01 RESOLVED (Phase B2, b745a74): x86_leaf_flags() translates the
 *     vmm.h profiles explicitly (opt-in RW, native PTE_NX honoured);
 *     vmm_map_ram_wx() maps text RX, rodata RO+NX, other RAM RW+NX.
 *   AMMU-02 RESOLVED (Phase B2): arch_vmm_protect is real — 4KB-precise
 *     attribute rewrite with large-page splitting and SMP TLB shootdown.
 *   AMMU-03 (FIXED) the divergent arch teardown was removed; the single
 *     path is vmm_destroy_pgd() in kernel/mm/vmm.c, which frees private
 *     table pages and PTE_USER frames (see its ownership rules).
 *   AMMU-04 (W2 BUG) get_next_table's large-page split code never runs in
 *     arch_vmm_map (which passes level=0 for the PML4→PDPT step, skipping the
 *     block-split branch); the vmm.c generic walker does not understand 2MB/1GB
 *     blocks at all — walking a region mapped by arch_vmm_map_range (2MB pages)
 *     via vmm.c's get_next_table dereferences the block entry as a table pointer.
 *   AMMU-05 (W2 SECURITY) All intermediate page-table entries (PDPT/PD/PT)
 *     are tagged X86_PTE_US (user-accessible), making them more permissive than
 *     necessary; only leaf PTEs for user pages need X86_PTE_US.
 *   AMMU-06 (W2 PERF) arch_vmm_map_mmio maps 0xFE000000–0xFFFFFFFF one 4KB
 *     page at a time (~8192 iterations) at every PGD setup.  A single 2MB page
 *     or a pre-computed PDPT entry would suffice.
 *   AMMU-07 (W2 PERF/REFINE) MMIO identity-mapped only for 0xFE000000–
 *     0xFFFFFFFF.  Devices with BAR addresses outside this window are unmapped.
 *   AMMU-08 RESOLVED (Phase B2): arch_vmm_unmap performs an IPI-based SMP
 *     shootdown (amd64_tlb_shootdown_va, kernel/arch/amd64/mm/tlb.c) so peer
 *     CPUs cannot keep using a cleared mapping.  arch_vmm_map keeps its local
 *     invlpg only: x86 TLBs do not cache not-present entries, so mapping a
 *     previously-absent VA needs no remote flush, and today's callers never
 *     live-remap a VA that another CPU is concurrently using.
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
/* ── Page Table Entry Flag Bits (Intel SDM Vol.3, Table 4-20) ──────────────
 * These apply to all four levels (PML4E, PDPTE, PDE, PTE) where relevant.
 * ─────────────────────────────────────────────────────────────────────────*/
#define X86_PTE_P   0x001         /* Present: entry is valid */
#define X86_PTE_RW  0x002         /* Read/Write: 0=read-only, 1=writable */
#define X86_PTE_US  0x004         /* User/Supervisor: 1=user-accessible */
#define X86_PTE_PWT 0x008         /* Page-level write-through cache policy */
#define X86_PTE_PCD 0x010         /* Page-level cache disable */
#define X86_PTE_A   0x020         /* Accessed: set by CPU on any access */
#define X86_PTE_D   0x040         /* Dirty: set by CPU on write */
#define X86_PTE_PAT 0x080         /* Page Attribute Table index bit */
#define X86_PTE_G   0x100         /* Global: don't flush from TLB on CR3 write */
#define X86_PTE_NX  (1ULL << 63)  /* No-Execute (requires IA32_EFER.NXE=1) */

/* PTE_ADDR_MASK: extract the 40-bit physical page-frame address from a PTE.
 * Bits [11:0] are flags; bits [63:52] are reserved/NX; bits [51:12] are PA. */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Virtual address index extractors for each paging level.
 * PML4: VA[47:39] (9 bits) → one of 512 PML4 entries → covers 512 GB each.
 * PDPT: VA[38:30] (9 bits) → one of 512 PDPT entries → covers 1 GB each.
 * PD:   VA[29:21] (9 bits) → one of 512 PD entries  → covers 2 MB each.
 * PT:   VA[20:12] (9 bits) → one of 512 PT entries  → covers 4 KB each. */
#define PML4_INDEX(a) (((a) >> 39) & 0x1FF)
#define PDPT_INDEX(a) (((a) >> 30) & 0x1FF)
#define PD_INDEX(a)   (((a) >> 21) & 0x1FF)
#define PT_INDEX(a)   (((a) >> 12) & 0x1FF)

int arch_vmm_map(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t flags);
int arch_vmm_unmap(uint64_t pgd, uint64_t va);
uint64_t arch_vmm_get_physical(uint64_t pgd, uint64_t va);

extern uint64_t boot_pml4[];

/*
 * arch_vmm_init_hw - switch to and fully populate the kernel PML4.
 *
 * Called once by the BSP after pmm_init() has populated the memory map.
 * Steps:
 *   1. Load kernel_pgd into CR3 (arch_vmm_set_pgd), replacing the minimal
 *      boot_pml4 set up by start.S.
 *   2. Identity-map the low 1 MB [0, 0x100000) for the SMP trampoline
 *      (TRAMPOLINE_BASE = 0x1000) and legacy BIOS data areas.
 *   3. Identity-map all usable RAM regions reported by arch_platform_get_mem_regions.
 *      arch_vmm_map_range uses 2MB large pages where VA/PA are 2MB-aligned
 *      for efficiency, then 4KB pages for the remainder.
 *   4. Identity-map the MMIO window via arch_vmm_map_mmio.
 *
 * NOTE(AMMU-01): Kernel RAM is mapped PTE_RW without PTE_NX because the NX
 * condition in arch_vmm_map only sets X86_PTE_NX when BOTH PTE_UXN and
 * PTE_PXN are set.  Neither flag is passed here, so kernel code pages are
 * mapped W+X.
 * NOTE(AMMU-08): After this switch all other CPUs (not yet started) will
 * have stale TLBs for the old boot_pml4; no SMP shootdown is needed at this
 * point since APs have not enabled paging yet.
 */
void arch_vmm_init_hw(uint64_t kernel_pgd) {
  /* Switch to the new kernel PML4 */
  arch_vmm_set_pgd(kernel_pgd);
  pr_info("AMD64 VMM: Switched to kernel PGD at %p\n", (void *)kernel_pgd);

  /* Register the TLB shootdown IPI (MM-VMM-05/AMMU-08).  Before this point
   * only the BSP runs, so local flushes were sufficient. */
  amd64_tlb_ipi_init();

  /* Pre-populate the kernel half's top-level slots (PML4 256..259, i.e.
   * direct-map VAs for PA 0..2TB) with empty PDPTs.  Process PML4s share
   * the kernel half by COPYING entries 256..511 at creation time; a
   * top-level entry created later (e.g. arch_vmm_map_device for a >512GB
   * BAR) would be invisible to already-created processes.  Pre-allocating
   * the PDPTs makes those slots shared pointers from day one. */
  uint64_t *pml4_va = (uint64_t *)phys_to_virt(kernel_pgd);
  for (int i = 256; i < 260; i++) {
    if (!(pml4_va[i] & X86_PTE_P)) {
      void *pdpt = pmm_alloc_page();
      if (!pdpt)
        break;
      memset(pdpt, 0, PAGE_SIZE);
      pml4_va[i] = virt_to_phys(pdpt) | X86_PTE_P | X86_PTE_RW;
    }
  }

  /* Map all detected RAM regions (the low-2MB identity window for the
   * SMP trampoline is part of arch_vmm_map_mmio, so EVERY kernel PGD —
   * this one and vmm_dynamic_remap's — gets it). */
  size_t count = 0;
  struct mem_region *regions = arch_platform_get_mem_regions(&count);

  for (size_t i = 0; i < count; i++) {
    if (regions[i].type == MEM_REGION_USABLE) {
      pr_info("AMD64 VMM: Mapping RAM 0x%lx - 0x%lx\n",
              regions[i].base, regions[i].base + regions[i].size);
      /* W^X section split (AMMU-01 resolved): text RX, rodata RO+NX,
       * the rest RW+NX.  kernel_pgd is a PA here; vmm_map_ram_wx takes
       * the pointer form. */
      vmm_map_ram_wx((uint64_t *)phys_to_virt(kernel_pgd), regions[i].base,
                     regions[i].size);
    }
  }

  /* Map MMIO regions (LAPIC, VirtIO, PCI) */
  arch_vmm_map_mmio((uint64_t *)phys_to_virt(kernel_pgd));
}

/*
 * arch_vmm_map_mmio - identity-map the 32-bit MMIO window into pgd.
 *
 * Maps 0xFE000000–0xFFFFFFFF (not including 0xFFFFFFFF itself) as uncached,
 * write-through, present, R/W pages.  This range covers:
 *   0xFEE00000 : LAPIC base (LAPIC_DEFAULT_BASE)
 *   0xFEC00000 : I/O APIC base (if present)
 *   0xFE000000+ : PCI MMIO BARs that QEMU places in the 3–4 GB hole
 *
 * NOTE(AMMU-06): ~8192 individual 4KB arch_vmm_map calls are issued per PGD
 * setup.  This is inefficient; a single 2MB large page per 2MB-aligned chunk
 * (or pre-populating the PDPT entry for 0xFE000000–0xFFFFFFF) would be faster.
 * NOTE(AMMU-07): Devices with BAR addresses outside 0xFE000000–0xFFFFFFFF
 * are not mapped here and will fault on access.  Notably, at '-m 4G' QEMU
 * places virtio-blk BARs above 4 GB — those are outside this window and also
 * above 32-bit PA so pci_get_bar can't address them (DRV-VIRTIO-01/DRV-PCI-02).
 *
 * PTE flags: P|RW|PCD|PWT — cache-disabled, write-through for MMIO registers.
 */
void arch_vmm_map_mmio(uint64_t *pgd) {
  /* Map PCI MMIO and System MMIO (0xFE000000 to 0xFFFFFFFF) at their
   * direct-map VAs (phys_to_virt).
   * Covers PCI devices, LAPIC, IOAPIC, and upper BIOS ranges.
   * NOTE(AMMU-06): ~8192 individual 4KB map calls per PGD — inefficient. */
  for (uint64_t addr = 0xFE000000UL; addr < 0xFFFFFFFFUL; addr += 4096) {
    arch_vmm_map(virt_to_phys(pgd), (uint64_t)phys_to_virt(addr), addr,
                 PAGE_DEVICE);
  }

  /* Identity-map the low 2MB into every KERNEL PGD (this function is
   * called for the phase-1 PGD and for vmm_dynamic_remap's, never for
   * process PGDs):
   *  - [0, 1MB): SMP trampoline target (TRAMPOLINE_BASE 0x1000 must be
   *    writable: arch_cpu_wake_secondary copies and patches it there) and
   *    legacy BIOS data areas.
   *  - [1MB, 2MB): the low-linked boot sections (kernel.ld) — platform.c
   *    reads the trampoline blob via its low link address.
   * NX is safe: APs execute the trampoline in real mode (paging off) and
   * then on boot_pml4, never through these kernel_pgd entries. */
  arch_vmm_map_range(virt_to_phys(pgd), 0, 0, 0x200000, PAGE_KERNEL);
}

/*
 * arch_vmm_map_device - identity-map a device MMIO region [base, base+size)
 * into the active kernel page tables as uncached device memory (PCD|PWT).
 *
 * FIX(DRV-VIRTIO-01 / AMMU-07): called by the amd64 PCI scan for a device's
 * BAR so the region is reachable even when QEMU places a 64-bit BAR ABOVE 4 GB
 * (e.g. with '-m 4G'), which the fixed 0xFE000000-0xFFFFFFFF window misses.
 * Identity (VA==PA), consistent with the rest of the amd64 MMU model.
 */
int arch_vmm_map_device(uint64_t base, uint64_t size);
int arch_vmm_map_device(uint64_t base, uint64_t size) {
  extern uint64_t *kernel_pgd;
  if (!base || !size)
    return -1;
  uint64_t start = base & ~0xFFFUL;
  uint64_t end = (base + size + 0xFFFUL) & ~0xFFFUL;
  for (uint64_t a = start; a < end; a += 4096) {
    arch_vmm_map(virt_to_phys(kernel_pgd), (uint64_t)phys_to_virt(a), a,
                 PAGE_DEVICE);
  }
  arch_tlb_flush_all();
  return 0;
}

/*
 * arch_vmm_init - early VMM initialisation (boot path).
 *
 * Called before arch_vmm_init_hw.  At this point boot_pml4 (set up by start.S)
 * is already loaded in CR3 with a 1 GB identity map.  This function only logs
 * the boot PML4 address; the full map is built in arch_vmm_init_hw.
 */
void arch_vmm_init(void) {
  /* Boot PML4 is already set up with identity map by boot.S */
  pr_info("AMD64 VMM initialized (PML4 @ %p)\n", (void *)boot_pml4);
}

/*
 * get_next_table - walk or allocate the next-level page table.
 *
 * Params:
 *   table  - current-level page table (PML4, PDPT, or PD)
 *   index  - entry index within 'table' (0-511)
 *   alloc  - 1 = allocate a new page if the entry is absent
 *   level  - 0=PML4→PDPT, 1=PDPT→PD (1GB blocks), 2=PD→PT (2MB blocks)
 *
 * Returns pointer to the next-level table page, or NULL on absent/OOM.
 *
 * Large-page / block-entry handling:
 *   If the entry has the PS bit (bit 7, 0x080) set and alloc==1, the block
 *   is split: a new page table is allocated and filled with 512 entries that
 *   each cover one sub-region.
 *     level==1: 1GB block → 512×2MB entries (each with PS bit preserved)
 *     level==2: 2MB block → 512×4KB entries (PS bit cleared for leaf PTEs)
 *
 * NOTE(AMMU-05): New intermediate table entries always include X86_PTE_US
 * (user-accessible), which is more permissive than necessary for kernel-only
 * intermediate nodes.
 *
 * NOTE(AMMU-04): arch_vmm_map calls get_next_table with level=0, 1, 2 for
 * the PML4→PDPT, PDPT→PD, and PD→PT steps respectively.  The block-split
 * code for level==1/2 is only exercised when walking a range that was
 * previously mapped as a large page — which requires the caller to know that
 * situation exists.  vmm.c's generic get_next_table does not understand large
 * pages at all: it will dereference a 2MB-block PDE as a table pointer and
 * corrupt memory.  [inferred; cross-ref AMMU-04]
 */
static uint64_t *get_next_table(uint64_t *table, uint64_t index, int alloc, int level) {
  uint64_t entry = table[index];

  if (entry & X86_PTE_P) {
    /* Check for 2MB/1GB block (PS bit = bit 7) */
    if (entry & 0x080) {
      if (!alloc) return NULL;

      /* SPLIT BLOCK: allocate a new sub-table and fill it with fine-grained entries */
      void *new_table = pmm_alloc_page();
      if (!new_table) return NULL;
      memset(new_table, 0, PAGE_SIZE);

      uint64_t block_pa = entry & PTE_ADDR_MASK;
      uint64_t block_flags = entry & ~PTE_ADDR_MASK;
      /* Remove PS bit and ensure table bits are correct */
      uint64_t *sub_table = (uint64_t *)new_table;
      if (level == 1) {
        /* 1GB Block → 512 × 2MB blocks (preserve PS bit in sub-entries) */
        for (int i = 0; i < 512; i++) {
          sub_table[i] = (block_pa + (uint64_t)i * 0x200000) | block_flags;
        }
      } else if (level == 2) {
        /* 2MB Block → 512 × 4KB pages (clear PS bit: leaf PTEs have no PS) */
        for (int i = 0; i < 512; i++) {
          sub_table[i] = (block_pa + (uint64_t)i * 4096) | (block_flags & ~0x080);
        }
      }

      /* Replace the block entry with a pointer to the new sub-table.
       * NOTE(AMMU-05): X86_PTE_US on an intermediate entry; kernel-only
       * intermediate tables do not need to be user-accessible. */
      table[index] = virt_to_phys(new_table) | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
      return (uint64_t *)new_table;
    }
    /* Entry is present and not a large page: return the next-level table PA.
     * Identity-map invariant: PA == VA, so the cast is safe. */
    return (uint64_t *)phys_to_virt(entry & PTE_ADDR_MASK);
  }

  if (!alloc) return NULL;

  /* Entry is absent: allocate a new page table page */
  void *page = pmm_alloc_page();
  if (!page) return NULL;
  memset(page, 0, PAGE_SIZE);

  /* NOTE(AMMU-05): X86_PTE_US on intermediate table entry */
  table[index] = virt_to_phys(page) | X86_PTE_P | X86_PTE_RW | X86_PTE_US;
  return (uint64_t *)page;
}

/*
 * arch_vmm_map - map a single 4KB virtual page to a physical page.
 *
 * Params:
 *   pgd   - physical address of the PML4 (== virtual due to identity map)
 *   va    - virtual address to map (4KB-aligned)
 *   pa    - physical address to map to (4KB-aligned)
 *   flags - arch-neutral PTE_* flags (PTE_USER, PTE_RO, PTE_UXN, PTE_PXN,
 *           PTE_ATTR_DEVICE) converted to x86 PTE bits here
 *
 * Flag translation:
 *   PTE_USER  → X86_PTE_US (user-accessible leaf PTE)
 *   !PTE_RO   → X86_PTE_RW (writable; read-only if PTE_RO set)
 *   PTE_UXN && PTE_PXN → X86_PTE_NX (no-execute)
 *     NOTE(AMMU-01): NX requires BOTH PTE_UXN and PTE_PXN to be set.
 *     Kernel mappings pass PTE_RW only → NX is never set → W+X pages.
 *   PTE_ATTR_DEVICE → X86_PTE_PCD | X86_PTE_PWT (uncached MMIO)
 *
 * TLB invalidation: if the pgd being modified is the active CR3 for this CPU,
 * invlpg flushes the single VA's TLB entry.  If modifying a different PGD
 * (e.g. a new process), no flush is issued for this CPU — correct, since the
 * modified PGD is not loaded.
 * No IPI is sent on map: x86 TLBs do not cache not-present entries, so a
 * fresh mapping cannot be stale anywhere; live remaps do not exist in
 * today's callers (see AMMU-08 note in the file header).
 *
 * Returns 0 on success, -1 if any page-table page allocation fails.
 */
/*
 * x86_leaf_flags - translate arch-neutral PTE/PAGE profile bits to x86 PTE bits.
 *
 * Flag translation (AMMU-01 resolved): callers pass the amd64 PTE/PAGE
 * profiles from vmm.h, which carry the final x86 bits — translate them
 * explicitly.  Writability is OPT-IN (PTE_RW present), so read-only user
 * segments and kernel text are actually read-only (CR0.WP is set).  NX is
 * honoured both natively (PTE_NX) and from the arch-neutral UXN+PXN
 * encoding used by shared code.  Shared by arch_vmm_map, the 2MB path of
 * arch_vmm_map_range, and arch_vmm_protect.
 */
static uint64_t x86_leaf_flags(uint64_t flags) {
  uint64_t x86_flags = X86_PTE_P;
  if (flags & PTE_USER)
    x86_flags |= X86_PTE_US;
  if (flags & PTE_RW)
    x86_flags |= X86_PTE_RW;
  if (flags & PTE_PWT)
    x86_flags |= X86_PTE_PWT;
  if (flags & PTE_PCD)
    x86_flags |= X86_PTE_PCD;
  if ((flags & PTE_NX) || ((flags & PTE_UXN) && (flags & PTE_PXN)))
    x86_flags |= X86_PTE_NX;
  return x86_flags;
}

int arch_vmm_map(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t flags) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pgd);
  uint64_t x86_flags = x86_leaf_flags(flags);

  /* Walk (and allocate if absent) PML4 → PDPT → PD → PT */
  uint64_t *pdpt = get_next_table(pml4, PML4_INDEX(va), 1, 0);
  if (!pdpt) return -1;

  uint64_t *pd = get_next_table(pdpt, PDPT_INDEX(va), 1, 1);
  if (!pd) return -1;

  uint64_t *pt = get_next_table(pd, PD_INDEX(va), 1, 2);
  if (!pt) return -1;

  /* Write the leaf PTE: physical address + flags */
  pt[PT_INDEX(va)] = (pa & PTE_ADDR_MASK) | x86_flags;

  /* Optimized TLB flush: only if we are modifying the ACTIVE address space.
   * Local-only by design — fresh mappings are never cached remotely. */
  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  if ((pgd & PTE_ADDR_MASK) == (current_cr3 & PTE_ADDR_MASK)) {
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
  }

  return 0;
}

/*
 * arch_vmm_unmap - clear the leaf PTE for a single 4KB virtual page.
 *
 * Walks PML4→PDPT→PD→PT without allocating; if any level is absent (not
 * present), returns 0 silently (idempotent).  On success, zeroes the leaf
 * PTE and performs an SMP TLB shootdown for the VA (AMMU-08 resolved): when
 * this returns, no online CPU still translates 'va' through the old entry,
 * so the caller may safely recycle the backing frame.
 *
 * NOTE(AMMU-03): Does not free the physical frame that was backing 'va'.
 *   Frame lifecycle is tracked in process.c; this function only clears the
 *   page-table entry.
 *
 * Returns 0 always (no failure path for missing entries).
 */
int arch_vmm_unmap(uint64_t pgd, uint64_t va) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pgd);

  if (!(pml4[PML4_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(va)] & PTE_ADDR_MASK);

  if (!(pdpt[PDPT_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(va)] & PTE_ADDR_MASK);

  if (!(pd[PD_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(va)] & PTE_ADDR_MASK);

  pt[PT_INDEX(va)] = 0;
  amd64_tlb_shootdown_va(va); /* local invlpg + IPI to online peers */
  return 0;
}

/*
 * arch_vmm_get_physical - translate a virtual address to its physical address.
 *
 * Walks PML4→PDPT→PD→PT without allocation; returns 0 if any level is not
 * present.  On success returns (leaf_PTE & PTE_ADDR_MASK) | (va & 0xFFF),
 * which is the physical byte address corresponding to 'va'.
 *
 * Handles 1GB (PDPTE.PS) and 2MB (PDE.PS) large pages by combining the
 * block base with the in-block offset of 'va' — previously these were
 * misread as table pointers and the lookup returned 0/garbage for any
 * address inside the 2MB-mapped RAM identity window.
 */
uint64_t arch_vmm_get_physical(uint64_t pgd, uint64_t va) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pgd);

  if (!(pml4[PML4_INDEX(va)] & X86_PTE_P))
    return 0;
  uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[PML4_INDEX(va)] & PTE_ADDR_MASK);

  uint64_t pdpte = pdpt[PDPT_INDEX(va)];
  if (!(pdpte & X86_PTE_P))
    return 0;
  if (pdpte & 0x080) /* 1GB page */
    return ((pdpte & PTE_ADDR_MASK) & ~0x3FFFFFFFULL) | (va & 0x3FFFFFFFULL);
  uint64_t *pd = (uint64_t *)phys_to_virt(pdpte & PTE_ADDR_MASK);

  uint64_t pde = pd[PD_INDEX(va)];
  if (!(pde & X86_PTE_P))
    return 0;
  if (pde & 0x080) /* 2MB page */
    return ((pde & PTE_ADDR_MASK) & ~0x1FFFFFULL) | (va & 0x1FFFFFULL);
  uint64_t *pt = (uint64_t *)phys_to_virt(pde & PTE_ADDR_MASK);

  if (!(pt[PT_INDEX(va)] & X86_PTE_P))
    return 0;

  return (pt[PT_INDEX(va)] & PTE_ADDR_MASK) | (va & 0xFFF);
}

uint64_t arch_vmm_create_process_pgd(void);

/*
 * arch_vmm_create_process_pgd - allocate a PML4 for a new user process.
 *
 * Strategy (higher-half model):
 *   1. Allocate a fresh PML4 page, zeroed.
 *   2. Copy the kernel-half entries (PML4 indices 256-511) from kernel_pgd
 *      by value.  arch_vmm_init_hw pre-populates every slot the kernel can
 *      ever need (256..259), so the copies always point at SHARED PDPTs and
 *      later kernel mappings are visible to all processes automatically.
 *   3. The user half (indices 0..255) starts EMPTY; the ELF loader and
 *      sbrk populate it on demand via arch_vmm_map().
 *
 * Teardown: the generic vmm_destroy_pgd() (kernel/mm/vmm.c) walks the
 * private index-0 subtree built on demand and reclaims table pages + user
 * frames; the copied kernel-half entries are never descended.
 *
 * Returns the physical address of the new PML4, or 0 on allocation failure.
 */
uint64_t arch_vmm_create_process_pgd(void) {
  uint64_t *new_pml4 = (uint64_t *)pmm_alloc_page();
  if (!new_pml4)
    return 0;

  memset(new_pml4, 0, PAGE_SIZE);

  /*
   * Higher-half model (memlayout.h):
   *   PML4 index 0..255   user half — starts EMPTY; the ELF loader / sbrk
   *                       populate it via arch_vmm_map().
   *   PML4 index 256..511 kernel half — shared by copying the kernel_pgd
   *                       entries by value.  Every kernel top-level slot
   *                       that can ever be needed (direct map of RAM and
   *                       device BARs: 256..259, pre-populated by
   *                       arch_vmm_init_hw) already exists, so later
   *                       kernel mappings land in SHARED PDPTs and are
   *                       visible to every process automatically.
   * Nothing else is cloned: no low identity, no per-process MMIO map.
   */
  extern uint64_t *kernel_pgd;
  for (int i = 256; i < 512; i++) {
    new_pml4[i] = kernel_pgd[i];
  }

  /* Contract: return the PGD's PHYSICAL address (vmm_create_pgd converts). */
  return virt_to_phys(new_pml4);
}

/* AMMU-03 resolved: the divergent arch teardown (which freed only the PML4
 * page and leaked the private PDPT + all PD/PT pages and user frames) has
 * been removed — it had no callers.  The single teardown path is the generic
 * vmm_destroy_pgd() (kernel/mm/vmm.c), which walks the private index-0
 * subtree, frees process-private table pages and PTE_USER leaf frames, and
 * skips entries shared by value with kernel_pgd. */

/*
 * arch_vmm_protect - rewrite the attributes of existing 4KB mappings (AMMU-02
 * resolved).
 *
 * Params:
 *   pgd       physical address of the PML4.
 *   va, size  range to change; rounded outward to page boundaries.
 *   flags     arch-neutral PTE/PAGE profile (same vocabulary as
 *             arch_vmm_map); every attribute bit of each leaf PTE is
 *             replaced via x86_leaf_flags(), only the frame address is kept.
 *
 * A 1GB/2MB large page covering part of the range is first split into the
 * next finer granularity (get_next_table with alloc=1, which preserves the
 * translations bit-identically) so the change applies with 4KB precision.
 *
 * Returns 0 on success; -1 if any page in the range is unmapped or a split
 * allocation fails (pages BEFORE the failure keep the new attributes).
 *
 * TLB: one SMP shootdown round (full flush, all online CPUs) after the
 * loop — runs on both success and failure paths so rewritten PTEs are never
 * masked by stale TLB copies anywhere.
 */
int arch_vmm_protect(uint64_t pgd, uint64_t va, uint64_t size, uint64_t flags);
int arch_vmm_protect(uint64_t pgd, uint64_t va, uint64_t size, uint64_t flags) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pgd);
  uint64_t x86_flags = x86_leaf_flags(flags);
  uint64_t v = va & ~0xFFFUL;
  uint64_t end = (va + size + 0xFFFUL) & ~0xFFFUL;
  int rc = 0;

  for (; v < end; v += 4096) {
    uint64_t pml4e = pml4[PML4_INDEX(v)];
    if (!(pml4e & X86_PTE_P)) { rc = -1; break; }
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4e & PTE_ADDR_MASK);

    uint64_t pdpte = pdpt[PDPT_INDEX(v)];
    if (!(pdpte & X86_PTE_P)) { rc = -1; break; }
    uint64_t *pd;
    if (pdpte & 0x080) {
      pd = get_next_table(pdpt, PDPT_INDEX(v), 1, 1); /* split 1GB page */
      if (!pd) { rc = -1; break; }
    } else {
      pd = (uint64_t *)phys_to_virt(pdpte & PTE_ADDR_MASK);
    }

    uint64_t pde = pd[PD_INDEX(v)];
    if (!(pde & X86_PTE_P)) { rc = -1; break; }
    uint64_t *pt;
    if (pde & 0x080) {
      pt = get_next_table(pd, PD_INDEX(v), 1, 2); /* split 2MB page */
      if (!pt) { rc = -1; break; }
    } else {
      pt = (uint64_t *)phys_to_virt(pde & PTE_ADDR_MASK);
    }

    uint64_t pte = pt[PT_INDEX(v)];
    if (!(pte & X86_PTE_P)) { rc = -1; break; }
    pt[PT_INDEX(v)] = (pte & PTE_ADDR_MASK) | x86_flags;
  }

  amd64_tlb_shootdown_all();
  return rc;
}

/*
 * arch_vmm_map_range - map a contiguous VA→PA range, using 2MB pages where possible.
 *
 * Iterates [va, va+size) and maps each chunk:
 *   - If both VA and PA are 2MB-aligned AND remaining size >= 2MB: map a single
 *     2MB large page in the PD (PS bit = 0x080, level 2 in PD) without a PT.
 *   - Otherwise: falls through to arch_vmm_map for 4KB granularity.
 *
 * After the loop, arch_tlb_flush_all() (write to CR3) flushes the entire
 * local TLB.  Local-only is correct here: map_range is used at boot (before
 * SMP) and for fresh ranges, never to live-remap pages another CPU uses.
 *
 * The 2MB path builds the PD entry with PS|P|RW (and optionally US) but does
 * NOT set NX, because PTE_UXN/PTE_PXN flags are not in the arch_vmm_map_range
 * callers' flag set.  NOTE(AMMU-01): same W+X issue as arch_vmm_map.
 *
 * Returns 0 on success, -1 if any allocation fails.
 */
int arch_vmm_map_range(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t size, uint64_t flags) {
  uint64_t v = va;
  uint64_t p = pa;
  uint64_t end = va + size;

  while (v < end) {
    uint64_t remaining = end - v;
    
    if ((v & 0x1FFFFF) == 0 && (p & 0x1FFFFF) == 0 && remaining >= 0x200000) {
      uint64_t *pml4 = (uint64_t *)phys_to_virt(pgd);
      uint64_t *pdpt = get_next_table(pml4, PML4_INDEX(v), 1, 0);
      if (!pdpt) return -1;

      uint64_t *pd = get_next_table(pdpt, PDPT_INDEX(v), 1, 1);
      if (!pd) return -1;

      /* Level 2 2MB Page Mapping — same explicit translation as
       * arch_vmm_map (opt-in RW, NX from PTE_NX or UXN+PXN). */
      uint64_t x86_flags = x86_leaf_flags(flags) | 0x080; /* PS bit for 2MB */

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
