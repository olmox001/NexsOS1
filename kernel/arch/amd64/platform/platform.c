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
#include <kernel/types.h>

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
#define MB2_TAG_TYPE_END 0
#define MB2_TAG_TYPE_MMAP 6
#define MB2_TAG_TYPE_BASIC_MEMINFO 4

struct mem_region *arch_platform_get_mem_regions(size_t *count) {
  if (count) *count = arch_region_count;
  return arch_mem_regions;
}

/* Platform timer ticks (dummy for now) */
volatile uint64_t jiffies = 0;

extern uint64_t mb_magic;

#include <drivers/timer.h>

void arch_platform_early_init(void) {
  uart_init();
  pr_info("AMD64 Platform Initialization (Magic: 0x%lx, Info: 0x%lx)\n", mb_magic, mb_info_ptr);

  pic_init();
  pit_init_hz(HZ);

  if (mb_info_ptr == 0) {
    pr_warn("Boot information missing! Attempting to continue with fallback.\n");
  }

  arch_region_count = 0;

  if (mb_magic == 0x2BADB002) {
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
  } else if (mb_magic == 0x36d24269) {
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
  } else if (mb_magic == 0x336ec578) {
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
      pr_warn("Unknown boot protocol (Magic: 0x%lx). Using 1GB default fallback.\n", mb_magic);
      /* Use 1GB fallback for QEMU compatibility if magic is missing */
      arch_mem_regions[0].base = 0x100000;
      arch_mem_regions[0].size = 1024ULL * 1024 * 1024;
      arch_mem_regions[0].type = MEM_REGION_USABLE;
      arch_region_count = 1;
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
    arch_yield();
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
void *arch_get_kernel_stack(uint32_t cpu_id) {
  if (cpu_id >= 16) return NULL;
  return (void *)&__kernel_stack[cpu_id * 131072];
}

/* Wake secondary CPU */
int arch_cpu_wake_secondary(uint64_t cpu_id, void (*entry)(void), void *stack) {
  (void)entry;
  (void)stack;
  
  if (cpu_id >= 16) return -1;

  /* TODO: Send INIT-SIPI via APIC to wake up secondary cores.
   * For now, we return 0 to indicate the system should continue, 
   * even if only one core is actually running. */
  pr_warn("AMD64: CPU %lu wakeup via APIC not yet implemented\n", cpu_id);
  return 0; 
}

/* Get boot information from Multiboot2 */
uint64_t arch_get_boot_info(void) { return (uint64_t)mb_info_ptr; }
