/*
 * kernel/HAL/arch/aarch64/boot/start.S
 * Kernel entry point — higher-half bring-up.
 *
 * The kernel is LINKED at 0xFFFF000040080000 (KERNEL_VIRT_BASE + load PA,
 * see memlayout.h / kernel.ld) but LOADED and ENTERED at physical
 * 0x40080000 with the MMU off.  Everything in this file up to the "br"
 * into the higher half is position-independent (adrp/PC-relative), so it
 * runs correctly at the load address: adrp resolves link-relative, which
 * equals the PHYSICAL address while PC is low and the VIRTUAL address
 * once PC is high.
 *
 * Boot page tables (built here, in BSS, before the MMU is enabled):
 *   boot_pt_l1      one shared L1 table:
 *                     [0]      1GB Device block  (PA 0: UART/GIC/VirtIO)
 *                     [1..127] 1GB Normal blocks (PA 1GB..128GB: covers all
 *                              possible RAM; the platform RAM probe relies
 *                              on mapped-but-unbacked PAs still faulting)
 *   boot_pgd_ttbr0  L0 with [0] -> boot_pt_l1   (identity, for this file)
 *   boot_pgd_ttbr1  L0 with [0] -> boot_pt_l1   (higher half: VA-base+PA)
 *
 * After SCTLR.M is set the code jumps (absolute literal) to the link
 * address, moves SP to the linked stack and calls kernel_main.  C code
 * never runs identity-mapped.  vmm_init/vmm_dynamic_remap later replace
 * TTBR1 with the real kernel PGD (W^X, precise regions); TTBR0 keeps the
 * boot identity tables until the first process switch loads a user PGD.
 */

#include <kernel/cpu.h>
#include <hal/boot_info.h>

/*
 * Linux ARM64 Image header consumed by GRUB's `linux` command.
 * This is only a boot protocol: the payload remains the NexsOS kernel.
 * text_offset=0x80000 matches the current QEMU virt DRAM base 0x40000000.
 * flags=2 declares little-endian + 4KB granule, with the image placed near
 * the beginning of DRAM. The linker resolves image_size including BSS.
 */
.section .linux_header, "ax"
.balign 8
.global __arm64_image_header
__arm64_image_header:
    b _start
    nop
    .quad 0x80000
    .quad __kernel_size
    .quad 0x2
    .quad 0
    .quad 0
    .quad 0
    .word 0x644d5241
    .word 0
.if (. - __arm64_image_header) != 64
.error "AArch64 Image header must be exactly 64 bytes"
.endif

/* Keep in sync with KERNEL_VIRT_BASE in kernel/include/kernel/memlayout.h */
#define KVB 0xFFFF000000000000

/* TCR_EL1: T0SZ=16, T1SZ=16 (48-bit both halves), 4KB granules
 * (TG0=0, TG1=2), inner WB/WA + inner-shareable walks on both halves,
 * IPS=2 (40-bit PA — RAM tops out below 1TB on QEMU virt).
 * AS=1 (bit 36): 16-bit ASID — TTBR0_EL1[63:48] tags each address space for
 *   flush-free context switches (perf §3).  A1=0 (TTBR0 defines the ASID). */
#define BOOT_TCR 0x12B5103510

/* Block descriptors: VALID | AF | attrs (level-1 1GB blocks, bit1 = 0). */
#define BLK_DEVICE 0x405       /* AttrIndx=1 (Device nGnRE), AF */
#define BLK_NORMAL 0x701       /* AttrIndx=0 (Normal WB), SH=inner, AF */

.section .text.boot
.global _start

/*
 * Kernel entry point
 * Called from a bootloader with:
 *   x0 = DTB physical address
 *   x1 = DTB available span from the NexsOS EFI stub; zero for legacy Image
 *        loaders that only provide the DTB address.
 *
 * The x1 length extension is part of the NexsOS handoff contract. GRUB's
 * legacy ARM64 Image path continues to pass zero and uses fdt_init_legacy().
 */
_start:
    /* Preserve the architecture-neutral NX-HAL handoff before setup. */
    mov x20, x0
    mov x21, x1
    mov x22, x2
    mov x23, x3
    mov x24, x4
    adrp x5, nxhal_boot_info
    add x5, x5, :lo12:nxhal_boot_info
    str x20, [x5, #NXHAL_BOOT_INFO_FDT_PHYS_OFFSET]
    str x21, [x5, #NXHAL_BOOT_INFO_FDT_SIZE_OFFSET]
    cbz x21, .Lboot_handoff_saved
    str x22, [x5, #NXHAL_BOOT_INFO_FW_MAP_PHYS_OFFSET]
    str w23, [x5, #NXHAL_BOOT_INFO_FW_MAP_COUNT_OFFSET]
    mov w6, #NXHAL_UEFI_MEMORY_RECORD_SIZE
    str w6, [x5, #NXHAL_BOOT_INFO_FW_MAP_RECORD_SIZE_OFFSET]
    str w24, [x5, #NXHAL_BOOT_INFO_EBS_ATTEMPTS_OFFSET]
    lsr x6, x24, #32
    str w6, [x5, #NXHAL_BOOT_INFO_MAP_GROWTHS_OFFSET]
.Lboot_handoff_saved:

    /* Disable interrupts */
    msr daifset, #0xf

    /* Get CPU ID */
    mrs x1, mpidr_el1
    and x1, x1, #0xff

    /* CPU 0 does primary init */
    cbz x1, primary_init

    /* Secondary CPUs wait (PSCI handles this, but if we get here, hang) */
    b halt

/*
 * Primary CPU initialization
 */
primary_init:
    adrp x0, __kernel_stack
    add x0, x0, :lo12:__kernel_stack
    add x0, x0, #131072    /* Use first 128KB slice for CPU 0 */
    mov sp, x0

    /* Switch to SP_EL1 */
    msr spsel, #1
    /* Re-set SP for EL1 just in case */
    mov sp, x0

    /* Check current exception level */
    mrs x0, CurrentEL
    lsr x0, x0, #2      /* Extract EL bits */

    cmp x0, #3
    beq el3_setup
    cmp x0, #2
    beq el2_setup
    /* Already at EL1, continue */
    b el1_setup

/*
 * EL3 setup (if started by firmware at EL3)
 */
el3_setup:
    /* Configure EL3 to drop to EL1 */
    mov x0, #0x531          /* SCR_EL3: RW=1, NS=1 (EL1 AArch64, non-secure) */
    msr scr_el3, x0

    mov x0, #0x3c5          /* SPSR: EL1h, all interrupts masked */
    msr spsr_el3, x0

    adrp x0, el1_setup
    add x0, x0, :lo12:el1_setup
    msr elr_el3, x0
    eret

/*
 * EL2 setup (if started by hypervisor at EL2)
 */
el2_setup:
    /* Enable EL1 access to timers */
    mrs x0, cnthctl_el2
    orr x0, x0, #3          /* EL1PCEN, EL1PCTEN */
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr    /* Virtual timer offset = 0 */

    /* Configure HCR_EL2 */
    mov x0, #(1 << 31)      /* RW bit: EL1 uses AArch64 */
    msr hcr_el2, x0

    /* SPSR_EL2: Return to EL1h with interrupts masked */
    mov x0, #0x3c5
    msr spsr_el2, x0

    adrp x0, el1_setup
    add x0, x0, :lo12:el1_setup
    msr elr_el2, x0
    eret

/*
 * EL1 setup: FPU, BSS, boot page tables, MMU on, jump to the higher half.
 */
el1_setup:
    /* Temporary physical SP (nothing is pushed before the MMU jump) */
    adrp x0, __kernel_stack_top
    add x0, x0, :lo12:__kernel_stack_top
    mov sp, x0

    /* Enable FPU/SIMD (NEON) before anything else */
    /* Set CPACR_EL1.FPEN = 0b11 to enable FP/SIMD at EL0 and EL1 */
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    /* Clear BSS (adrp yields physical addresses while PC is low) */
    adrp x0, __bss_start
    add x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add x1, x1, :lo12:__bss_end
clear_bss:
    cmp x0, x1
    bge bss_done
    str xzr, [x0], #8
    b clear_bss
bss_done:

    /*
     * Build the boot page tables (they live in BSS, just cleared).
     * x1 = PA(boot_pt_l1)
     */
    adrp x1, boot_pt_l1
    add x1, x1, :lo12:boot_pt_l1

    /* L1[0]: 1GB Device block at PA 0 (UART/GIC/VirtIO window) */
    mov x2, #BLK_DEVICE
    str x2, [x1]

    /* L1[1..127]: 1GB Normal blocks covering PA 1GB..128GB */
    mov x3, #1
    ldr x4, =BLK_NORMAL
1:  lsl x2, x3, #30
    orr x2, x2, x4
    str x2, [x1, x3, lsl #3]
    add x3, x3, #1
    cmp x3, #128
    b.lt 1b

    /* boot_pgd_ttbr0[0] = boot_pgd_ttbr1[0] = L1 | TABLE | VALID */
    orr x2, x1, #0x3
    adrp x0, boot_pgd_ttbr0
    add x0, x0, :lo12:boot_pgd_ttbr0
    str x2, [x0]
    adrp x5, boot_pgd_ttbr1
    add x5, x5, :lo12:boot_pgd_ttbr1
    str x2, [x5]

    /* MAIR: Attr0 = Normal WB/WA (0xFF), Attr1 = Device nGnRE (0x04) */
    mov x2, #0xff
    orr x2, x2, #(0x04 << 8)
    msr mair_el1, x2

    /* TCR: 48-bit VA on both halves, 4KB granules, IPS=40-bit */
    ldr x2, =BOOT_TCR
    msr tcr_el1, x2

    /* TTBR0 = identity L0, TTBR1 = higher-half L0 (same L1 underneath) */
    msr ttbr0_el1, x0
    msr ttbr1_el1, x5

    tlbi vmalle1
    dsb ish
    isb

    /* Enable MMU + D/I caches.  RES1 bits per ARM DDI 0487 D5.2.88;
     * A/SA (alignment checks) stay off, matching the previous C bring-up. */
    mrs x2, sctlr_el1
    orr x2, x2, #(1 << 29)
    orr x2, x2, #(1 << 28)
    orr x2, x2, #(1 << 23)
    orr x2, x2, #(1 << 22)
    orr x2, x2, #(1 << 20)
    orr x2, x2, #(1 << 11)
    orr x2, x2, #(1 << 0)   /* M */
    orr x2, x2, #(1 << 2)   /* C */
    orr x2, x2, #(1 << 12)  /* I */
    bic x2, x2, #(1 << 1)   /* A */
    bic x2, x2, #(1 << 3)   /* SA */
    dsb ish
    msr sctlr_el1, x2
    isb

    /* Jump to the link address (literal = virtual/high) */
    ldr x9, =boot_high_entry
    br x9

boot_high_entry:
    /* PC is now in the higher half; adrp yields virtual addresses */
    adrp x0, __kernel_stack_top
    add x0, x0, :lo12:__kernel_stack_top
    mov sp, x0

    /* Reload the physical DTB base and available span from BootInfo. */
    adrp x9, nxhal_boot_info
    add x9, x9, :lo12:nxhal_boot_info
    ldr x0, [x9, #NXHAL_BOOT_INFO_FDT_PHYS_OFFSET]
    ldr x1, [x9, #NXHAL_BOOT_INFO_FDT_SIZE_OFFSET]
    bl nxcore_hal_boot_entry

    /* Should never return */
halt:
    wfe
    b halt

/*
 * Secondary CPU startup (PSCI Entry Point) — entered at PA, MMU off.
 * x0 = context_id = kernel stack top (VIRTUAL, used only after MMU on)
 *
 * Re-entrancy note (companion to the amd64 trampoline hardening in
 * trampoline.S/platform.c): amd64's AP bring-up needed an explicit
 * software re-entrancy guard because a second STARTUP IPI can physically
 * re-enter a SHARED, statically-addressed scratch page while an earlier
 * pass is still mid-flight through its 16->32->64-bit transition — the
 * kernel itself owns and re-triggers that delivery mechanism.  PSCI
 * CPU_ON, used here, has no equivalent risk: it is a single firmware call
 * per CPU (see arch_impl_smp_wake_secondary() below — called exactly once per
 * CPU by smp_bringup_secondary(), kernel/core/smp.c, with no retry), and
 * the PSCI specification defines CPU_ON as idempotent at the FIRMWARE
 * level — calling it again on a core that is already ON (or coming ON)
 * returns ALREADY_ON/ON_PENDING and does not re-invoke this entry point.
 * There is also no shared scratch page here to race over: each CPU's
 * context_id (x0, its own stack pointer) arrives as a PSCI call argument,
 * not as a value patched into common memory ahead of time.  A software
 * busy-flag guard would therefore be protecting against a class of
 * re-entry this path cannot experience, not genuine defense-in-depth.
 */
secondary_startup:
    mov sp, x0              /* high VA; untouched until the MMU is on */

    /* 1. TTBR1 = kernel PGD (physical, published by the primary CPU) */
    adrp x4, secondary_ttbr1
    add x4, x4, :lo12:secondary_ttbr1
    ldr x4, [x4]
    msr ttbr1_el1, x4

    /* TTBR0 = boot identity tables (defined state until a process loads) */
    adrp x5, boot_pgd_ttbr0
    add x5, x5, :lo12:boot_pgd_ttbr0
    msr ttbr0_el1, x5

    /* 2. MAIR (Attr0=Normal, Attr1=Device) — must match the primary CPU */
    mov x4, #0xff
    orr x4, x4, #(0x04 << 8)
    msr mair_el1, x4

    /* 3. TCR — same 48/48 split as the primary CPU */
    ldr x4, =BOOT_TCR
    msr tcr_el1, x4
    isb

    /* 4. SCTLR_EL1 (Enable M, C, I) */
    mrs x4, sctlr_el1
    orr x4, x4, #(1 << 0)  /* M */
    orr x4, x4, #(1 << 2)  /* C */
    orr x4, x4, #(1 << 12) /* I */
    dsb ish
    msr sctlr_el1, x4
    isb

    /* Jump to C code (absolute virtual address via TTBR1) */
    ldr x3, =nxhal_core_secondary_entry
    br x3
    b halt

/*
 * Wake secondary CPU via PSCI
 * x0 = cpu id
 * x1 = stack pointer - passed as context_id
 */
.global arch_impl_smp_wake_secondary
arch_impl_smp_wake_secondary:
    mov x3, x1          /* x3 = context_id (stack, virtual) */

    /* PSCI needs the PHYSICAL entry point.  adrp at a high PC yields the
     * link (virtual) address — subtract KERNEL_VIRT_BASE. */
    adrp x2, secondary_startup
    add x2, x2, :lo12:secondary_startup
    ldr x4, =KVB
    sub x2, x2, x4

    mov x1, x0          /* x1 = target_cpu (MPIDR) */

    /* Load PSCI_CPU_ON_64 (0xC4000003) */
    mov x0, #0x0003
    movk x0, #0xC400, lsl #16

    hvc #0              /* Call PSCI via HVC */
    ret

/*
 * Get legacy boot argument (the stored value is the DTB PHYSICAL address).
 * The full typed BootInfo record is exported as the nxhal_boot_info data symbol.
 */
.global arch_get_boot_info
arch_get_boot_info:
    adrp x0, nxhal_boot_info
    add x0, x0, :lo12:nxhal_boot_info
    ldr x0, [x0]
    ret

/*
 * Data section
 */
.section .data
.balign 8

.global nxhal_boot_info
nxhal_boot_info:
    .quad 0                              /* fdt_phys */
    .quad 0                              /* fdt_size */
    .long NXHAL_BOOT_INFO_MAGIC
    .short NXHAL_BOOT_INFO_VERSION
    .short NXHAL_BOOT_INFO_SIZE
    .long NXHAL_BOOT_SOURCE_UNKNOWN
    .long 0                              /* flags */
    .quad 0                              /* firmware_map_phys */
    .long 0                              /* firmware_map_record_count */
    .long 0                              /* firmware_map_record_size */
    .long 0                              /* ebs_attempts */
    .long 0                              /* map_growths */
    .quad 0                              /* reserved0 */

cpu_spin_table:
    .skip MAX_CPUS * 16     /* MAX_CPUS entries */

cpu_stacks:
    .skip MAX_CPUS * 16     /* MAX_CPUS stack pointers */

/*
 * BSS section
 */
.section .bss
.balign 16

/* Per-CPU stacks (128KB each) */
.global __kernel_stack
__kernel_stack:
    .skip 131072 * MAX_CPUS     /* MAX_CPUS * 128KB */
.global __kernel_stack_top
__kernel_stack_top:

/* Boot page tables (cleared with BSS, then built by el1_setup) */
.balign 4096
boot_pgd_ttbr0:
    .skip 4096
.global boot_pgd_ttbr1
boot_pgd_ttbr1:
    .skip 4096
boot_pt_l1:
    .skip 4096

.section .data
.align 3
.global secondary_ttbr1
secondary_ttbr1: .quad 0