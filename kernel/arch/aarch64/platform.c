/*
 * kernel/arch/aarch64/platform.c
 * Platform hardware discovery and SMP bring-up for AArch64 (QEMU virt machine).
 *
 * Role:
 *   This file implements the arch_platform_* interface declared in
 *   kernel/include/kernel/platform.h for the AArch64 target.  It is responsible
 *   for three tasks:
 *   1. Early init (arch_platform_early_init): register the GIC interrupt
 *      controller and initialise the FDT/DTB parser.
 *   2. Memory discovery (arch_platform_get_mem_regions): obtain a list of usable
 *      RAM regions for the PMM.  Primary path: FDT "memory" node parse.  Fallback:
 *      manual physical-page probe using the probe_in_progress / probe_failed flags
 *      and the EL1 Data Abort recovery path in sync_handler (cpu.c).
 *   3. SMP bring-up (arch_smp_init): wake secondary CPUs via PSCI CPU_ON
 *      (arch_cpu_wake_secondary, start.S), wait for each to acknowledge, and
 *      create an idle task for each newly-online CPU.
 *
 * Known issues:
 *   ARCH-04 (W2 BAD-IMPL·BUG) The manual probe fallback writes arch_mem_regions[0]
 *            TWICE: once at lines 83–86 (after finding found_end), and again at
 *            91–94 (the "safety" adjustment).  The second write silently overwrites
 *            the first, making the lines 83–86 dead.  The size computed in the second
 *            write also subtracts PAGE_SIZE regardless of whether found_end > probe_base,
 *            which can produce size == 0 when found_end == probe_base. [static]
 *   ARCH-05 (W2 REFINE) The probe reads at 128MB intervals (0x08000000) from the
 *            QEMU virt base.  On real hardware some physical addresses in the probe
 *            range map to device registers that do not cleanly abort; a read could
 *            trigger an unintended device side-effect before the sync exception fires.
 */
#include <kernel/fdt.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/memlayout.h>
#include <kernel/pmm.h>
#include <kernel/platform.h>
#include <kernel/printk.h>
#include <drivers/gic.h>
#include <drivers/uart.h>

/*
 * arch_mem_regions[] - static array of usable RAM descriptors filled by
 * arch_platform_get_mem_regions() from FDT or the manual probe.
 * Sized to MAX_CPUS * 2 which is larger than the typical 1–2 QEMU virt
 * memory regions; this avoids dynamic allocation during early boot.
 */
static struct mem_region arch_mem_regions[MAX_CPUS * 2]; /* Enough slots */
static size_t arch_region_count = 0;

/*
 * arch_platform_early_init - very early platform setup before PMM is available.
 *
 * Called by kernel_main() before memory discovery; must not call kmalloc.
 *
 * Actions:
 *   1. Register the GIC (Generic Interrupt Controller) driver via gic_register().
 *      This installs the GIC's irq_ctrl_ops into the kernel IRQ subsystem;
 *      the GIC is not yet initialised for per-CPU distributor/redistributor
 *      setup (that happens later in arch_irq_init / hal.c).
 *   2. Parse the Device Tree Blob (DTB) passed by the firmware/QEMU in x0 at
 *      kernel entry.  The DTB pointer is saved as boot_fdt_ptr in start.S.
 *      fdt_init() makes the FDT available for subsequent fdt_get_mem_regions()
 *      and fdt_count_cpus() calls.
 *
 * If boot_fdt_ptr is NULL (no DTB from firmware), hardware discovery falls back
 * to the manual probe path in arch_platform_get_mem_regions (ARCH-04/05).
 */
void arch_platform_early_init(void) {
    uart_puts("PLATFORM: arch_platform_early_init starting\n");
    /*
     * Register the Interrupt Controller.
     * gic_register() installs the GIC irq_ctrl_ops; the physical GIC init
     * (GICD/GICR programming) happens later in arch_irq_init().
     */
    gic_register();

    /* Initialize FDT parser from the DTB pointer saved by start.S at _start. */
    if (boot_fdt_ptr) {
        if (fdt_init(boot_fdt_ptr) == 0) {
            pr_info("AArch64: IDTF/FDT initialized from 0x%lx\n", boot_fdt_ptr);
        } else {
            pr_warn("AArch64: [IDTF] Failed to initialize FDT from 0x%lx! Hardware discovery compromised.\n", boot_fdt_ptr);
        }
    } else {
        pr_warn("%s", "AArch64: [IDTF] No Hardware Description Tree (DTB) found! System will attempt manual discovery.\n");
    }
}

/*
 * arch_platform_get_mem_regions - discover and return usable RAM regions.
 *
 * Parameters:
 *   count  Output: number of valid entries in the returned array.
 * Returns: pointer to the static arch_mem_regions[] array.
 *
 * Idempotent: if arch_region_count > 0 the cached result is returned immediately.
 *
 * Primary path — FDT memory parse:
 *   fdt_get_mem_regions() walks the FDT "/memory" nodes and fills arch_mem_regions[].
 *   On QEMU virt with a DTB, this correctly discovers all RAM (e.g., 3967 MB at
 *   0x40000000 for "-m 4G").
 *
 * Fallback path — manual physical probe (NOTE(ARCH-04), NOTE(ARCH-05)):
 *   When FDT parse fails or returns 0 regions, the code performs a bus-abort probe:
 *   - Start at 0x40000000 + 1GB (skip the kernel's own 1GB).
 *   - Advance in 128MB (0x08000000) steps up to a 128GB ceiling.
 *   - For each candidate address: set probe_in_progress, attempt a volatile read,
 *     then check probe_failed (set by sync_handler if a Data Abort fires).
 *   - Stop when probe_failed is true; found_end is the last successfully read addr.
 *
 *   NOTE(ARCH-04): The probe result is written to arch_mem_regions[0] TWICE:
 *     First write (lines 83–86): base=probe_base, size=found_end-probe_base.
 *       This is immediately followed by arch_region_count=1 and a pr_info.
 *     Second write (lines 91–94): base=probe_base, size=(found_end-probe_base-PAGE_SIZE)
 *       or 0.  This is the effective write; lines 83–86 are dead code.
 *     The second write also sets arch_region_count=1 redundantly.
 *     If found_end == probe_base (nothing found), size is set to 0.
 *
 *   NOTE(ARCH-05): The probe reads at 128MB intervals from 0x40000000+1GB.
 *   On QEMU virt this is safe because physical RAM is at 0x40000000 and the
 *   machine model aborts reads above the configured RAM ceiling.  On real
 *   hardware some addresses in the range may be device registers; a read can
 *   cause an unintended side-effect before the abort arrives.
 */
struct mem_region *arch_platform_get_mem_regions(size_t *count) {
    if (arch_region_count > 0) {
        /* Cached from a previous call; return immediately. */
        if (count) *count = arch_region_count;
        return arch_mem_regions;
    }

    if (fdt_get_mem_regions(arch_mem_regions, MAX_CPUS * 2, &arch_region_count) == 0 && arch_region_count > 0) {
        /* FDT parse succeeded; log each discovered region. */
        for (size_t i = 0; i < arch_region_count; i++) {
            pr_info("AArch64: IDTF RAM Region [%zu]: 0x%lx - 0x%lx\n",
                    i, arch_mem_regions[i].base, arch_mem_regions[i].base + arch_mem_regions[i].size);
        }
    } else {
        extern volatile bool probe_in_progress;
        extern volatile bool probe_failed;

        pr_warn("%s", "AArch64: [IDTF] Memory discovery failed! Attempting manual page-by-page probe...\n");

        /* Manual Probe Fallback: scan physical RAM in 128MB steps.
         * QEMU virt places RAM at 0x40000000; probe_base skips the kernel's 1GB. */
        uint64_t probe_base  = 0x40000000UL;
        uint64_t probe_limit = 0x40000000UL + (128UL << 30); /* cap at 128GB total */
        uint64_t found_end   = probe_base;

        for (uint64_t addr = probe_base + (1UL << 30); addr < probe_limit; addr += 0x08000000UL) {
            /* Signal sync_handler to recover from an abort at this address. */
            probe_in_progress = true;
            probe_failed = false;
            arch_mb(); /* DSB: ensure flags visible before the load */

            /* Issue the probe load; if addr is unmapped, a Data Abort fires.
             * sync_handler sets probe_failed=true and increments ELR_EL1 by 4
             * to skip this instruction, then returns to the instruction below. */
            /* 'addr' is a PHYSICAL address; probe through its direct-map
             * VA (identity while KERNEL_VIRT_BASE == 0). */
            volatile uint64_t *ptr = (volatile uint64_t *)phys_to_virt(addr);
            (void)*ptr;

            arch_mb(); /* DSB: ensure probe_failed is current after handler runs */
            if (probe_failed) {
                probe_in_progress = false;
                break; /* addr is unmapped; stop probing */
            }

            probe_in_progress = false;
            found_end = addr; /* addr is mapped; extend found_end */
        }

        /* NOTE(ARCH-04): Dead write — immediately overwritten by lines 91–94 below.
         * The intent was to save the result before applying the PAGE_SIZE safety trim.
         * In practice the second write clobbers this unconditionally. */
        if (found_end > probe_base) {
            arch_mem_regions[0].base = probe_base;
            arch_mem_regions[0].size = found_end - probe_base;
            arch_mem_regions[0].type = MEM_REGION_USABLE;
            arch_region_count = 1;
            pr_info("AArch64: Manual discovery found %lu MB RAM\n", arch_mem_regions[0].size / 1024 / 1024);
        }

        /* Effective write (NOTE(ARCH-04)): subtract PAGE_SIZE to avoid mapping the
         * last probed page which may not be fully present.  If found_end == probe_base
         * (probe found nothing), size is set to 0. */
        arch_mem_regions[0].base = probe_base;
        arch_mem_regions[0].size = (found_end > probe_base) ? (found_end - probe_base - PAGE_SIZE) : 0;
        arch_mem_regions[0].type = MEM_REGION_USABLE;
        arch_region_count = 1;

        pr_info("AArch64: Manual probe discovered %lu MB RAM\n", arch_mem_regions[0].size / (1024 * 1024));
    }

    if (count) *count = arch_region_count;
    return arch_mem_regions;
}
extern volatile uint32_t cpu_boot_ack;
extern void *arch_secondary_stacks[MAX_CPUS];
extern void kernel_secondary_main(void);
extern void smp_create_idle_task(uint32_t cpu_id);

/*
 * arch_smp_init - wake and synchronise all secondary CPUs.
 *
 * Called by kernel_main() after the primary CPU's scheduler is ready.
 *
 * Sequence:
 *   1. Publish the current TTBR0_EL1 (kernel PGD) to secondary_ttbr0 via
 *      arch_vmm_set_secondary_pgd().  Secondaries read this in secondary_startup
 *      (start.S:159-161) before enabling their MMU.  The cache clean in
 *      arch_vmm_set_secondary_pgd ensures secondaries see the correct value.
 *   2. Query FDT for CPU count (fdt_count_cpus).  Falls back to MAX_CPUS if
 *      FDT reports 0 (e.g., no FDT or FDT without cpu nodes).
 *   3. Pre-allocate kernel stacks for all CPUs via arch_smp_setup_stacks().
 *      This must be done before any CPU is woken to avoid races on the stack
 *      array.
 *   4. For each secondary CPU i (1 .. cpu_count-1):
 *      a. Obtain or allocate the kernel stack top for CPU i.
 *      b. Call arch_cpu_wake_secondary(i, kernel_secondary_main, stack):
 *         Issues PSCI CPU_ON HVC (start.S:arch_cpu_wake_secondary) to ask the
 *         firmware to start CPU i at secondary_startup with stack as context_id.
 *      c. If wake succeeds (ret==0): create an idle task for the new CPU
 *         (smp_create_idle_task), then spin-wait on cpu_boot_ack == i with a
 *         10M-iteration timeout.  cpu_boot_ack is set by kernel_secondary_main
 *         after the secondary CPU completes its arch_cpu_init().
 *      d. On timeout or wake failure: free any dynamically allocated stack and
 *         stop (break) — do not attempt further CPUs.
 *
 * cpu_boot_ack is a volatile uint32_t written by the secondary CPU; the primary
 * CPU spins with arch_nop() (which typically maps to a WFE-like instruction or
 * NOP) to yield the pipeline between checks.
 *
 * NOTE: The spin-wait does not use a DSB before reading cpu_boot_ack; on weakly-
 * ordered hardware the secondary CPU's write to cpu_boot_ack may not be visible
 * without a load-acquire or explicit barrier on the primary side.  On QEMU this
 * works in practice because the HVC and the memory writes are serialised through
 * the emulator. [static, not verified on real hardware]
 */
void arch_smp_init(void) {
    /* Publish the KERNEL PGD (TTBR1 root) before waking any secondary CPU.
     * secondary_startup (start.S) reads secondary_ttbr1 and loads it into
     * TTBR1_EL1; TTBR0 (user half) gets the boot identity tables. */
    uint64_t current_pgd = arch_vmm_get_kernel_pgd();
    arch_vmm_set_secondary_pgd(current_pgd);

    /* Discover CPU count from FDT; fall back to a small sane cap if unavailable.
     * Using MAX_CPUS (64) as a fallback would allocate 56 wasted dynamic stacks
     * and attempt PSCI CPU_ON for 60 non-existent cores.  Instead, cap at 8 CPUs
     * (the maximum supported by the pre-allocated BSS stacks in start.S), which
     * avoids any dynamic stack allocation for the fallback path.  If FDT is
     * working correctly (x0 set by QEMU -dtb), this branch is never taken. */
#define AARCH64_FALLBACK_CPU_CAP 8
    uint32_t cpu_count = fdt_count_cpus();
    if (cpu_count == 0) {
        pr_warn("AArch64: FDT CPU discovery failed, capping at %d cores (fallback)\n",
                AARCH64_FALLBACK_CPU_CAP);
        cpu_count = AARCH64_FALLBACK_CPU_CAP;
    }
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    pr_info("AArch64: Starting SMP initialization for %u potential cores\n", cpu_count);

    /* Pre-allocate kernel stacks for all CPUs before waking any secondary.
     * This prevents race conditions on arch_secondary_stacks[] between CPUs. */
    arch_smp_setup_stacks(cpu_count);

    for (uint32_t i = 1; i < cpu_count; i++) {
        void *stack = NULL;
        bool allocated = false;

        if (i < 8) {
            /* CPUs 0..7: use pre-allocated BSS stack slices. */
            stack = arch_get_kernel_stack(i);
        } else {
            /* CPUs 8+: allocate a dynamic 128KB stack from PMM. */
            void *ptr = pmm_alloc_pages(131072 / 4096);
            if (!ptr) {
                pr_err("AArch64: Failed to allocate stack for CPU %d\n", i);
                break;
            }
            /* Stack grows downward; pass the TOP (highest address). */
            stack = (void *)((uintptr_t)ptr + 131072);
            arch_secondary_stacks[i] = stack;
            allocated = true;
        }

        /* SMP-IDLE-RACE (#169/#170): publish the idle task BEFORE waking the AP.
         * The AP enables IRQs in kernel_secondary_main and is preempted into
         * schedule() — which picks cpu_data[i].idle_task — possibly before we'd
         * otherwise create it (it was created only after the wake here), leaving
         * a NULL idle_task -> context switch into NULL. Create it first. */
        smp_create_idle_task(i);

        /* Issue PSCI CPU_ON for CPU i.  The entry point is secondary_startup
         * (start.S); stack is passed as context_id (x0 on CPU startup). */
        int ret = arch_cpu_wake_secondary(i, (void (*)(void))kernel_secondary_main, stack);

        if (ret == 0) {
            /* Spin-wait for secondary to write cpu_boot_ack = i.
             * Timeout = 10M nops (~tens of milliseconds on a GHz CPU). */
            volatile uint32_t timeout = 10000000;
            while (cpu_boot_ack != i && timeout > 0) {
                timeout--;
                arch_nop();
            }
            if (timeout == 0) {
                pr_warn("AArch64: CPU %d failed to acknowledge boot (timeout)\n", i);
                if (allocated) {
                    /* Reclaim the dynamic stack; ptr = top - 131072. */
                    pmm_free_pages((void *)((uintptr_t)stack - 131072), 32);
                    arch_secondary_stacks[i] = 0;
                }
            } else {
                pr_info("AArch64: CPU %d online\n", i);
            }
        } else {
            /* PSCI CPU_ON failed (e.g., CPU not present or firmware error). */
            if (allocated) {
                pmm_free_pages((void *)((uintptr_t)stack - 131072), 32);
                arch_secondary_stacks[i] = 0;
            }
            break; /* assume remaining CPUs are also absent */
        }
    }
}
