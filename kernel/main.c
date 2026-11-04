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
#ifndef ARCH_AMD64
#include <kernel/boot_info.h>
#endif
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
#define KERNEL_VERSION_BUILD 5

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
  case BOOT_PHASE_RUNNING:
    return "running";
  default:
    return "unknown";
  }
}

/* Forward declarations */
static void print_banner(void);
static void init_memory(void);
static void init_scheduler(void);
static void init_compositor(void);
static int spawn_init_process(void);

/* FIX(NASA-01, Power of Ten Rule 4 — short functions): kernel_main() used to
 * be one ~110-line function covering K1 (hardware bring-up) through the
 * idle loop.  Split into two named, single-purpose stage functions so each
 * stays reviewable on its own and the K1/K2/K3 boundary already documented
 * in comments is now also a function boundary, not just a comment.  Boot
 * ORDER is UNCHANGED from the original — this is a pure decomposition, not
 * a reordering: local_irq_enable() still runs at the very end, after the K3
 * gate (spawn_init_process()), exactly as before.  Neither stage function
 * is reentrant and each runs exactly once, from kernel_main(), on the BSP
 * only — this is not a general-purpose helper. */
static void kernel_stage_k1_hardware(uint64_t boot_arg0, uint64_t boot_arg1);
static void kernel_stage_k2_k3(void);

/*
 * Kernel main entry point
 */
/* Forward declaration for kernel_main */
#ifdef ARCH_AMD64
void kernel_main(uint64_t magic, uint64_t mbi_ptr);
#else
void kernel_main(uint64_t x0_arg, uint64_t x1_arg);
#endif
extern void timer_init_percpu(void);

/* Kernel entry point - receives multiboot info pointer from bootloader */
#ifdef ARCH_AMD64
void kernel_main(uint64_t magic, uint64_t mbi_ptr) {
  kernel_stage_k1_hardware(magic, mbi_ptr);
#else
void kernel_main(uint64_t x0_arg, uint64_t x1_arg) {
  kernel_stage_k1_hardware(x0_arg, x1_arg);
#endif
  kernel_stage_k2_k3();
}

/*
 * kernel_stage_k1_hardware - K1: bring up this CPU and the raw platform
 * (console, CPU exception/IDT state, IRQ controller, timer) to the point
 * where memory management and the scheduler can be initialized.
 *
 * @boot_arg0: amd64: multiboot magic (RDI). aarch64: x0 (DTB physical base).
 * @boot_arg1: amd64: multiboot info pointer (RSI). aarch64: x1 (validated DTB
 *             available span; zero only for the explicit legacy adapter).
 *
 * Called exactly once, from kernel_main(), before kernel_stage_k2_k3().
 * Not reentrant; must run on the BSP with a valid boot stack and nothing
 * else concurrently touching arch/platform globals.
 */
static void kernel_stage_k1_hardware(uint64_t boot_arg0, uint64_t boot_arg1) {
#ifdef ARCH_AMD64
  /* For AMD64, bootloader passes mb_magic via RDI, mb_info_ptr via RSI */
  extern uint64_t mb_info_ptr;
  extern uint64_t mb_magic;
  mb_info_ptr = boot_arg1;
  mb_magic = boot_arg0;
#endif
  /* Initialize UART first for debug output. */
  driver_console_init();

  /* Print kernel banner before the exception vectors are installed. */
  print_banner();

  /* Reseed the SSP stack canary from arch entropy before any deep call chain
   * runs (LIB-SSP-01 / #71). Safe here because kernel_main never returns
   * through a canary-checked epilogue. */
  stack_guard_init();

  /* CPU initialization (exception vectors, per-CPU data) */
  pr_info("%s", "Initializing CPU...\n");
  cpu_init();

#ifndef ARCH_AMD64
  /* Validate the handoff record before touching the physical DTB. */
  if (boot_info.magic != BOOT_INFO_MAGIC ||
      boot_info.version != BOOT_INFO_VERSION ||
      boot_info.size != BOOT_INFO_SIZE ||
      boot_info.fdt_phys != boot_arg0 ||
      boot_info.fdt_size != boot_arg1)
    panic("BootInfo: invalid AArch64 handoff record");

  boot_info.source = boot_arg1 != 0
      ? BOOT_INFO_SOURCE_AARCH64_EFI : BOOT_INFO_SOURCE_LEGACY;
  boot_info.flags = 0; /* Commit flags only after successful DTB validation. */

  size_t actual_fdt_size = 0;
  int fdt_status = boot_arg1 != 0
      ? fdt_init((uintptr_t)boot_arg0, (size_t)boot_arg1)
      : fdt_init_legacy((uintptr_t)boot_arg0, &actual_fdt_size);
  if (fdt_status != FDT_PARSER_OK)
    panic("FDT: validation failed status=%d base=0x%lx span=0x%lx",
          fdt_status, boot_arg0, boot_arg1);

  if (boot_arg1 == 0 && actual_fdt_size != boot_fdt_size)
    panic("BootInfo: legacy DTB length mismatch handoff=0x%lx parsed=0x%lx",
          (uint64_t)actual_fdt_size, boot_fdt_size);

  /* Store the actual validated FDT totalsize, not the EFI input span. */
  boot_info.fdt_phys = boot_fdt_ptr;
  boot_info.fdt_size = boot_fdt_size;
  if (boot_arg1 != 0) {
    boot_info.flags = BOOT_INFO_FLAG_FDT_SIZE_VALID;
  } else if (boot_arg0 != 0) {
    boot_info.flags = BOOT_INFO_FLAG_LEGACY_TRUSTED_INPUT;
  } else {
    boot_info.flags = BOOT_INFO_FLAG_LEGACY_SEARCH;
  }
  pr_info("BootInfo: source=%u DTB=0x%lx size=0x%lx flags=0x%x\n",
          boot_info.source, boot_info.fdt_phys, boot_info.fdt_size,
          boot_info.flags);
  pr_info("Kernel: Entry x0 = 0x%lx x1 = 0x%lx\n",
          boot_arg0, boot_arg1);
#else
  if (fdt_init(0, 0) != FDT_PARSER_OK)
    pr_info("%s", "FDT: no device tree found — expected on amd64, devices come "
                  "from PCI enumeration\n");
#endif

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
}

/*
 * kernel_stage_k2_k3 - K2 (memory/process/scheduler/SMP/compositor
 * subsystem bring-up) followed by the K3 gate (userland spawn) and the
 * final interrupt-enable + idle loop.  Ordering is exactly as it was in
 * the original monolithic kernel_main(): local_irq_enable() runs LAST,
 * after spawn_init_process(), not before it.
 *
 * Called exactly once, from kernel_main(), immediately after
 * kernel_stage_k1_hardware() returns. Never returns (ends in the idle
 * loop). Not reentrant.
 */
static void kernel_stage_k2_k3(void) {
  /* Memory management */
  pr_info("%s", "Initializing memory...\n");
  init_memory();

  /* Process subsystem initialization (locks, etc.) */
  pr_info("%s", "Initializing processes...\n");
  process_init();

  /* Scheduler (K2): CPU0's idle task ONLY.  The compositor is no longer part
   * of scheduler init — it is a graphics subsystem, brought up by
   * init_compositor() below, after every CPU has an idle task.  PID1 is NOT
   * spawned here — K3 is gated below. */
  pr_info("%s", "Initializing scheduler...\n");
  init_scheduler();

  /* Wake secondary CPUs via Unified HAL (completes K1: whole machine up).
   * The arch SMP code publishes each AP's idle task BEFORE that AP can take
   * a timer tick (SMP-IDLE-RACE #169/#170), so when this returns every
   * cpu_info->idle_task in the machine is non-NULL. */
  pr_info("%s", "Waking secondary CPUs...\n");
  arch_smp_init();

  /* Compositor (K2, graphics subsystem) — SEPARATED from the scheduler and
   * deliberately started AFTER the idle tasks: scheduler init now owns
   * nothing but scheduling, and the compositor comes up once the whole
   * machine is idle-capable.  It still runs before the K3 gate, so userland
   * never starts against an uninitialized compositor.  Ordering assumption:
   * with the APs parked in their idle loops, nothing on the timer/schedule
   * path calls into the compositor — userland is its only entry point, and
   * userland starts later (K3). */
  pr_info("%s", "Initializing compositor...\n");
  init_compositor();

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

  /* CPU0 idle loop: from here on all work happens in scheduled tasks.
   * NASA-02 (Power of Ten Rule 2 — bounded loops): this loop is intentionally
   * unbounded. kernel_main() never returns on a running kernel by design —
   * there is no caller to return to and no shutdown path that resumes this
   * frame — so an upper iteration bound is not applicable here, unlike every
   * other loop in this file (all of which are bounded and asserted below).
   * Documented per the JPL exception process for the one genuine
   * event-loop/idle-loop case a kernel entry point requires. */
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

/* FIX(NASA-01, Power of Ten Rule 4 — short functions): init_memory() used
 * to be one function covering both physical/virtual memory bring-up and
 * every K2 subsystem driver (bus/block/GPU/graphics/GPT/buffer/VFS/
 * keyboard/registry/procfs).  Split at the boundary the code already named
 * ("K1->K2 boundary", boot_phase_set(BOOT_PHASE_K2_SUBSYS)) into two
 * single-purpose static functions; init_memory() itself is now a 3-line
 * composition of the two, in the SAME order as the original.  Neither half
 * is reentrant; both run exactly once, from init_memory(), on the BSP only. */
static void init_memory_core(void);
static void init_memory_subsystems(void);

/*
 * Initialize memory subsystem
 */
static void init_memory(void) {
  init_memory_core();
  init_memory_subsystems();
}

/*
 * init_memory_core - K1 tail: physical/virtual memory manager bring-up.
 * Brings PMM/VMM online and runs the post-MM unit test pass. Must run
 * before init_memory_subsystems() (every K2 driver below allocates memory).
 */
static void init_memory_core(void) {
  /* Initialize physical memory manager with architecture-detected regions */
  size_t count = 0;
  struct mem_region *regions = arch_platform_get_mem_regions(&count);
  if (regions == NULL || count == 0)
    panic("%s", "BOOT-MEM: platform returned no validated memory regions");

  /* Reserve a boot module (the release rootfs disk.img, loaded into RAM by
   * GRUB) BEFORE the PMM is built, so it is never handed out as free RAM and
   * the metadata is placed clear of it.  No-op when there is no module
   * (aarch64, or the virtio-blk dev loop). */
  {
    uint64_t mb_base, mb_size;
    if (arch_platform_get_boot_module(&mb_base, &mb_size)) {
      if (count >= ARCH_MAX_MEM_REGIONS)
        panic("%s", "BOOT-MEM: no room for reserved boot-module descriptor");
      regions[count].base = mb_base;
      regions[count].size = mb_size;
      regions[count].type = MEM_REGION_RESERVED;
      count++;
    }
  }

  /* Emit the exact map consumed by PMM.  Report usable sum separately from
   * highest-end span: address holes and reserved overlays are not RAM. */
  uint64_t boot_usable_bytes = 0;
  uint64_t boot_highest_usable_end = ARCH_RAM_START;
  uint64_t boot_highest_reserved_end = 0;
  size_t boot_reserved_regions = 0;
  for (size_t i = 0; i < count; i++) {
    if (regions[i].size > UINT64_MAX - regions[i].base)
      panic("BOOT-MEM: region %zu overflows physical address space", i);
    uint64_t end = regions[i].base + regions[i].size;
    pr_info("BOOT-MEM: region[%zu] type=%u base=0x%lx size=0x%lx end=0x%lx\n",
            i, (unsigned)regions[i].type,
            (unsigned long)regions[i].base,
            (unsigned long)regions[i].size, (unsigned long)end);
    if (regions[i].type == MEM_REGION_USABLE) {
      if (end > boot_highest_usable_end)
        boot_highest_usable_end = end;
    } else {
      boot_reserved_regions++;
      if (end > boot_highest_reserved_end)
        boot_highest_reserved_end = end;
    }
  }
  if (boot_highest_usable_end <= ARCH_RAM_START)
    panic("%s", "BOOT-MEM: no usable RAM above architecture memory base");
  boot_highest_usable_end &= ~((uint64_t)PAGE_SIZE - 1ULL);
  boot_usable_bytes = pmm_calculate_usable_bytes(
      regions, count, ARCH_RAM_START, boot_highest_usable_end);
  if (boot_usable_bytes == 0)
    panic("%s", "BOOT-MEM: usable RAM union is empty after page alignment");

  pr_info("BOOT-MEM: regions=%zu usable=%lu MiB usable_span=%lu MiB "
          "base=0x%lx reserved_descriptors=%zu highest_reserved_end=0x%lx\n",
          count, (unsigned long)(boot_usable_bytes / (1024UL * 1024UL)),
          (unsigned long)((boot_highest_usable_end - ARCH_RAM_START) /
                          (1024UL * 1024UL)),
          (unsigned long)ARCH_RAM_START, boot_reserved_regions,
          (unsigned long)boot_highest_reserved_end);

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
}

/*
 * init_memory_subsystems - K2: bus/block/GPU/graphics/filesystem/input/
 * registry driver bring-up. Must run after init_memory_core() (every
 * driver here allocates through the PMM/VMM/kmalloc that function brings
 * up) and before process_init()/init_scheduler() (K2 userland cannot read
 * a filesystem or take keyboard input that has not been wired up yet).
 */
static void init_memory_subsystems(void) {
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
  /* Composition root: if the only on-disk filesystem provider fails to
   * register, the root mount below has nothing to mount WITH, and the symptom
   * is an unbootable system reported as "no root filesystem" rather than as a
   * registration that failed here. */
  if (vfs_register_fs(&ext4_fs_ops) != 0)
    panic("VFS: ext4 provider could not be registered — no root filesystem is "
          "possible");
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
 * init_scheduler (K2): pure scheduling concerns — CPU0's idle task only.
 * The compositor used to live here; it is a graphics subsystem and is now
 * initialized by init_compositor(), after all idle tasks exist.  Userland
 * (PID1) is deliberately NOT started here either — that is K3, gated in
 * kernel_main after arch_smp_init() confirms the whole machine (S-ALIGN F9).
 */
static void init_scheduler(void) {
  pr_info("%s", "Scheduler: Initializing...\n");

  /* Create Idle Task for CPU 0 */
  smp_create_idle_task(0);

  /* Input server thread (DIR-02/DIR-03, #68/#194) is STAGED, not launched:
   * the arch_cpu_yield cooperative-switch-to-user path is still being
   * hardened.  Input dispatches synchronously meanwhile (see keyboard.c). */
}

/*
 * init_compositor (K2, graphics): separated from the scheduler — it has no
 * scheduling dependency.  It only needs the GPU/framebuffer bring-up already
 * done in init_memory() (virtio_gpu_init / graphics_init) and a machine in
 * which every CPU has an idle task, which is guaranteed after arch_smp_init()
 * returns.  Called before the K3 gate so userland finds it ready.
 */
static void init_compositor(void) {
  pr_info("%s", "Compositor: Initializing...\n");
  compositor_init();
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
  if (nxinit && process_load_elf(nxinit, "/sbin/nxinit") == 0) {
    pr_info("K3: Initialized PID %d (/sbin/nxinit)\n", nxinit->pid);
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

  /* Acknowledge boot BEFORE enabling interrupts.  The idle task is already
   * published and all per-CPU hardware state above is initialized, so the BSP
   * may safely observe this CPU as ready.  Reversing these two operations is
   * essential: if the local timer IRQ fires immediately after IRQ enable, the
   * scheduler can preempt this boot path before smp_ack_boot(), leaving the
   * BSP spinning until its handshake timeout even though the AP has started.
   * Release-store in smp_ack_boot publishes all per-CPU initialization. */
  smp_ack_boot(cpu);

  /* Only now allow the timer/scheduler to preempt the secondary boot path. */
  local_irq_enable();

  pr_info("Secondary CPU %u online and ready\n", cpu);

  /* Enter idle loop - scheduler will preempt this */
  while (1) {
    hal_cpu_idle();
  }
}