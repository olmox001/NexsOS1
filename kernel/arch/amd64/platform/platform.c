/*
 * kernel/arch/amd64/platform/platform.c
 * Platform initialization for AMD64
 */
#include <arch/amd64_internal.h>
#include <arch/arch.h>
#include <kernel/arch.h>
#include <kernel/platform.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/cpu.h>
#include <kernel/hal.h>
#include <arch/amd64/apic.h>

extern void uart_init(void);
extern void pic_init(void);
extern void pit_init_hz(uint32_t hz);

/* Defined in multiboot2 header passed by boot.S */
extern uint64_t mb_info_ptr;

/* PVH Start Info */
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

/* Global memory regions for PMM */
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
  if (count) *count = arch_region_count;
  return arch_mem_regions;
}

/* Platform timer ticks (dummy for now) */
volatile uint64_t jiffies = 0;

extern uint64_t mb_magic;

#include <drivers/timer.h>
#include "../../../include/kernel/multiboot2.h"

void arch_platform_early_init(void) {
  uart_init();
  pr_info("AMD64 Platform Initialization (Magic: 0x%lx, Info: 0x%lx)\n", mb_magic, mb_info_ptr);

  pic_init();

  if (mb_info_ptr == 0) {
    pr_warn("Boot information missing! Attempting to continue with fallback.\n");
  }

  arch_region_count = 0;

  if (mb_magic == MB1_MAGIC) {
    /* Multiboot v1 */
    struct mb1_info *mb1 = (struct mb1_info *)mb_info_ptr;
    pr_info("Multiboot v1: Upper memory %u KB\n", mb1->mem_upper);

    if (mb1->flags & (1 << 6)) { /* MMAP available */
      uint32_t mmap_ptr = mb1->mmap_addr;
      uint32_t mmap_len = mb1->mmap_len;
      uint32_t processed = 0;
      
      while (processed < mmap_len && arch_region_count < 32) {
          uint32_t size = *(uint32_t *)(uintptr_t)mmap_ptr;
          struct mb2_mmap_entry *entry = (struct mb2_mmap_entry *)(uintptr_t)(mmap_ptr + 4);
          
          arch_mem_regions[arch_region_count].base = entry->addr;
          arch_mem_regions[arch_region_count].size = entry->len;
          arch_mem_regions[arch_region_count].type = (entry->type == 1) ? MEM_REGION_USABLE : MEM_REGION_RESERVED;
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
    /* Multiboot v2 */
    uint8_t *mb_data = (uint8_t *)mb_info_ptr;
    struct mb2_tag *tag = (struct mb2_tag *)(mb_data + 8);
    while (tag->type != MB2_TAG_TYPE_END && arch_region_count < 32) {
      if (tag->type == MB2_TAG_TYPE_MMAP) {
        struct mb2_tag_mmap *mmap = (struct mb2_tag_mmap *)tag;
        uint32_t entry_size = mmap->entry_size;
        uint32_t num_entries = (mmap->size - sizeof(struct mb2_tag_mmap)) / entry_size;
        for (uint32_t i = 0; i < num_entries && arch_region_count < 32; i++) {
          struct mb2_mmap_entry *entry = (struct mb2_mmap_entry *)((uint8_t *)mmap->entries + i * entry_size);
          arch_mem_regions[arch_region_count].base = entry->addr;
          arch_mem_regions[arch_region_count].size = entry->len;
          arch_mem_regions[arch_region_count].type = (entry->type == 1) ? MEM_REGION_USABLE : MEM_REGION_RESERVED;
          arch_region_count++;
        }
      }
      tag = (struct mb2_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
    }
  } else if (mb_magic == PVH_MAGIC) {
    /* PVH boot */
    struct hvm_start_info *pvh = (struct hvm_start_info *)mb_info_ptr;
    pr_info("PVH: Memmap at 0x%lx, entries: %u\n", pvh->memmap_paddr, pvh->memmap_entries);
    
    struct hvm_memmap_table_entry *entries = (struct hvm_memmap_table_entry *)pvh->memmap_paddr;
    for (uint32_t i = 0; i < pvh->memmap_entries && arch_region_count < 32; i++) {
        arch_mem_regions[arch_region_count].base = entries[i].addr;
        arch_mem_regions[arch_region_count].size = entries[i].len;
        arch_mem_regions[arch_region_count].type = (entries[i].type == 1) ? MEM_REGION_USABLE : MEM_REGION_RESERVED;
        arch_region_count++;
    }
  } else {
      pr_warn("AMD64: [IDTF] Unknown boot protocol (Magic: 0x%lx). Using safe 1GB default.\n", mb_magic);
      
      /* Safe default for QEMU/Basic systems */
      arch_mem_regions[0].base = 0;
      arch_mem_regions[0].size = 640 * 1024;
      arch_mem_regions[0].type = MEM_REGION_USABLE;
      arch_mem_regions[1].base = 0x100000;
      arch_mem_regions[1].size = (1024UL * 1024 * 1024) - 0x100000;
      arch_mem_regions[1].type = MEM_REGION_USABLE;
      arch_region_count = 2;
  }
}


uint64_t timer_get_us(void) {
  /* Stub: return TSC based pseudo-time or PIT ticks.
   * For now, just return jiffies * 1000 if we set up a 1ms timer. */
  return jiffies * 1000;
}

extern uint32_t ticks_per_ms;
void udelay(uint32_t us) {
  if (ticks_per_ms == 0) {
    /* Fallback if not calibrated yet: rough I/O port delay */
    for (uint32_t i = 0; i < us * 10; i++) {
      outb(0x80, 0);
    }
    return;
  }

  uint32_t ticks_to_wait = (ticks_per_ms * us) / 1000;
  if (ticks_to_wait == 0) ticks_to_wait = 1;

  /* Use LAPIC Timer Current Count for delay */
  /* Note: This assumes LAPIC timer is running or was at least started. */
  uint32_t start = lapic_read(0x0390); /* LAPIC_TCC */
  while (1) {
    uint32_t current = lapic_read(0x0390);
    uint32_t elapsed = (start > current) ? (start - current) : (0xFFFFFFFF - current + start);
    if (elapsed >= ticks_to_wait) break;
    arch_nop();
  }
}

void arch_pci_init(void) { /* Minimal stub */ }

extern char __kernel_stack[];
uint64_t secondary_ttbr0 = 0;

/* Secondary CPU boot support */
void arch_vmm_set_secondary_pgd(uint64_t pgd) { 
  secondary_ttbr0 = pgd;
}

/* Get kernel stack for a CPU */
extern char __kernel_stack[];
void *arch_get_kernel_stack(uint32_t cpu_id) {
  if (cpu_id >= MAX_CPUS) return NULL;
  /* Each CPU gets 128KB stack. We return the TOP of the stack (descending). */
  return (void *)&__kernel_stack[(uint64_t)(cpu_id + 1) * 131072];
}

/* Wake secondary CPU */
extern char trampoline_start[], trampoline_end[];
extern uint32_t trampoline_pml4;
extern uint64_t trampoline_stack, trampoline_entry;
extern uint64_t kernel_pgd_phys;
extern void secondary_cpu_entry(void);

int arch_cpu_wake_secondary(uint64_t cpu_id, void (*entry)(void), void *stack) {
  (void)entry;
  
  if (cpu_id >= MAX_CPUS) return -1;

  /* 1. Copy trampoline to base */
  uint8_t *dest = (uint8_t *)TRAMPOLINE_BASE;
  size_t size = trampoline_end - trampoline_start;
  memcpy(dest, trampoline_start, size);

  /* 2. Setup trampoline parameters */
  uint32_t *p_pml4 = (uint32_t *)(TRAMPOLINE_BASE + ((uintptr_t)&trampoline_pml4 - (uintptr_t)trampoline_start));
  uint64_t *p_stack = (uint64_t *)(TRAMPOLINE_BASE + ((uintptr_t)&trampoline_stack - (uintptr_t)trampoline_start));
  uint64_t *p_entry = (uint64_t *)(TRAMPOLINE_BASE + ((uintptr_t)&trampoline_entry - (uintptr_t)trampoline_start));

  *p_pml4 = (uint32_t)kernel_pgd_phys;
  *p_stack = (uint64_t)stack;
  *p_entry = (uint64_t)secondary_cpu_entry;

  /* 3. Send INIT IPI */
  lapic_send_ipi(cpu_id, ICR_INIT | ICR_ASSERT | ICR_LEVEL | ICR_PHYSICAL);
  
  /* 4. Wait 10ms */
  udelay(10000);

  /* 5. Send STARTUP IPI (Vector 0x01 -> 0x1000) */
  lapic_send_ipi(cpu_id, ICR_STARTUP | 0x01 | ICR_PHYSICAL);

  pr_info("AMD64: Sent INIT-SIPI to CPU %lu\n", cpu_id);
  return 0; 
}

/* Get boot information from Multiboot2 */
uint64_t arch_get_boot_info(void) { return (uint64_t)mb_info_ptr; }

extern volatile uint32_t cpu_boot_ack;
extern void secondary_cpu_entry(void);
extern void smp_create_idle_task(uint32_t cpu_id);

void arch_smp_init(void) {
    uint32_t bsp_id = (uint32_t)hal_cpu_id();
    pr_info("AMD64: Initializing SMP (BSP ID: %u)\n", bsp_id);

    /* For now, we attempt to wake up to 4 CPUs (matching QEMU config) */
    /* In a production kernel, we would parse ACPI MADT here. */
    for (uint32_t i = 0; i < 4; i++) {
        if (i == bsp_id) continue;

        cpu_boot_ack = 0;
        void *stack_bottom = arch_get_kernel_stack(i);
        void *stack_top = (void *)((uintptr_t)stack_bottom + 131072);
        
        if (arch_cpu_wake_secondary(i, secondary_cpu_entry, stack_top) == 0) {
            /* Wait for ACK with timeout */
            int timeout = 1000;
            while (cpu_boot_ack != i && timeout > 0) {
                udelay(1000);
                timeout--;
            }

            if (cpu_boot_ack == i) {
                pr_info("AMD64: CPU %u is online\n", i);
                smp_create_idle_task(i);
            } else {
                pr_warn("AMD64: CPU %u failed to ACK boot\n", i);
            }
        }
    }
}

