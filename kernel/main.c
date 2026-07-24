/*
 * kernel/kernel.c
 * Main kernel initialization and entry point
 */
#include <drivers/keyboard.h>
#include <drivers/virtio_blk.h>
#include <drivers/virtio_gpu.h>
#include <kernel/arch.h>
#include <kernel/bootmodule.h>
#include <kernel/bootphase.h>
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
#include <kernel/procfs.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/ssp.h>
#include <kernel/string.h>
#include <kernel/test.h>
#include <kernel/types.h>
#include <kernel/vmm.h>

/* Version */
#define KERNEL_VERSION_MAJOR 0
#define KERNEL_VERSION_MINOR 0
#define KERNEL_VERSION_PATCH 5
#define KERNEL_VERSION_BUILD 3

#ifdef ARCH_AMD64
#define KERNEL_NAME "AMD64 NexsOS1"
#else
#define KERNEL_NAME "AArch64 NexsOS1"
#endif

/* External symbols */
extern void secondary_cpu_entry(void); /* Assembly wrapper for AMD64 */
void kernel_secondary_main(void);
/* The boot-ack handshake cell lives in kernel/core/smp.c (S-ALIGN F8). */

/* Boot-phase tracking (S-ALIGN F9, kernel/bootphase.h): single-writer (BSP,
 * this file), read by panic() to stamp fault reports.  K3 (userland) is
 * entered ONLY after K1+K2 are confirmed — the explicit gate the K1/K2/K3
 * model requires. */
static enum boot_phase boot_phase = BOOT_PHASE_K1_HW;
void boot_phase_set(enum boot_phase p) { boot_phase = p; }
enum boot_phase boot_phase_get(void) { return boot_phase; }
const char *boot_phase_name(enum boot_phase p) {
  switch (p) {
  case BOOT_PHASE_K1_HW:
    return "K1-hw";
  case BOOT_PHASE_K2_SUBSYS:
    return "K2-subsys";
  case BOOT_PHASE_K3_USERLAND:
    return "K3-userland";
  default:
    return "running";
  }
}

/* Forward declarations */
static void print_banner(void);
static void init_memory(void);
static void init_scheduler(void);
static int spawn_init_process(void);

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

  /* Reseed the SSP stack canary from arch entropy before any deep call chain
   * runs (LIB-SSP-01 / #71).  Safe here: kernel_main never returns through a
   * canary-checked epilogue, so replacing the global guard mid-boot cannot
   * trip a stale-canary check on a frame already on the stack. */
  stack_guard_init();

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

  /* Scheduler (K2): compositor + CPU0's idle task.  PID1 is NOT spawned
   * here any more — K3 is gated below. */
  pr_info("%s", "Initializing scheduler...\n");
  init_scheduler();

  /* Wake secondary CPUs via Unified HAL (completes K1: whole machine up). */
  pr_info("%s", "Waking secondary CPUs...\n");
  arch_smp_init();

  /* ---- K3 GATE (S-ALIGN F9): K1+K2 confirmed, only now start userland. ----
   * Previously PID1 was created+enqueued in init_scheduler() BEFORE
   * arch_smp_init(): an early-woken AP could work-steal PID1 and run
   * userland while the BSP was still bringing up later CPUs (kernel audit
   * §1.3, the sharpest phase-blur).  A K3-only failure (userland won't
   * load) no longer panics a healthy K1+K2: the kernel stays alive and
   * diagnosable on the UART (kernel-alone mode). */
  boot_phase_set(BOOT_PHASE_K3_USERLAND);
  if (spawn_init_process() != 0)
    pr_err("%s", "K3: userland failed to start — kernel-alone mode "
                 "(K1+K2 healthy, no reboot; inspect via UART)\n");

  /* Enable interrupts on primary core */
  pr_info("%s", "Enabling interrupts...\n");
  local_irq_enable();
  boot_phase_set(BOOT_PHASE_RUNNING);

  pr_info("%s", "Kernel initialized successfully!\n");
  pr_info("Boot info at: 0x%016lx\n", arch_get_boot_info());

  /* CPU0 idle loop: from here on all work happens in scheduled tasks. */
  pr_info("%s", "Entering idle loop...\n");
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
  printk(" %s v%d.%d.%d.%d\n", KERNEL_NAME, KERNEL_VERSION_MAJOR,
         KERNEL_VERSION_MINOR, KERNEL_VERSION_PATCH, KERNEL_VERSION_BUILD);
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

  /* K1→K2 boundary (S-ALIGN F9): memory/hardware is up; everything from
   * hal_bus_init() on is subsystem init (bus/block/GPU/graphics/GPT/buffer/
   * VFS/keyboard/registry/procfs — the kernel-audit §1.1 boot map). */
  boot_phase_set(BOOT_PHASE_K2_SUBSYS);

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
  /* Mount it as the "/reg" file namespace (Plan 9-style): registry state is now
   * reachable through the uniform VFS (e.g. cat /reg/system/hostname). */
  registry_mount_vfs();
  /* Mount /proc: live processes as TYPED capability objects in the namespace
   * (open /proc/<pid> -> an OBJ_TYPE_PROCESS object). */
  procfs_init();
  pr_info("%s", "Registry: Initialized.\n");

  /* Note: Slab allocator (kmalloc) is auto-initialized on first use. */
}

/* smp_create_idle_task moved to arch-specific code or process.c */

/*
 * init_scheduler (K2): compositor + CPU0's idle task.  Userland (PID1) is
 * deliberately NOT started here — that is K3, gated in kernel_main after
 * arch_smp_init() confirms the whole machine (S-ALIGN F9).
 */
static void init_scheduler(void) {
  pr_info("%s", "Scheduler: Initializing...\n");

  /* Initialize Compositor */
  compositor_init();

  /* Create Idle Task for CPU 0 */
  smp_create_idle_task(0);

  /* Input server thread (DIR-02/DIR-03, #68/#194) is STAGED, not launched:
   * the arch_cpu_yield cooperative-switch-to-user path is still being
   * hardened.  Input dispatches synchronously meanwhile (see keyboard.c). */
}

/*
 * spawn_init_process (K3): create, load and enqueue PID1 (nx).
 * ROOT + explicit caps, NOT machine: PLVL_MACHINE is the machine's own
 * identity (B3 §3.1) — it would make PID1 unkillable and exempt from every
 * capability check and from the creator clamp for all its children.
 *
 * Returns 0 on success, -1 if userland could not be started — the caller
 * keeps the kernel alive (kernel-alone mode) instead of panicking: a
 * K3-only failure must not take down a healthy K1+K2.
 */
static int spawn_init_process(void) {
  pr_info("%s", "K3: Spawning First-Stage Init...\n");
  struct process *nxinit =
      process_create_caps("nxinit", PROC_PRIO_USER, PLVL_MACHINE, CAP_ALL);
  if (nxinit && process_load_elf(nxinit, "/sys/bin/nxinit") == 0) {
    pr_info("K3: Initialized PID %d (/sys/bin/nxinit)\n", nxinit->pid);
    enqueue_task(nxinit);
    return 0;
  }
  return -1;
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

  /* SMP-IDLE-RACE (#169/#170): do NOT enable interrupts — which lets the timer
   * drive schedule() — until this CPU's idle task is visible.  schedule() picks
   * cpu_info->idle_task when nothing else is runnable; if it is still NULL the
   * context switch jumps into NULL -> #PF/#GP at tiny addresses / corrupted
   * return frames.  The BSP now publishes it BEFORE waking us, so this passes
   * immediately; the acquire-load is the explicit ordering/visibility guard
   * (and covers a slow hypervisor like UTM where the AP starts late). */
  struct cpu_info *ci = get_cpu_info();
  while (!__atomic_load_n(&ci->idle_task, __ATOMIC_ACQUIRE))
    __asm__ volatile("" ::: "memory");

  /* Enable interrupts */
  local_irq_enable();

  /* Acknowledge boot to primary core (release store inside smp_ack_boot:
   * everything this CPU initialized above is visible before the BSP sees
   * the ack — the handshake lives in kernel/core/smp.c, S-ALIGN F8). */
  smp_ack_boot(cpu);

  pr_info("Secondary CPU %u online and ready\n", cpu);

  /* Enter idle loop - scheduler will preempt this */
  while (1) {
    hal_cpu_idle();
  }
}
