/*
 * kernel/arch/amd64/platform/platform.c
 * Platform initialization for AMD64
 */
#include <kernel/types.h>
#include <kernel/printk.h>
#include <kernel/pmm.h>
#include <kernel/platform.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>

extern void uart_init(void);
extern void pic_init(void);
extern void pit_init(void);

/* Defined in multiboot2 header passed by boot.S */
extern uint32_t mb_info_ptr;

/* Minimal Multiboot2 tags we care about */
#define MB2_TAG_TYPE_END         0
#define MB2_TAG_TYPE_MMAP        6
#define MB2_TAG_TYPE_BASIC_MEMINFO 4

struct mb2_tag {
  uint32_t type;
  uint32_t size;
};

struct mb2_mmap_entry {
  uint64_t addr;
  uint64_t len;
  uint32_t type;
  uint32_t zero;
};

struct mb2_tag_mmap {
  uint32_t type;
  uint32_t size;
  uint32_t entry_size;
  uint32_t entry_version;
  struct mb2_mmap_entry entries[];
};

struct mb2_tag_basic_meminfo {
  uint32_t type;
  uint32_t size;
  uint32_t mem_lower;
  uint32_t mem_upper;
};

/* Platform timer ticks (dummy for now) */
volatile uint64_t jiffies = 0;

void arch_platform_early_init(void) {
  uart_init();
  pr_info("AMD64 Platform Initialization\n");

  pic_init();
  pit_init();
  __arch_local_irq_enable();

  if (mb_info_ptr == 0) {
    pr_err("Multiboot2 info missing!\n");
    __arch_cpu_halt();
  }

  uint8_t *mb_data = (uint8_t *)(uint64_t)mb_info_ptr;
  uint32_t total_size = *(uint32_t *)mb_data;
  pr_info("Multiboot2 structure size: %u bytes\n", total_size);

  /* PMM limits */
  uint64_t max_ram = 0;

  /* Parse tags */
  struct mb2_tag *tag = (struct mb2_tag *)(mb_data + 8);
  while (tag->type != MB2_TAG_TYPE_END) {
    if (tag->type == MB2_TAG_TYPE_BASIC_MEMINFO) {
      struct mb2_tag_basic_meminfo *mem = (struct mb2_tag_basic_meminfo *)tag;
      pr_info("Lower Memory: %u KB, Upper Memory: %u KB\n", mem->mem_lower, mem->mem_upper);
    } 
    else if (tag->type == MB2_TAG_TYPE_MMAP) {
      struct mb2_tag_mmap *mmap = (struct mb2_tag_mmap *)tag;
      uint32_t entry_size = mmap->entry_size;
      uint32_t num_entries = (mmap->size - sizeof(struct mb2_tag_mmap)) / entry_size;
      
      for (uint32_t i = 0; i < num_entries; i++) {
        struct mb2_mmap_entry *entry = (struct mb2_mmap_entry *)((uint8_t *)mmap->entries + i * entry_size);
        pr_info("MMAP: Base 0x%lx, Length 0x%lx, Type %u\n", entry->addr, entry->len, entry->type);
        
        /* Type 1 is available RAM */
        if (entry->type == 1) {
          uint64_t end = entry->addr + entry->len;
          if (end > max_ram) max_ram = end;
        }
      }
    }
    
    /* Align to 8 bytes */
    tag = (struct mb2_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
  }

  pr_info("Detected Max RAM: %lu MB\n", max_ram / 1024 / 1024);

  /* Set up PMM region. Kernel uses 0-1MB for boot logic, 1MB-? for kernel code/data.
   * Assuming 1GB max for baremetal tests, we start giving out memory from 16MB. */
  if (max_ram > 0) {
    uint64_t pmm_start = 0x1000000; /* 16 MB */
    if (max_ram > pmm_start) {
      pmm_init_region(pmm_start, max_ram - pmm_start);
    }
  }
}

uint64_t timer_get_us(void) {
  /* Stub: return TSC based pseudo-time or PIT ticks.
   * For now, just return jiffies * 1000 if we set up a 1ms timer. */
  return jiffies * 1000;
}

void udelay(uint32_t us) {
  /* Stub */
  uint64_t end = timer_get_us() + us;
  while (timer_get_us() < end) {
    __arch_yield();
  }
}

void arch_pci_init(void) {
  /* Minimal stub */
}
