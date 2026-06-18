/*
 * kernel/kernel.c
 * Main kernel initialization and entry point
 */
#include <drivers/keyboard.h>
#include <drivers/virtio_blk.h>
#include <drivers/virtio_gpu.h>
#include <kernel/arch.h>
#include <kernel/bootmodule.h>
#include <kernel/buffer.h>
#include <kernel/cpu.h>
#include <kernel/drivers.h>
#include <kernel/ext4.h>
#include <kernel/fdt.h>
#include <kernel/gpt.h>
#include <kernel/graphics.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/platform.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/test.h>
#include <kernel/types.h>
#include <kernel/vmm.h>

/* Version */
#define KERNEL_VERSION_MAJOR 0
#define KERNEL_VERSION_MINOR 1
#define KERNEL_VERSION_PATCH 0
#ifdef ARCH_AMD64
#define KERNEL_NAME "AMD64 NexsOS1"
#else
#define KERNEL_NAME "AArch64 NexsOS1"
#endif

/* External symbols */
extern void secondary_cpu_entry(void); /* Assembly wrapper for AMD64 */
void kernel_secondary_main(void);
volatile uint32_t cpu_boot_ack = 0;

/* Forward declarations */
static void print_banner(void);
static void init_memory(void);
static void init_scheduler(void);

/*
 * Kernel main entry point
 */
/* Forward declaration for kernel_main */
#ifdef ARCH_AMD64
void kernel_main(uint64_t magic, uint64_t mbi_ptr);
#else
void kernel_main(uint64_t x0_arg);
#endif
extern void timer_init_percpu(void);

/* Kernel entry point - receives multiboot info pointer from bootloader */
#ifdef ARCH_AMD64
void kernel_main(uint64_t magic, uint64_t mbi_ptr) {
  /* For AMD64, bootloader passes mb_magic via RDI, mb_info_ptr via RSI */
  extern uint64_t mb_info_ptr;
  extern uint64_t mb_magic;
  mb_info_ptr = mbi_ptr;
  mb_magic = magic;
#else
void kernel_main(uint64_t x0_arg) {
#endif
  /* Initialize UART first for debug output */
  driver_console_init();

#ifndef ARCH_AMD64
  /* Ensure boot_fdt_ptr is set from the entry argument */
  boot_fdt_ptr = x0_arg;
  fdt_init(boot_fdt_ptr);
  pr_info("Kernel: Entry x0 = 0x%lx\n", x0_arg);
#else
  boot_fdt_ptr = 0;
  fdt_init(0);
#endif

  /* Print kernel banner */
  print_banner();

  /* CPU initialization (exception vectors, per-CPU data) */
  pr_info("%s", "Initializing CPU...\n");
  cpu_init();

  /* Platform-specific hardware registration */
  arch_platform_early_init();
  pr_info("%s", "Initializing IRQ...\n");
  driver_irq_init();
  irq_init();
  irq_init_percpu();

  /* System timer */
  pr_info("%s", "Initializing timer...\n");
  driver_timer_init();
  timer_init_percpu();

  /* Memory management */
  pr_info("%s", "Initializing memory...\n");
  init_memory();

  /* Process subsystem initialization (locks, etc.) */
  pr_info("%s", "Initializing processes...\n");
  process_init();

  /* Scheduler and First Process */
  pr_info("%s", "Initializing scheduler...\n");
  init_scheduler();

  /* Set CPU0 current task to idle (placeholder) to allow scheduling? */
  /* Actually, init_scheduler creates 'init' and 'idle' tasks. */
  /* We want CPU0 to pick 'init' immediately. */

  /* Wake secondary CPUs via Unified HAL */
  pr_info("%s", "Waking secondary CPUs...\n");
  arch_smp_init();

  /* Enable interrupts on primary core */
  pr_info("%s", "Enabling interrupts...\n");
  local_irq_enable();

  pr_info("%s", "Kernel initialized successfully!\n");
  pr_info("Boot info at: 0x%016lx\n", arch_get_boot_info());

  /* Main kernel loop */
  pr_info("%s", "Entering idle loop...\n");

  /* Enter supervisor loop */
  pr_info("%s", "[Init] Entering supervisor loop\n");
  while (1) {
    hal_cpu_idle();
  }
}

/*
 * Print kernel banner
 */
static void print_banner(void) {
  printk("\n");
  printk("========================================\n");
  printk("  %s v%d.%d.%d\n", KERNEL_NAME, KERNEL_VERSION_MAJOR,
         KERNEL_VERSION_MINOR, KERNEL_VERSION_PATCH);
  printk("  NeXsOS1\n");
  printk("========================================\n");
  printk("\n");
}

/*
 * Initialize memory subsystem
 */
static void init_memory(void) {
  /* Initialize physical memory manager with architecture-detected regions */
  size_t count = 0;
  struct mem_region *regions = arch_platform_get_mem_regions(&count);

  /* Reserve a boot module (the release rootfs disk.img, loaded into RAM by
   * GRUB) BEFORE the PMM is built, so it is never handed out as free RAM and
   * the metadata is placed clear of it.  No-op when there is no module
   * (aarch64, or the virtio-blk dev loop). */
  {
    uint64_t mb_base, mb_size;
    if (arch_platform_get_boot_module(&mb_base, &mb_size) && count < 32) {
      regions[count].base = mb_base;
      regions[count].size = mb_size;
      regions[count].type = MEM_REGION_RESERVED;
      count++;
    }
  }

  pmm_early_init(regions, count);
  pmm_init(regions, count);

  /* Initialize virtual memory manager (Phase 1: Bootstrap) */
  vmm_init();

  /* Phase 2: Dynamic RAM-aware remapping */
  vmm_dynamic_remap();

  /* Run unit tests now that PMM/VMM/kmalloc are live: memory tests (kmalloc
   * growth, vmm_protect) need real allocators, so the runner sits after the
   * MM bring-up instead of right after the banner. */
  ktest_run_all();

  /* Perform hardware discovery via Unified HAL */
  hal_bus_init();

  /* Initialize VirtIO Block Driver */
  virtio_blk_init();

  /* If the rootfs arrived as a boot module (release ISO), register the
   * RAM-backed ramdisk as the active block backend, overriding virtio-blk. */
  ramdisk_init();

  /* Initialize VirtIO GPU Driver */
  virtio_gpu_init();
  pr_info("%s", "VirtIO-GPU: Done.\n");

  /* Initialize Graphics Subsystem */
  graphics_init();

  /* Initialize GPT */
  gpt_init();
  pr_info("%s", "GPT: Done.\n");

  /* Initialize Buffer Cache */
  buffer_init();
  pr_info("%s", "Buffer: Done.\n");

  /* Mount the root filesystem: register providers, then probe partitions.
   * Composition root (ASTRA): the wiring fs-driver → VFS happens here only;
   * the rest of the kernel consumes the <kernel/vfs.h> contract. */
  vfs_register_fs(&ext4_fs_ops);
  vfs_init();
  pr_info("%s", "VFS: Done.\n");

  /* Initialize Keyboard */
  keyboard_init();

  /* Initialize System Registry */
  registry_init();
  pr_info("%s", "Registry: Initialized.\n");

  /* Note: Slab allocator (kmalloc) is auto-initialized on first use. */
}

/* smp_create_idle_task moved to arch-specific code or process.c */

/*
 * Initialize scheduler (placeholder)
 */
static void init_scheduler(void) {
  pr_info("%s", "Scheduler: Initializing...\n");

  /* Initialize Compositor */
  compositor_init();

  /* 1. Spawn the First-Stage Init Process (Must be PID 1) */
  pr_info("%s", "Scheduler: Spawning First-Stage Init...\n");
  struct process *init = process_create("init", PROC_PRIO_USER, PLVL_MACHINE);
  if (init && process_load_elf(init, "/sys/bin/init") == 0) {
    pr_info("Scheduler: Initialized PID %d (/sys/bin/init)\n", init->pid);
    enqueue_task(init);
  } else {
    panic("Failed to load /init");
  }

  /* 2. Create Idle Task for CPU 0 */
  smp_create_idle_task(0);
}

/*
 * Secondary CPU entry point
 */
void kernel_secondary_main(void) {
  uint32_t cpu = (uint32_t)hal_cpu_id();

  /* Initialize per-CPU state */
  cpu_init();
  irq_init_percpu();
  timer_init_percpu();

  /* Enable interrupts */
  local_irq_enable();

  /* Acknowledge boot to primary core */
  cpu_boot_ack = cpu;

  pr_info("Secondary CPU %u online and ready\n", cpu);

  /* Enter idle loop - scheduler will preempt this */
  while (1) {
    hal_cpu_idle();
  }
}
