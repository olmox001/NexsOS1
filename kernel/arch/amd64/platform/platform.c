/*
 * kernel/arch/amd64/platform/platform.c
 * AMD64 Platform Initialization: Boot Protocol Parsing, Memory Discovery,
 * SMP Bringup, Timer Stubs, and PCI Init
 *
 * Responsibilities:
 *   - arch_platform_early_init: detect the boot protocol (Multiboot1,
 *     Multiboot2, PVH, or unknown fallback), parse the memory map, populate
 *     arch_mem_regions[] for the PMM, and initialise the legacy PIC.
 *   - arch_smp_init: detect CPU count via CPUID, copy the AP trampoline to
 *     TRAMPOLINE_BASE, send INIT-SIPI sequences, and wait for AP ACKs.
 *   - arch_pci_init / arch_get_boot_info / arch_get_kernel_stack /
 *     arch_vmm_set_secondary_pgd: supporting stubs and utilities.
 *   - timer_get_us / udelay: timer utilities (stub and LAPIC-based delay).
 *
 * Boot protocol support matrix:
 *   MB1_MAGIC (0x2BADB002): Multiboot v1 — parses MMAP flag (bit 6).
 *   MB2_MAGIC (0x36D76289): Multiboot v2 — walks tag chain for MMAP tag.
 *   PVH_MAGIC             : Parses hvm_start_info.memmap_paddr entries.
 *   Unknown               : Fallback to hardcoded 1 GB map.
 *
 * Known issues:
 *   BOOT-01 (W4 BUG/WRONG-DESIGN) On 'make run ARCH=amd64' QEMU boots via PVH
 *     but delivers the hvm_start_info pointer in %ebx with magic inside the
 *     struct's first field, not in a "magic register" value.  The code checks
 *     'mb_magic == PVH_MAGIC' expecting a saved register, but QEMU's PVH ABI
 *     sets mb_magic = the raw %eax (which is 0 or another value, not PVH_MAGIC).
 *     Result: the else branch fires and the 1 GB fallback is used.  [verified:
 *     serial shows "Magic: 0x0"]
 *   BOOT-02 (W4 BUG) Consequence of BOOT-01: hardcoded 1 GB RAM map at
 *     arch_platform_early_init:173-185.  The real RAM amount is ignored; '-m 4G'
 *     causes a crash downstream via DRV-VIRTIO-01.
 *   BOOT-03 (W2 REFINE) arch_cpu_wake_secondary sends INIT + one STARTUP IPI
 *     at :272-279.  Intel SDM recommends INIT + two SIPIs for reliability on
 *     real hardware.  QEMU accepts a single SIPI so no observed failure.
 *   BOOT-04 (W1 DOC/BAD-IMPL) 'secondary_ttbr0' and 'arch_vmm_set_secondary_pgd'
 *     at :224-228 use the AArch64 register name "ttbr0" for an x86 CR3 value —
 *     cross-architecture naming leak.
 *   ARCH-01 (W3 WRONG-DESIGN) amd64_count_cpus uses CPUID.01h EBX[23:16] which
 *     returns the max addressable APIC IDs (not online CPUs); on single-socket
 *     systems it may match, but on NUMA or hyperthreaded systems it over-counts.
 *     Should parse ACPI MADT for accurate online CPU discovery.
 *   ARCH-02 (W3 STUB) arch_pci_init is an empty stub at :222; amd64 has no
 *     ACPI table parsing; PCI enumeration relies solely on pci_enumerate/HAL.
 *   ARCH-03 (W2 STUB) timer_get_us returns jiffies*1000; 'jiffies' is a dummy
 *     counter incremented by the timer ISR and not a real microsecond source.
 */
#include <arch/amd64/apic.h>
#include <arch/amd64_internal.h>
#include <arch/arch.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/hal.h>
#include <kernel/platform.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/types.h>

extern void uart_init(void);
extern void pic_init(void);
extern void pit_init_hz(uint32_t hz);

/* mb_info_ptr: physical address of the Multiboot information structure.
 * Set in start.S _start_64 from %esi (which holds the %ebx value at boot). */
extern uint64_t mb_info_ptr;

/*
 * struct hvm_start_info - Xen/PVH boot handoff structure (XEN_ELFNOTE_PHYS32_ENTRY).
 *
 * On PVH boot, the firmware places a pointer to this struct in %ebx and stores
 * its identifying magic in hvm_start_info.magic (not in a CPU register).
 * NOTE(BOOT-01): platform.c checks mb_magic (a saved register value) against
 * PVH_MAGIC; on QEMU PVH boot, %eax (saved as mb_magic) is 0 — not PVH_MAGIC —
 * so this path is never reached and the 1 GB fallback fires instead.
 */
struct hvm_start_info {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t nr_modules;
  uint64_t modlist_paddr;
  uint64_t cmdline_paddr;
  uint64_t rsdp_paddr;
  uint64_t memmap_paddr;
  uint32_t memmap_entries;
  uint32_t reserved;
};

struct hvm_memmap_table_entry {
  uint64_t addr;
  uint64_t len;
  uint32_t type;
  uint32_t reserved;
};

#include <kernel/multiboot2.h>

struct mb1_info {
  uint32_t flags;
  uint32_t mem_lower;
  uint32_t mem_upper;
  uint32_t boot_device;
  uint32_t cmdline;
  uint32_t mods_count;
  uint32_t mods_addr;
  uint32_t syms[4];
  uint32_t mmap_len;
  uint32_t mmap_addr;
};

/* arch_mem_regions[]: PMM-visible memory map built from boot protocol data.
 * Holds up to 32 regions; populated by arch_platform_early_init and consumed
 * by arch_platform_get_mem_regions (called by vmm/pmm init code). */
static struct mem_region arch_mem_regions[32];
static size_t arch_region_count = 0;

/* Minimal Multiboot2 tags we care about */
#ifndef MB2_TAG_TYPE_END
#endif /* MB2_TAG_TYPE_END */
#ifndef MB2_TAG_TYPE_MMAP
#endif /* MB2_TAG_TYPE_MMAP */
#ifndef MB2_TAG_TYPE_BASIC_MEMINFO
#endif /* MB2_TAG_TYPE_BASIC_MEMINFO */

struct mem_region *arch_platform_get_mem_regions(size_t *count) {
  if (count)
    *count = arch_region_count;
  return arch_mem_regions;
}

/* jiffies: wall-clock tick counter incremented by the timer ISR (kernel_timer_tick).
 * NOTE(ARCH-03): Not a real microsecond source; timer_get_us() multiplies this
 * by 1000 to produce a coarse millisecond-granularity pseudo-timestamp. */
volatile uint64_t jiffies = 0;

extern uint64_t mb_magic;

#include "../../../include/kernel/multiboot2.h"
#include <drivers/timer.h>

/*
 * arch_platform_early_init - detect boot protocol and build the memory map.
 *
 * Called very early (before PMM, VMM, or SMP); only UART and PIC are
 * initialised here for output and IRQ masking.
 *
 * Protocol detection via mb_magic (saved from %eax in start.S _start_64):
 *   MB1_MAGIC (0x2BADB002): Multiboot 1 — parse mmap if flag bit 6 set,
 *     otherwise use mem_upper for two regions.
 *   MB2_MAGIC (0x36D76289): Multiboot 2 — walk tag chain, collect MMAP tags.
 *   PVH_MAGIC            : Parse hvm_start_info.memmap_paddr.
 *     NOTE(BOOT-01): PVH magic is in the struct, not in %eax; this branch is
 *     never reached on 'make run ARCH=amd64' (mb_magic == 0 from QEMU PVH).
 *   else (unknown/0)     : Fallback to hardcoded 640KB + (1GB - 1MB) regions.
 *     NOTE(BOOT-02): The 1 GB fallback is the path taken on 'make run' because
 *     BOOT-01 prevents PVH detection.  Real memory size is ignored.
 *
 * Side effects:
 *   - uart_init(), pic_init() called.
 *   - arch_mem_regions[] and arch_region_count populated.
 */
void arch_platform_early_init(void) {
  uart_init();
  pr_info("AMD64 Platform Initialization (Magic: 0x%lx, Info: 0x%lx)\n",
          mb_magic, mb_info_ptr);

  pic_init();

  if (mb_info_ptr == 0) {
    pr_warn(
        "Boot information missing! Attempting to continue with fallback.\n");
  }

  arch_region_count = 0;

  if (mb_magic == MB1_MAGIC) {
    /* Multiboot v1: %eax == 0x2BADB002 set by the MB1-compliant bootloader */
    struct mb1_info *mb1 = (struct mb1_info *)mb_info_ptr;
    pr_info("Multiboot v1: Upper memory %u KB\n", mb1->mem_upper);

    if (mb1->flags & (1 << 6)) { /* MMAP available (Multiboot1 flags bit 6) */
      uint32_t mmap_ptr = mb1->mmap_addr;
      uint32_t mmap_len = mb1->mmap_len;
      uint32_t processed = 0;

      while (processed < mmap_len && arch_region_count < 32) {
        uint32_t size = *(uint32_t *)(uintptr_t)mmap_ptr;
        struct mb2_mmap_entry *entry =
            (struct mb2_mmap_entry *)(uintptr_t)(mmap_ptr + 4);

        arch_mem_regions[arch_region_count].base = entry->addr;
        arch_mem_regions[arch_region_count].size = entry->len;
        arch_mem_regions[arch_region_count].type =
            (entry->type == 1) ? MEM_REGION_USABLE : MEM_REGION_RESERVED;
        arch_region_count++;

        mmap_ptr += size + 4;
        processed += size + 4;
      }
    } else {
      /* Fallback if no mmap */
      arch_mem_regions[0].base = 0;
      arch_mem_regions[0].size = 640 * 1024;
      arch_mem_regions[0].type = MEM_REGION_USABLE;
      arch_mem_regions[1].base = 0x100000;
      arch_mem_regions[1].size = (uint64_t)mb1->mem_upper * 1024;
      arch_mem_regions[1].type = MEM_REGION_USABLE;
      arch_region_count = 2;
    }
  } else if (mb_magic == MB2_MAGIC) {
    /* Multiboot v2: walk the tag list starting 8 bytes past the header.
     * Tags are 8-byte aligned; the loop advances by (tag->size + 7) & ~7. */
    uint8_t *mb_data = (uint8_t *)mb_info_ptr;
    struct mb2_tag *tag = (struct mb2_tag *)(mb_data + 8);
    while (tag->type != MB2_TAG_TYPE_END && arch_region_count < 32) {
      if (tag->type == MB2_TAG_TYPE_MMAP) {
        struct mb2_tag_mmap *mmap = (struct mb2_tag_mmap *)tag;
        uint32_t entry_size = mmap->entry_size;
        uint32_t num_entries =
            (mmap->size - sizeof(struct mb2_tag_mmap)) / entry_size;
        for (uint32_t i = 0; i < num_entries && arch_region_count < 32; i++) {
          struct mb2_mmap_entry *entry =
              (struct mb2_mmap_entry *)((uint8_t *)mmap->entries +
                                        i * entry_size);
          arch_mem_regions[arch_region_count].base = entry->addr;
          arch_mem_regions[arch_region_count].size = entry->len;
          arch_mem_regions[arch_region_count].type =
              (entry->type == 1) ? MEM_REGION_USABLE : MEM_REGION_RESERVED;
          arch_region_count++;
        }
      }
      tag = (struct mb2_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
    }
  } else if (mb_magic == PVH_MAGIC ||
             (mb_info_ptr &&
              ((struct hvm_start_info *)mb_info_ptr)->magic == PVH_MAGIC)) {
    /* FIX(BOOT-01/02): QEMU's PVH boot ('make run ARCH=amd64' via -kernel) sets
     * %eax (mb_magic) to 0 and passes the hvm_start_info pointer in %ebx
     * (mb_info_ptr); the identifying magic lives INSIDE the struct, not in a
     * register. Detect PVH by the struct magic so the REAL memory map (up to
     * 4GB) is parsed instead of the hardcoded 1GB fallback. */
    /* PVH boot: hvm_start_info at mb_info_ptr contains the E820/memmap.
     * NOTE(BOOT-01): On QEMU PVH boot mb_magic is 0, not PVH_MAGIC; this
     * branch is never reached.  mb_info_ptr would be the hvm_start_info PA
     * but only if start.S correctly saved %ebx — which it does via %esi.
     * The root cause of BOOT-01 is the magic check, not the pointer save. */
    struct hvm_start_info *pvh = (struct hvm_start_info *)mb_info_ptr;
    pr_info("PVH: Memmap at 0x%lx, entries: %u\n", pvh->memmap_paddr,
            pvh->memmap_entries);

    struct hvm_memmap_table_entry *entries =
        (struct hvm_memmap_table_entry *)pvh->memmap_paddr;
    for (uint32_t i = 0; i < pvh->memmap_entries && arch_region_count < 32;
         i++) {
      arch_mem_regions[arch_region_count].base = entries[i].addr;
      arch_mem_regions[arch_region_count].size = entries[i].len;
      arch_mem_regions[arch_region_count].type =
          (entries[i].type == 1) ? MEM_REGION_USABLE : MEM_REGION_RESERVED;
      arch_region_count++;
    }
  } else {
    /* NOTE(BOOT-02): This branch fires on 'make run ARCH=amd64' because
     * BOOT-01 prevents PVH detection.  mb_magic == 0 on that path. */
    pr_warn("AMD64: [IDTF] Unknown boot protocol (Magic: 0x%lx). Using safe "
            "1GB default.\n",
            mb_magic);

    /* Safe default for QEMU/Basic systems.
     * NOTE(BOOT-02): Hardcoded 1 GB ignores actual RAM size; '-m 4G' results
     * in a crash via DRV-VIRTIO-01 (PCI BAR above 4GB). */
    arch_mem_regions[0].base = 0;
    arch_mem_regions[0].size = 640 * 1024;
    arch_mem_regions[0].type = MEM_REGION_USABLE;
    arch_mem_regions[1].base = 0x100000;
    arch_mem_regions[1].size = (1024UL * 1024 * 1024) - 0x100000;
    arch_mem_regions[1].type = MEM_REGION_USABLE;
    arch_region_count = 2;
  }
}

/*
 * timer_get_us - return a pseudo-microsecond timestamp.
 *
 * NOTE(ARCH-03): This is a stub.  'jiffies' is a tick counter incremented by
 * kernel_timer_tick at HZ rate.  Multiplying by 1000 converts ticks to rough
 * millisecond values, not microseconds.  A real implementation would read the
 * TSC (with a calibrated ns/tick ratio) or the HPET.
 *
 * Returns: jiffies * 1000 (milliseconds, labelled as "microseconds").
 */
uint64_t timer_get_us(void) {
  /* NOTE(ARCH-03): jiffies*1000 is millisecond granularity, not microseconds */
  return jiffies * 1000;
}

/*
 * udelay - spin for approximately 'us' microseconds.
 *
 * If the LAPIC timer has been calibrated (ticks_per_ms != 0), reads LAPIC_TCC
 * (Timer Current Count, MMIO offset 0x390) to measure elapsed LAPIC ticks.
 * The elapsed calculation handles counter wrap-around (LAPIC counts down from
 * TIC to 0 and reloads for periodic mode).
 *
 * If ticks_per_ms == 0 (calibration not yet run), falls back to a busy-loop
 * writing to port 0x80 (a conventional POST-code delay port).
 *
 * Caution: assumes the LAPIC timer is in periodic mode and running.  Before
 * lapic_timer_calibrate() has been called (e.g. in early boot), only the port
 * 0x80 fallback is accurate.
 *
 * NOTE(ARCH-03): Microsecond accuracy depends on ticks_per_ms calibration;
 * accuracy is typically ±1 ms due to the PIT 10ms measurement window in
 * lapic_timer_calibrate.
 */
extern uint32_t ticks_per_ms;
void udelay(uint32_t us) {
  if (ticks_per_ms == 0) {
    /* Fallback: rough delay via port 0x80 writes (~1µs each on legacy systems) */
    for (uint32_t i = 0; i < us * 10; i++) {
      outb(0x80, 0);
    }
    return;
  }

  uint32_t ticks_to_wait = (ticks_per_ms * us) / 1000;
  if (ticks_to_wait == 0)
    ticks_to_wait = 1;

  /* Use LAPIC Timer Current Count (LAPIC_TCC, register offset 0x390) for delay.
   * LAPIC counts down; handles wrap-around when the timer reloads at zero. */
  uint32_t start = lapic_read(0x0390); /* LAPIC_TCC: current countdown value */
  while (1) {
    uint32_t current = lapic_read(0x0390);
    uint32_t elapsed =
        (start > current) ? (start - current) : (0xFFFFFFFF - current + start);
    if (elapsed >= ticks_to_wait)
      break;
    arch_nop();
  }
}

/*
 * arch_pci_init - AMD64 PCI controller initialization.
 *
 * NOTE(ARCH-02): Empty stub.  amd64 has no ACPI table parsing.  PCI
 * enumeration is performed later by arch_bus_scan (hal.c) via pci_enumerate,
 * which uses the legacy CF8/CFC configuration mechanism.  A complete
 * implementation would parse ACPI MCFG for PCIe MMCFG base addresses.
 */
void arch_pci_init(void) { /* Minimal stub — NOTE(ARCH-02): no ACPI parsing */ }

extern char __kernel_stack[];
/* secondary_ttbr0: the kernel PML4 physical address written for AP trampoline use.
 * NOTE(BOOT-04): Named 'ttbr0' (an AArch64 register) for an x86 CR3 value —
 * cross-arch naming leak; should be 'secondary_cr3' or 'secondary_pgd_phys'. */
uint64_t secondary_ttbr0 = 0;

/*
 * arch_vmm_set_secondary_pgd - record the PML4 PA for AP trampoline use.
 *
 * The trampoline reads trampoline_pml4 (patched in arch_cpu_wake_secondary)
 * directly; secondary_ttbr0 is a separate copy for callers that query the
 * current secondary PGD before waking the AP.
 *
 * NOTE(BOOT-04): Cross-arch naming; 'secondary_ttbr0' is confusingly AArch64.
 */
void arch_vmm_set_secondary_pgd(uint64_t pgd) { secondary_ttbr0 = pgd; }

/*
 * arch_get_kernel_stack - return the top of the kernel stack for cpu_id.
 *
 * __kernel_stack is a statically allocated BSS region of size 128KB * MAX_CPUS
 * (start.S: .skip 131072 * MAX_CPUS).  Each CPU gets 131072 bytes.
 * The stack grows downward, so the top of CPU N's slice is at:
 *   &__kernel_stack[(N+1) * 131072]
 *
 * This address is passed to trampoline_stack in arch_cpu_wake_secondary and
 * will become the AP's RSP when it starts executing C code after the trampoline.
 *
 * Returns NULL if cpu_id >= MAX_CPUS.
 */
extern char __kernel_stack[];
void *arch_get_kernel_stack(uint32_t cpu_id) {
  if (cpu_id >= MAX_CPUS)
    return NULL;
  /* Each CPU gets 128KB stack. We return the TOP of the stack (descending). */
  return (void *)&__kernel_stack[(uint64_t)(cpu_id + 1) * 131072];
}

/* External symbols for the AP trampoline code (trampoline.S) */
extern char trampoline_start[], trampoline_end[];
extern uint32_t trampoline_pml4;
extern uint64_t trampoline_stack, trampoline_entry;
extern uint64_t kernel_pgd_phys;
extern void secondary_cpu_entry(void);

/*
 * arch_cpu_wake_secondary - boot an AP from real mode via INIT-SIPI sequence.
 *
 * The AP starts executing in real mode at the 4KB-aligned physical address
 * given by the STARTUP IPI vector field (vector << 12 = 0x01 << 12 = 0x1000).
 * TRAMPOLINE_BASE must match this address.
 *
 * Steps:
 *   1. Copy the trampoline blob (trampoline.S) to TRAMPOLINE_BASE (0x1000).
 *      The trampoline is position-independent; it references its own data
 *      using base-relative addressing (e.g. trampoline_pml4 - trampoline_start + 0x1000).
 *   2. Patch the trampoline's PML4, stack, and entry-point fields at their
 *      relocated addresses inside the trampoline copy.
 *      - trampoline_pml4 (uint32_t): CR3 value for 64-bit mode.
 *      - trampoline_stack (uint64_t): RSP to load before calling C code.
 *      - trampoline_entry (uint64_t): the C entry point (secondary_cpu_entry).
 *   3. Send INIT IPI (edge-triggered, assert, physical destination).
 *   4. Wait 10 ms (Intel SDM requirement between INIT and first STARTUP).
 *   5. Send STARTUP IPI with vector 0x01 (page 0x1000).
 *      NOTE(BOOT-03): SDM recommends two SIPIs (the second after 200µs) for
 *      robust bringup on real hardware.  Only one SIPI is sent here; QEMU
 *      accepts a single SIPI so no observed failure in emulation.
 *
 * Params:
 *   cpu_id - destination LAPIC ID.
 *   entry  - ignored (entry point is hard-coded to secondary_cpu_entry).
 *   stack  - top of the AP kernel stack (as returned by arch_get_kernel_stack).
 *
 * NOTE(CPU-TRAMP-01): The trampoline GDT (trampoline.S:76-81) has only 4
 *   entries: null, 32-bit code, 32-bit data, 64-bit code.  After the far jump
 *   to 64-bit mode (ljmp $0x18), DS/ES/SS still hold 0x10 (the 32-bit data
 *   descriptor); no 64-bit data segment is present.  Some CPUs may enforce the
 *   D-bit on SS in 64-bit mode.  [inferred; not observed to crash on QEMU]
 *
 * Returns 0 on success, -1 if cpu_id is out of range.
 */
int arch_cpu_wake_secondary(uint64_t cpu_id, void (*entry)(void), void *stack) {
  (void)entry; /* hard-coded to secondary_cpu_entry in trampoline patch below */

  if (cpu_id >= MAX_CPUS)
    return -1;

  /* 1. Copy trampoline to TRAMPOLINE_BASE (physical address 0x1000).
   * The low 1 MB is identity-mapped (RW) by arch_vmm_init_hw. */
  uint8_t *dest = (uint8_t *)TRAMPOLINE_BASE;
  size_t size = trampoline_end - trampoline_start;
  memcpy(dest, trampoline_start, size);

  /* 2. Patch trampoline parameters at their relocated addresses.
   * Each parameter's address is (TRAMPOLINE_BASE + (symbol - trampoline_start))
   * because the copied code was originally linked at trampoline_start. */
  uint32_t *p_pml4 =
      (uint32_t *)(TRAMPOLINE_BASE +
                   ((uintptr_t)&trampoline_pml4 - (uintptr_t)trampoline_start));
  uint64_t *p_stack =
      (uint64_t *)(TRAMPOLINE_BASE + ((uintptr_t)&trampoline_stack -
                                      (uintptr_t)trampoline_start));
  uint64_t *p_entry =
      (uint64_t *)(TRAMPOLINE_BASE + ((uintptr_t)&trampoline_entry -
                                      (uintptr_t)trampoline_start));

  *p_pml4 = (uint32_t)kernel_pgd_phys;  /* CR3: kernel PML4 physical address */
  *p_stack = (uint64_t)stack;            /* RSP: top of AP kernel stack */
  *p_entry = (uint64_t)secondary_cpu_entry; /* call target after long mode */

  /* 3. Send INIT IPI: reset AP to 'wait-for-SIPI' state */
  lapic_send_ipi(cpu_id, ICR_INIT | ICR_ASSERT | ICR_LEVEL | ICR_PHYSICAL);

  /* 4. Wait 10 ms (Intel SDM: 10ms between INIT and SIPI for spec compliance) */
  udelay(10000);

  /* 5. Send STARTUP IPI: vector 0x01 → AP starts at physical 0x1000.
   * NOTE(BOOT-03): SDM recommends a second SIPI after 200µs; omitted here. */
  lapic_send_ipi(cpu_id, ICR_STARTUP | 0x01 | ICR_PHYSICAL);

  pr_info("AMD64: Sent INIT-SIPI to CPU %lu\n", cpu_id);
  return 0;
}

/* arch_get_boot_info - return the physical address of the boot info structure. */
uint64_t arch_get_boot_info(void) { return (uint64_t)mb_info_ptr; }

/*
 * amd64_count_cpus - estimate the number of logical CPUs via CPUID.
 *
 * Executes CPUID with EAX=01h and extracts EBX[23:16], which the Intel SDM
 * defines as "Maximum number of addressable IDs for logical processors in this
 * physical package."  On simple single-socket systems this equals the number
 * of logical cores.
 *
 * NOTE(ARCH-01): EBX[23:16] is the MAX APIC ID count, not necessarily the
 * number of *online* cores.  On hyperthreaded or NUMA systems this field can
 * be a power-of-two larger than the actual core count.  The correct approach
 * is to parse the ACPI MADT (Multiple APIC Description Table) which enumerates
 * exactly the enabled/online LAPIC entries.  This function may over-count.
 *
 * Returns: number of logical CPUs in range [1, MAX_CPUS].
 */
static uint32_t amd64_count_cpus(void) {
  uint32_t ebx;
  uint32_t cpu_count = 1; /* At least the BSP */

  /* CPUID EAX=01h: EBX[23:16] = maximum addressable APIC IDs in package.
   * NOTE(ARCH-01): Over-counts on HT/NUMA; should use ACPI MADT instead. */
  __asm__ volatile(
      "movl $1, %%eax\n\t"
      "cpuid\n\t"
      : "=b"(ebx)
      :
      : "eax", "ecx", "edx"
  );

  cpu_count = (ebx >> 16) & 0xFF;
  if (cpu_count == 0) cpu_count = 1;
  if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

  pr_info("AMD64: CPUID reports %u logical processors\n", cpu_count);
  return cpu_count;
}

extern void secondary_cpu_entry(void);

/*
 * arch_smp_init - bring application processors online one at a time.
 *
 * Called by the BSP after all per-CPU BSP init (GDT/IDT/LAPIC) is complete.
 * Iterates logical CPU IDs 1..(cpu_count-1) (skipping the BSP ID).
 *
 * For each AP: compute its kernel stack slice (arch_get_kernel_stack returns
 * the bottom; stack_top = bottom + 131072 for 128 KB per CPU), then hand off
 * to smp_bringup_secondary() (kernel/core/smp.c) — the shared arch-neutral
 * primitive owning the idle-task-first invariant (#169/#170), the INIT-SIPI
 * wake via arch_cpu_wake_secondary, and the acquire/release ack handshake
 * with a ~10 s wall-time timeout.  A timeout just skips the CPU (absent AP
 * or very slow hypervisor — its idle task already exists, so a late-arriving
 * AP still schedules safely).
 *
 * NOTE(ARCH-01): cpu_count from amd64_count_cpus may over-count; APs that
 * don't exist simply time out and emit a warning.
 * NOTE(BOOT-03): arch_cpu_wake_secondary sends only one SIPI; see BOOT-03.
 */
void arch_smp_init(void) {
  uint32_t bsp_id = (uint32_t)hal_cpu_id();
  pr_info("AMD64: Initializing SMP (BSP ID: %u)\n", bsp_id);

  /* Determine the number of CPUs to probe (like fdt_count_cpus() on aarch64).
   * NOTE(ARCH-01): CPUID-based; may over-count on HT/NUMA systems. */
  uint32_t cpu_count = amd64_count_cpus();
  if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

  pr_info("AMD64: Starting SMP initialization for %u potential cores\n", cpu_count);

  /* Iterate only up to detected CPU count */
  for (uint32_t i = 1; i < cpu_count; i++) {
    if (i == bsp_id)
      continue;

    /* Stack slice: 128KB per CPU (131072 bytes).  arch_get_kernel_stack returns
     * the bottom of the slice; add 131072 for the top (stack descends). */
    void *stack_bottom = arch_get_kernel_stack(i);
    void *stack_top = (void *)((uintptr_t)stack_bottom + 131072);

    /* Shared bring-up primitive (S-ALIGN F8, B6 slice): idle-task-first
     * (#169/#170), the INIT-SIPI wake and the ack handshake all live in
     * kernel/core/smp.c — the ordering fix is reasoned about ONCE, not once
     * per arch.  NOTE(BOOT-03): single SIPI only. */
    if (smp_bringup_secondary(i, secondary_cpu_entry, stack_top) == 0) {
      pr_info("AMD64: CPU %u is online\n", i);
    } else {
      /* Wake failed, or the AP did not respond in time (absent, or a very
       * slow hypervisor).  Its idle task already exists, so if it comes up
       * late it schedules safely. */
      pr_warn("AMD64: CPU %u failed to ACK boot\n", i);
    }
  }
}