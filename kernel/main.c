/*
 * kernel/kernel.c
 * Main kernel initialization and entry point
 */
#include <drivers/gic.h>
#include <drivers/keyboard.h>
#include <drivers/timer.h>
#include <drivers/uart.h>
#include <drivers/virtio_blk.h>
#include <drivers/virtio_gpu.h>
#include <kernel/arch.h>
#include <kernel/buffer.h>
#include <kernel/cpu.h>
#include <kernel/ext4.h>
#include <kernel/gpt.h>
#include <kernel/graphics.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>

/* Version */
#define KERNEL_VERSION_MAJOR 0
#define KERNEL_VERSION_MINOR 1
#define KERNEL_VERSION_PATCH 0
#define KERNEL_NAME "AArch64 Microkernel"

/* External symbols */
extern uint64_t boot_info;
extern void cpu_init(void);
extern int cpu_wake_secondary(uint64_t cpu_id, void (*entry)(void),
                              void *stack);
extern void local_irq_enable(void);
extern void secondary_cpu_entry(void);
extern char __kernel_stack[];
extern uint64_t smp_boot_magic; // Assuming this is a new external symbol for
                                // the cache flush

/* Forward declarations */
static void print_banner(void);
static void init_memory(void);
static void init_scheduler(void);

/*
 * Kernel main entry point
 */
/* Forward declaration for kernel_main */
void kernel_main(void);

void kernel_main(void) {
  /* Initialize UART first for debug output */
  uart_init();

  /* Print kernel banner */
  print_banner();

  /* CPU initialization (exception vectors, per-CPU data) */
  pr_info("%s", "Initializing CPU...\n");
  cpu_init();

  /* Interrupt controller */
  pr_info("%s", "Initializing GIC...\n");
  gic_init();
  gic_init_percpu();

  /* System timer */
  pr_info("%s", "Initializing timer...\n");
  timer_init();
  timer_init_percpu();

  /* Memory management */
  pr_info("%s", "Initializing memory...\n");
  init_memory();

  /* Process subsystem initialization (locks, etc.) */
  process_init();

  /* Scheduler and First Process */
  init_scheduler();

  /* Set CPU0 current task to idle (placeholder) to allow scheduling? */
  /* Actually, init_scheduler creates 'init' and 'idle' tasks. */
  /* We want CPU0 to pick 'init' immediately. */

  /* Wake secondary CPUs (1-3) */
  pr_info("%s", "Waking secondary CPUs...\n");

  /* Write TTBR0 for secondary cores */
  extern uint64_t secondary_ttbr0;
  uint64_t current_ttbr0 = arch_get_ttbr0();
  secondary_ttbr0 = current_ttbr0;
  /* Flush to PoC so secondary cores see it */
  arch_clean_cache_va(&secondary_ttbr0);
  arch_dsb();

  for (int i = 1; i < 4; i++) {
    void *stack = (void *)&__kernel_stack[(i + 1) * 131072];
    int ret = cpu_wake_secondary(i, secondary_cpu_entry, stack);
    if (ret != 0) {
      pr_err("Failed to wake CPU %d: ret=%d (0x%x)\n", i, ret, ret);
    } else {
      pr_info("CPU %d woken successfully\n", i);
    }
  }

  /* Enable interrupts on primary core */
  pr_info("%s", "Enabling interrupts...\n");
  local_irq_enable();

  pr_info("%s", "Kernel initialized successfully!\n");
  pr_info("Boot info at: 0x%016lx\n", boot_info);

  /* Main kernel loop */
  pr_info("%s", "Entering idle loop...\n");

  /* Enter supervisor loop */
  pr_info("%s", "[Init] Entering supervisor loop\n");
  while (1) {
    arch_wfi();
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
  printk("  Production-Ready AArch64 Kernel\n");
  printk("========================================\n");
  printk("\n");
}

/*
 * Initialize memory subsystem
 */
static void init_memory(void) {
  /* Initialize physical memory manager */
  pmm_init(NULL, 0); /* Use default 1GB memory detection */

  /* Initialize virtual memory manager */
  vmm_init();

  /* Initialize VirtIO Block Driver */
  virtio_blk_init();

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

  /* Initialize Ext4 */
  ext4_init();
  pr_info("%s", "Ext4: Done.\n");

  /* Initialize Keyboard */
  keyboard_init();

  /* Initialize System Registry */
  registry_init();
  pr_info("%s", "Registry: Initialized.\n");

  /* TODO: Initialize slab allocator */
}

/*
 * Initialize scheduler (placeholder)
 */
static void init_scheduler(void) {
  pr_info("%s", "Scheduler: Initializing...\n");

  /* Initialize Compositor */
  compositor_init();

  /* 1. Spawn the First-Stage Init Process (Must be PID 1) */
  pr_info("%s", "Scheduler: Spawning First-Stage Init...\n");
  struct process *init =
      process_create("init", PROC_PRIO_USER, PROC_PERM_SYSTEM);
  if (init && process_load_elf(init, "/bin/init") == 0) {
    pr_info("Scheduler: Initialized PID %d (/init)\n", init->pid);
    enqueue_task(init);
  } else {
    panic("Failed to load /init");
  }

  /* 2. Create 4 Idle Tasks - One for each CPU */
  extern void idle_task_entry(void);
  for (int i = 0; i < 4; i++) {
    struct process *idle =
        process_create("idle", PROC_PRIO_IDLE, PROC_PERM_SYSTEM);
    if (idle) {
      idle->on_cpu = i; /* Bind to specific CPU */
      cpu_data[i].idle_task = idle;
      
      /* Explicitly initialize context to known state */
      memset(idle->context, 0, sizeof(struct pt_regs));
      idle->context->elr = (uint64_t)idle_task_entry;
      idle->context->spsr = 0x05; /* EL1h + Unmasked */
      idle->context->sp_el0 = 0;

      /* CRITICAL: Flush idle task context frame and the process struct itself to POC */
      arch_clean_cache_range_va(idle, sizeof(struct process));
      arch_clean_cache_range_va(idle->context, sizeof(struct pt_regs));
      arch_dsb();
      arch_isb();

      enqueue_task(idle);
    }
  }
}

/*
 * Secondary CPU entry point
 */
void secondary_cpu_entry(void) {
  uint32_t cpu = (uint32_t)arch_get_cpu_id();

  /* Initialize per-CPU state */
  cpu_init();
  gic_init_percpu();
  timer_init_percpu();

  /* Enable interrupts */
  local_irq_enable();

  pr_info("Secondary CPU %u online and ready\n", cpu);

  /* Enter idle loop - scheduler will preempt this */
  while (1) {
    arch_wfi();
  }
}
