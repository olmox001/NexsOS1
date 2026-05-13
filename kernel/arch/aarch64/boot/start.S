/*
 * kernel/arch/aarch64/boot/start.S
 * Kernel entry point
 */

#include <kernel/cpu.h>

.section .text.boot
.global _start

/*
 * Kernel entry point
 * Called from bootloader/QEMU with:
 *   x0 = DTB pointer
 *   x1 = 0 (reserved)
 *
 * QEMU virt machine starts kernel at EL1 directly
 */
_start:
    /* Save boot info (DTB pointer) immediately */
    mov x20, x0
    adrp x1, boot_info
    str x0, [x1, :lo12:boot_info]

    /* Disable interrupts */
    msr daifset, #0xf
    
    /* Get CPU ID */
    mrs x1, mpidr_el1
    and x1, x1, #0xff
    
    /* CPU 0 does primary init */
    cbz x1, primary_init
    
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
 * EL1 setup
 */
el1_setup:
    /* Reload stack (in case we came from ERET) */
    adrp x0, __kernel_stack_top
    add x0, x0, :lo12:__kernel_stack_top
    mov sp, x0
    
    /* Enable FPU/SIMD (NEON) before anything else */
    /* Set CPACR_EL1.FPEN = 0b11 to enable FP/SIMD at EL0 and EL1 */
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    /* Clear BSS */
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

    /* Configure SCTLR_EL1 */
    /* Read current value */
    mov x0, #0
    bic x0, x0, #(1 << 12)   /* I-cache DISABLE */
    bic x0, x0, #(1 << 2)    /* D-cache DISABLE (avoid MMIO cache issues without MMU) */
    bic x0, x0, #(1 << 0)    /* MMU disabled for now */
    msr sctlr_el1, x0
    isb
    
    /* Call kernel main */
    mov x0, x20
    bl kernel_main
    
    /* Should never return */
halt:
    wfe
    b halt

/*
 * Secondary CPU initialization
 */
/*
 * Secondary CPU startup (PSCI Entry Point)
 * x0 = context_id (Stack Pointer)
 */
secondary_startup:
    mov sp, x0
    
    /* 1. Load Kernel PGD from secondary_ttbr0 */
    /* Use adrp/add to get the physical address of secondary_ttbr0 */
    adrp x4, secondary_ttbr0
    add x4, x4, :lo12:secondary_ttbr0
    ldr x4, [x4]
    
    msr ttbr0_el1, x4
    
    /* 2. Setup MAIR (Attr0=Normal, Attr1=Device) */
    mov x4, #0xff
    orr x4, x4, #(0x04 << 8)
    msr mair_el1, x4
    
    /* 3. TCR_EL1 (40-bit PA, 48-bit VA, 4KB, Inner Shareable, WB/WA) */
    /* (16<<0) | (3<<12) | (1<<10) | (1<<8) | (2<<32) */
    mov x4, #0x3510
    movk x4, #0x0002, lsl #32 /* IPS=2 */
    msr tcr_el1, x4
    
    /* 4. SCTLR_EL1 (Enable M, C, I) */
    mrs x4, sctlr_el1
    orr x4, x4, #(1 << 0)  /* M */
    orr x4, x4, #(1 << 2)  /* C */
    orr x4, x4, #(1 << 12) /* I */
    msr sctlr_el1, x4
    
    dsb ish
    isb
    
    /* Jump to C code (Absolute Virtual Address) */
    ldr x3, =kernel_secondary_main
    br x3
    b halt

/*
 * Wake secondary CPU
 */
.global cpu_wake_secondary
/*
 * Wake secondary CPU via PSCI
 * x0 = cpu id
 * x1 = entry point (user C function) - ignored, we use secondary_startup
 * x2 = stack pointer - passed as context_id
 */
.global arch_cpu_wake_secondary
arch_cpu_wake_secondary:
    mov x3, x2          /* x3 = context_id (stack) */
    
    /* Calculate Physical Address of secondary_startup */
    adrp x2, secondary_startup
    add x2, x2, :lo12:secondary_startup
    
    mov x1, x0          /* x1 = target_cpu (MPIDR) */
    
    /* Load PSCI_CPU_ON_64 (0xC4000003) */
    mov x0, #0x0003
    movk x0, #0xC400, lsl #16
    
    hvc #0              /* Call PSCI via HVC */
    ret

/*
 * Get boot info pointer
 */
.global arch_get_boot_info
arch_get_boot_info:
    adrp x0, boot_info
    add x0, x0, :lo12:boot_info
    ldr x0, [x0]
    ret

/*
 * Data section
 */
.section .data
.balign 8

.global boot_info
boot_info:
    .quad 0

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

.section .data
.align 3
.global secondary_ttbr0
secondary_ttbr0: .quad 0
