#include <kernel/cpu.h>

.section .multiboot, "a"
.align 8
multiboot2_header:
    .long 0xe85250d6        /* Multiboot 2 Magic */
    .long 0                 /* Architecture: i386 */
    .long multiboot2_header_end - multiboot2_header
    .long -(0xe85250d6 + 0 + (multiboot2_header_end - multiboot2_header))

    .align 8
    .short 3
    .short 0
    .long 12
    .long _start_32

    .align 8
    .short 0
    .short 0
    .long 8
multiboot2_header_end:

/* PVH Header */
.section .note.PVH, "a"
.align 4
    .long 4             /* Namesz */
    .long 4             /* Descsz */
    .long 18            /* Type (XEN_ELFNOTE_PHYS32_ENTRY) */
    .ascii "Xen\0"      /* Name */
    .long _start_32     /* Desc (Entry point) */

.section .text.boot, "ax"
.code32
.global _start_32
_start_32:
    /* Debug: Print 'B' to serial port 0x3f8 */
    movb $'B', %al
    movw $0x3f8, %dx
    outb %al, %dx

    cli
    cld

    /* 1. Setup minimal GDT for 32-bit mode */
    lgdt (boot_gdtr_32)
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* Save magic and info for later */
    movl %eax, %ebp
    movl %ebx, %edx

    /* 2. Setup temporary stack */
    movl $boot_stack_top, %esp

    /* 3. Clear Page Tables */
    movl $boot_pml4, %edi
    xorl %eax, %eax
    movl $4096, %ecx /* 4 pages * 1024 dwords each */
    rep stosl

    /* 4. Enable PAE */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    /* 5. Setup Page Tables (Identity map first 1GB) */
    movl $boot_pdpt, %eax
    orl $3, %eax
    movl %eax, (boot_pml4)
    movl $0, (boot_pml4 + 4)

    movl $boot_pd, %eax
    orl $3, %eax
    movl %eax, (boot_pdpt)
    movl $0, (boot_pdpt + 4)

    /* PDPT[3] = boot_pd_high (For 3GB-4GB) */
    movl $boot_pd_high, %eax
    orl $3, %eax
    movl %eax, (boot_pdpt + 24)
    movl $0, (boot_pdpt + 28)

    /* PD[0..511] = 0, 2MB, 4MB, ... 1GB */
    movl $boot_pd, %edi
    movl $0x83, %eax
    movl $512, %ecx
.fill_pd:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd

    /* PD_HIGH (3GB-4GB) */
    movl $boot_pd_high, %edi
    movl $0xC0000083, %eax
    movl $512, %ecx
.fill_pd_high:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd_high

    /* 6. Load PML4 */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* 7. Enable Long Mode */
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    /* 8. Enable Paging */
    movl %cr0, %eax
    orl $(1 << 31 | 1 << 16), %eax
    movl %eax, %cr0

    /* 9. Jump to 64-bit */
    /* Debug: Print 'C' */
    movb $'C', %al
    movw $0x3f8, %dx
    outb %al, %dx

    pushl $0x08
    pushl $_start_64
    lret

.code64
_start_64:
    /* Debug: Print 'E' */
    movb $'E', %al
    movw $0x3f8, %dx
    outb %al, %dx

    /* Segment registers to NULL */
    xorl %eax, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Debug: Print 'F' */
    movb $'F', %al
    movw $0x3f8, %dx
    outb %al, %dx

    /* Clear BSS */
    movabsq $__bss_start, %rdi
    movabsq $__bss_end, %rcx
    subq %rdi, %rcx
    jz .bss_done
    xorq %rax, %rax
    rep stosb
.bss_done:

    /* Debug: Print 'G' */
    movb $'G', %al
    movw $0x3f8, %dx
    outb %al, %dx

    movl %ebp, %eax
    movq %rax, mb_magic(%rip)
    movl %edx, %eax
    movq %rax, mb_info_ptr(%rip)

    movabsq $__kernel_stack, %rsp
    addq $131072, %rsp
    movq %rsp, %rbp

    lgdt (boot_gdtr_64)
    
    movq mb_info_ptr(%rip), %rdi
    movq mb_magic(%rip), %rsi
    call kernel_main

.halt:
    hlt
    jmp .halt

.global secondary_cpu_entry
secondary_cpu_entry:
    cli
    xorl %eax, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    call arch_cpu_init
    call kernel_secondary_main

.global arch_yield
arch_yield:
    pause
    ret

.global arch_impl_idle
arch_impl_idle:
    hlt
    ret

.section .data
.align 16
boot_gdt:
    .quad 0x0000000000000000 /* Null */
    .quad 0x00af9a000000ffff /* Kernel Code 64 */
    .quad 0x00cf92000000ffff /* Kernel Data 64 (Usable in 32-bit too) */
boot_gdt_end:

boot_gdtr_32:
    .word boot_gdt_end - boot_gdt - 1
    .long boot_gdt

boot_gdtr_64:
    .word boot_gdt_end - boot_gdt - 1
    .quad boot_gdt

.global mb_magic, mb_info_ptr
mb_magic:    .quad 0
mb_info_ptr: .quad 0

.global kernel_pgd_phys
kernel_pgd_phys: .quad boot_pml4

.section .bss
.align 4096
boot_stack:
    .skip 4096
boot_stack_top:

.align 4096
.global boot_pml4
boot_pml4: .skip 4096
.global boot_pdpt
boot_pdpt: .skip 4096
.global boot_pd
boot_pd:   .skip 4096
.global boot_pd_high
boot_pd_high: .skip 4096

.section .bss
.align 4096
.global __kernel_stack
__kernel_stack:
    .skip 131072 * MAX_CPUS
.global __kernel_stack_top
__kernel_stack_top:
