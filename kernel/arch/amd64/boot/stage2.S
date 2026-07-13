/*
 * boot/amd64/stage2.S
 * AMD64 Bootloader - Stage 2 (64-bit)
 * Parity with aarch64 implementation
 */

.code64
.section .text

.global stage2_entry_trampoline
stage2_entry_trampoline:
    /* Reload segments */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Setup Stage 2 Stack */
    movq $__stage2_stack_top, %rsp

    /* Setup kernel environment */
    call setup_kernel_environment
    
    /* Jump to kernel entry point */
    movq (kernel_entry_point), %rax
    testq %rax, %rax
    jz .halt
    
    /* Pass Multiboot2 info in RDI (standard for our kernel) */
    movl (multiboot_info_ptr), %edi
    jmp *%rax

.halt:
    cli
.halt_loop:
    hlt
    jmp .halt_loop

setup_kernel_environment:
    /* Minimal for now, parity with aarch64 */
    ret

.section .bss
.align 16
__stage2_stack: .skip 16384
__stage2_stack_top:

/* Symbols defined in stage1.S */
.extern kernel_entry_point
.extern multiboot_info_ptr
