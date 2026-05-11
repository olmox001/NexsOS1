/*
 * boot/amd64/stage2.S
 * AMD64 Bootloader - Stage 2 (64-bit)
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

    /* Pass magic and info pointer to stage2_main(magic, info) */
    movl (multiboot_magic), %edi
    movl (multiboot_info_ptr), %esi

    /* Call stage2 main logic */
    call stage2_main

    /* Should not return */
    cli
.halt:
    hlt
    jmp .halt



.section .bss
.align 16
__stage2_stack: .skip 16384
__stage2_stack_top:
