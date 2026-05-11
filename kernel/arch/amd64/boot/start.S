/*
 * kernel/arch/amd64/boot/start.S
 * AMD64 Kernel Entry Point - Self-booting via Multiboot2
 */

#define MB2_MAGIC        0xE85250D6
#define MB2_ARCH_I386    0

.section .multiboot
.align 8
mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long mb2_header_end - mb2_header_start
    .long -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start))

    /* Entry Address Tag (Type 5) */
    .short 5
    .short 0
    .long 12
    .long _start_32

    /* End Tag */
    .short 0
    .short 0
    .long 8
mb2_header_end:

.code32
.section .text.boot
.global _start_32
_start_32:
    /* Disable interrupts */
    cli

    /* Save Multiboot2 info */
    movl %ebx, (mb_info_ptr_phys)

    /* Transition to 64-bit Long Mode */
    /* 1. Setup minimal page tables (Identity map 1GB) */
    movl $boot_pml4, %edi
    xorl %eax, %eax
    movl $4096, %ecx
    rep stosl

    /* PML4[0] -> PDPT */
    movl $boot_pml4, %edi
    movl $boot_pdpt, %eax
    orl  $0x3, %eax
    movl %eax, (%edi)

    /* PDPT[0] -> PD (1GB block) */
    movl $boot_pdpt, %edi
    movl $0x83, %eax    /* Present + Write + 1GB PS (if supported) */
    /* For safety, use 2MB pages */
    movl $boot_pd, %eax
    orl  $0x3, %eax
    movl %eax, (%edi)

    /* PD -> 2MB Pages */
    movl $boot_pd, %edi
    movl $0x83, %eax
    movl $512, %ecx
.fill_pd:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd

    /* 2. Enable PAE */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* 3. Load PML4 */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* 4. Enable Long Mode */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* 5. Enable Paging */
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    /* 6. Jump to 64-bit code */
    lgdt (boot_gdtr_phys)
    ljmp $0x08, $_start_64

.code64
.global _start_64
_start_64:
    /* Reload segments */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Setup stack */
    movabsq $_stack_top, %rsp
    movq %rsp, %rbp

    /* Clear BSS */
    movabsq $_bss_start, %rdi
    movabsq $_bss_end, %rcx
    subq %rdi, %rcx
    shr $3, %rcx
    xorq %rax, %rax
    rep stosq

    /* Load kernel GDT */
    lgdt (boot_gdtr)

    /* Call kernel_main(mb_info_ptr) */
    movq (mb_info_ptr), %rdi
    call kernel_main

.halt:
    hlt
    jmp .halt

.section .data
.align 8
.global mb_info_ptr
mb_info_ptr:
mb_info_ptr_phys: .quad 0

/* Kernel GDT */
.align 16
boot_gdt:
    .quad 0x0000000000000000   /* Null */
    .quad 0x00AF9A000000FFFF   /* Kernel Code (64-bit, DPL=0) */
    .quad 0x00CF92000000FFFF   /* Kernel Data (DPL=0) */
    .quad 0x00CFF2000000FFFF   /* User Data (DPL=3) */
    .quad 0x00AFFA000000FFFF   /* User Code (DPL=3) */
boot_gdt_end:

.global boot_gdtr
boot_gdtr:
boot_gdtr_phys:
    .word boot_gdt_end - boot_gdt - 1
    .quad boot_gdt

.section .bss
.align 4096
.global boot_pml4
boot_pml4: .skip 4096
boot_pdpt: .skip 4096
boot_pd:   .skip 4096

/* Kernel stack */
.align 4096
.global _stack_bottom
_stack_bottom:
    .skip 0x20000   /* 128 KB */
.global _stack_top
_stack_top:
