/*
 * kernel/HAL/arch/aarch64/platform.c
 * Platform hardware discovery and SMP bring-up for AArch64 (QEMU virt machine).
 *
 * Role:
 *   This file implements the arch_platform_* interface declared in
 *   kernel/include/kernel/platform.h for the AArch64 target.  It is responsible
 *   for three tasks:
 *   1. Early init (arch_platform_early_init): register the GIC interrupt
 *      controller. kernel_stage_k1_hardware validates the FDT after cpu_init.
 *   2. Memory discovery (arch_platform_get_mem_regions): obtain a list of usable
 *      RAM regions for the PMM. Primary path: bounded FDT memory-node parse.
 *      Fallback: QEMU-virt-only page-by-page probe through the exact-LDR
 *      synchronous Data Abort fixup in cpu.c.
 *   3. SMP bring-up (arch_smp_init): wake secondary CPUs via PSCI CPU_ON
 *      (arch_cpu_wake_secondary, start.S), wait for each to acknowledge, and
 *      create an idle task for each newly-online CPU.
 *
 * Memory discovery invariants:
 *   - The FDT parser is the primary source and returns usable ranges plus
 *     RESERVED overlays (reservation map, /reserved-memory, DTB allocation).
 *   - Only an explicit NO_MEMORY result permits the QEMU-virt legacy probe.
 *     Malformed/unsupported reservation data fails closed.
 *   - The legacy probe begins at HAL_RAM_START and tests each 4 KiB page.
 *     It is not a generic physical-hardware probe and must not be reused by a
 *     different platform without a platform-owned safe RAM window.
 *     The 0x00000000..HAL_RAM_START gap is deliberately outside the PMM span.
 *   - Probe abort recovery is tied to the exact LDR instruction address in
 *     cpu.c; unrelated kernel aborts are never swallowed.
 */
#include <hal/diagnostic_protocol.h>
#include <hal/core_protocol.h>
#include <arch/pt_regs.h>
#include <hal/core.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/fdt.h>
#include <kernel/memlayout.h>
#include <kernel/pmm.h>
#include <kernel/platform.h>
#include <drivers/gic.h>
#include <drivers/uart.h>

/*
 * Static early-boot region storage. It includes usable RAM and reserved
 * overlays, so its capacity is independent of CPU count.
 */
#define ARCH_MEM_REGION_CAPACITY ARCH_MAX_MEM_REGIONS
static struct mem_region arch_mem_regions[ARCH_MEM_REGION_CAPACITY];
static size_t arch_region_count = 0;
/* 0 = not initialized, 1 = valid cached map, -1 = terminal discovery failure. */
static int arch_region_state = 0;

/*
 * arch_platform_early_init - register platform interrupt-controller ops.
 * FDT validation has already completed in kernel_stage_k1_hardware(), after
 * cpu_init() installed the exception vectors. Do not reinitialize or discard
 * the validated DTB view here.
 */
void arch_platform_early_init(void) {
    uart_puts("PLATFORM: arch_platform_early_init starting\n");
    /* GIC registration is independent of the already-validated FDT view. */
    gic_register();

    if (boot_fdt_ptr != 0 && boot_fdt_size != 0) {
        nxhal_diag_info("AArch64: validated FDT at 0x%lx size=0x%lx\n",
                boot_fdt_ptr, boot_fdt_size);
    } else {
        nxhal_diag_warn("%s", "AArch64: no validated FDT handoff record is available\n");
    }
}
/*
 * The FDT is authoritative. A missing /memory node is the only condition that
 * permits the legacy QEMU-virt RAM probe; malformed reservations, unsupported
 * dynamic reserved-memory requests, and capacity errors fail closed.
 *
 * The legacy probe is deliberately limited to this QEMU virt platform. It
 * walks every 4 KiB page beginning at HAL_RAM_START (not RAM_BASE + 1 GiB),
 * and records the end as the first page that raises a synchronous Data Abort.
 * The kernel image is included in the discovered RAM interval and is reserved
 * later by PMM. The physical hole below HAL_RAM_START is never counted.
 */
struct mem_region *arch_platform_get_mem_regions(size_t *count) {
    if (arch_region_state != 0) {
        if (count)
            *count = arch_region_count;
        return arch_region_state > 0 ? arch_mem_regions : NULL;
    }

    size_t discovered_count = 0;
    int fdt_status = fdt_get_mem_regions(arch_mem_regions,
                                         ARCH_MEM_REGION_CAPACITY,
                                         &discovered_count);

    if (fdt_status == FDT_PARSER_OK && discovered_count > 0) {
        for (size_t i = 0; i < discovered_count; i++) {
            if (arch_mem_regions[i].type != MEM_REGION_USABLE)
                continue;
            if (arch_mem_regions[i].base < HAL_RAM_START ||
                arch_mem_regions[i].base >
                    UINT64_MAX - arch_mem_regions[i].size ||
                arch_mem_regions[i].base + arch_mem_regions[i].size >
                    HAL_PHYS_MAP_LIMIT) {
                nxhal_diag_error("AArch64: FDT RAM region[%zu] is outside the mapped "
                       "physical window: base=0x%lx size=0x%lx\n",
                       i, arch_mem_regions[i].base, arch_mem_regions[i].size);
                arch_region_count = 0;
                arch_region_state = -1;
                if (count)
                    *count = 0;
                return NULL;
            }
        }
        arch_region_count = discovered_count;
        arch_region_state = 1;
        for (size_t i = 0; i < arch_region_count; i++) {
            uint64_t end = arch_mem_regions[i].base +
                           arch_mem_regions[i].size;
            nxhal_diag_info("AArch64: FDT region [%zu] type=%u 0x%lx-0x%lx size=0x%lx\n",
                    i, arch_mem_regions[i].type,
                    arch_mem_regions[i].base, end,
                    arch_mem_regions[i].size);
        }
    } else if (fdt_status == FDT_PARSER_NO_MEMORY) {
        /* Reserved entries (including the DTB itself) remain in the array. */
        nxhal_diag_warn("AArch64: FDT has no enabled memory node; "
                "retaining %zu reservations and probing QEMU RAM page by page\n",
                discovered_count);

        const uint64_t probe_base = (uint64_t)HAL_RAM_START;
        const uint64_t probe_limit = HAL_PHYS_MAP_LIMIT;
        uint64_t found_end = probe_base;
        uint64_t fault_address = probe_limit;

        for (uint64_t address = probe_base;
             address < probe_limit;
             address += PAGE_SIZE) {
            uint32_t probe_value = 0;
            if (arch_platform_probe_read32(address, &probe_value) != 0) {
                fault_address = address;
                break;
            }

            found_end = address + PAGE_SIZE;
            if (((address - probe_base) &
                 ((256ULL * 1024ULL * 1024ULL) - 1ULL)) == 0 &&
                address != probe_base) {
                nxhal_diag_info("AArch64: RAM page probe reached 0x%lx\n",
                        address);
            }
        }

        if (found_end <= probe_base ||
            discovered_count >= ARCH_MEM_REGION_CAPACITY ||
            boot_fdt_ptr < probe_base || boot_fdt_size == 0 ||
            boot_fdt_ptr > UINT64_MAX - boot_fdt_size ||
            boot_fdt_ptr + boot_fdt_size > found_end) {
            nxhal_diag_error("AArch64: page-by-page RAM probe failed at 0x%lx\n",
                   fault_address);
            arch_region_count = 0;
            arch_region_state = -1;
        } else {
            arch_mem_regions[discovered_count].base = probe_base;
            arch_mem_regions[discovered_count].size = found_end - probe_base;
            arch_mem_regions[discovered_count].type = MEM_REGION_USABLE;
            arch_region_count = discovered_count + 1U;
            arch_region_state = 1;
            nxhal_diag_info("AArch64: page probe RAM [0x%lx,0x%lx) size=0x%lx "
                    "(%llu MiB); first absent page=0x%lx\n",
                    probe_base, found_end, found_end - probe_base,
                    (found_end - probe_base) / (1024ULL * 1024ULL),
                    fault_address);
        }
    } else {
        nxhal_diag_error("AArch64: refusing unsafe RAM fallback; FDT region parse status=%d\n",
               fdt_status);
        arch_region_count = 0;
        arch_region_state = -1;
    }

    if (count)
        *count = arch_region_count;
    return arch_region_state > 0 ? arch_mem_regions : NULL;
}
extern void *arch_secondary_stacks[MAX_CPUS];

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
 *   2. Query FDT for CPU count (fdt_count_cpus).  Falls back to a small cap if
 *      FDT reports 0 (e.g., no FDT or FDT without cpu nodes).
 *   3. Pre-allocate kernel stacks for all CPUs via arch_smp_setup_stacks().
 *      This must be done before any CPU is woken to avoid races on the stack
 *      array.
 *   4. For each secondary CPU i (1 .. cpu_count-1): obtain/allocate its stack
 *      top, then hand off to smp_bringup_secondary() (kernel/core/smp.c) —
 *      the shared arch-neutral primitive that owns the idle-task-first
 *      invariant (#169/#170), the PSCI wake via arch_cpu_wake_secondary, and
 *      the acquire/release ack handshake with a wall-time timeout.  Only the
 *      per-arch failure POLICY lives here (PSCI failure = remaining CPUs
 *      absent -> break; ack timeout = skip this CPU, free its dynamic stack).
 */
void arch_smp_init(void) {
    /* Publish the KERNEL PGD (TTBR1 root) before waking any secondary CPU.
     * secondary_startup (start.S) reads secondary_ttbr1 and loads it into
     * TTBR1_EL1; TTBR0 (user half) gets the boot identity tables. */
    uint64_t current_pgd = hal_vmm_get_kernel_pgd();
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
        nxhal_diag_warn("AArch64: FDT CPU discovery failed, capping at %d cores (fallback)\n",
                AARCH64_FALLBACK_CPU_CAP);
        cpu_count = AARCH64_FALLBACK_CPU_CAP;
    }
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    nxhal_diag_info("AArch64: Starting SMP initialization for %u potential cores\n", cpu_count);

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
                nxhal_diag_error("AArch64: Failed to allocate stack for CPU %d\n", i);
                break;
            }
            /* Stack grows downward; pass the TOP (highest address). */
            stack = (void *)((uintptr_t)ptr + 131072);
            arch_secondary_stacks[i] = stack;
            allocated = true;
        }

        /* Shared bring-up primitive (S-ALIGN F8, B6 slice): idle-task-first
         * (#169/#170) + PSCI CPU_ON + bounded acquire-wait for the ack all
         * live in kernel/core/smp.c.  Only the failure POLICY stays here:
         * a PSCI failure means the remaining CPUs are absent too (break),
         * an ack timeout skips just this CPU. */
        int ret = smp_bringup_secondary(
            i, (void (*)(void))nxhal_core_secondary_entry, stack);

        if (ret == 0) {
            nxhal_diag_info("AArch64: CPU %d online\n", i);
        } else if (ret == -2) {
            nxhal_diag_warn("AArch64: CPU %d failed to acknowledge boot (timeout)\n", i);
            if (allocated) {
                /* Reclaim the dynamic stack; ptr = top - 131072. */
                pmm_free_pages((void *)((uintptr_t)stack - 131072), 32);
                arch_secondary_stacks[i] = 0;
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
