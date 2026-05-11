/*
 * kernel/arch/amd64/boot/start.S
 * AMD64 Kernel Entry Point - Supports Multiboot2 and PVH
 * Direct-Boot Architecture Parity with AArch64
 */

#define MB1_MAGIC        0x1BADB002
#define MB1_FLAGS        0x00000003 /* ALIGN + MEMINFO */
#define MB2_MAGIC        0xE85250D6
#define MB2_ARCH_I386    0

.section .multiboot
.align 4
mb1_header:
    .long MB1_MAGIC
    .long MB1_FLAGS
    .long -(MB1_MAGIC + MB1_FLAGS)

.align 8
mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long mb2_header_end - mb2_header_start
    .long -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start))

    /* Entry Address Tag (Type 5) */
    .align 8
    .short 5
    .short 0
    .long 12
    .long _start_32

    /* End Tag */
    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

/* PVH ELF Note for QEMU direct -kernel loading */
.section .note.PVH, "a", @note
.align 4
    .long 4
    .long 4
    .long 18
    .asciz "Xen"
    .align 4
    .long _start_32

.code32
.section .text.boot
.global _start_32
_start_32:
    cli

    /* Detect boot protocol: 
       Multiboot2 (EAX=0x36d24269)
       Multiboot1 (EAX=0x2BADB002)
       PVH (EAX=0x336ec578) */
    
    cmpl $0x336ec578, %eax
    je .pvh_boot
    
    /* Multiboot path */
    movl %eax, (mb_magic_phys)
    movl %ebx, (mb_info_ptr_phys)
    jmp .prepare_paging

.pvh_boot:
    movl %eax, (mb_magic_phys)
    movl %esi, (mb_info_ptr_phys)

.prepare_paging:
    /* Clear boot page tables */
    movl $boot_pml4, %edi
    xorl %eax, %eax
    movl $3072, %ecx /* PML4 + PDPT + PD (12KB) */
    rep stosl

    /* Identity Map first 1GB with 2MB pages */
    /* PML4[0] -> PDPT */
    movl $boot_pml4, %edi
    movl $boot_pdpt, %eax
    orl  $0x3, %eax
    movl %eax, (%edi)

    /* PDPT[0] -> PD */
    movl $boot_pdpt, %edi
    movl $boot_pd, %eax
    orl  $0x3, %eax
    movl %eax, (%edi)

    /* PD -> 512 * 2MB pages (1GB total) */
    movl $boot_pd, %edi
    movl $0x83, %eax /* Present | RW | PageSize(2MB) */
    movl $512, %ecx
.fill_pd:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd

    /* Enable PAE */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* Load CR3 (PML4) */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* Enable Long Mode in EFER MSR */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax /* LME */
    wrmsr

    /* Enable Paging and Protected Mode */
    movl %cr0, %eax
    orl  $0x80000001, %eax /* PG | PE */
    movl %eax, %cr0

    /* Load transition GDT and jump to 64-bit */
    lgdt (boot_gdtr_32)
    ljmp $0x08, $_start_64

.code64
.global _start_64
_start_64:
    /* Fully in 64-bit mode. Set segment registers to NULL (recommended) */
    xorl %eax, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Enable SSE/AVX (Required for modern C toolchains) */
    movq %cr0, %rax
    andw $0xFFFB, %ax    /* Clear EM */
    orw  $0x0002, %ax    /* Set MP */
    movq %rax, %cr0
    movq %cr4, %rax
    orw  $(3 << 9), %ax  /* OSFXSR | OSXMMEXCPT */
    movq %rax, %cr4

    /* Clear BSS */
    movabsq $__bss_start, %rdi
    movabsq $__bss_end, %rcx
.bss_loop:
    cmpq %rcx, %rdi
    jae .bss_done
    movb $0, (%rdi)
    incq %rdi
    jmp .bss_loop
.bss_done:

    /* Setup initial stack */
    movabsq $_stack_top, %rsp
    movq %rsp, %rbp

    /* Load final 64-bit GDT */
    lgdt (boot_gdtr)

    /* Handover to C: RDI = Multiboot info pointer */
    movq (mb_info_ptr), %rdi
    call kernel_main

.halt:
    hlt
    jmp .halt

/* Boot data located in .data to survive BSS clearing */
.section .data
.align 8
.global mb_info_ptr
.global mb_magic
mb_magic:
mb_magic_phys: .quad 0
mb_info_ptr:
mb_info_ptr_phys: .quad 0

.align 16
boot_gdt:
    .quad 0x0000000000000000   /* 0x00: Null */
    .quad 0x00AF9A000000FFFF   /* 0x08: Kernel Code */
    .quad 0x00CF92000000FFFF   /* 0x10: Kernel Data */
    .quad 0x00CFF2000000FFFF   /* 0x18: User Data */
    .quad 0x00AFFA000000FFFF   /* 0x20: User Code */
boot_gdt_end:

.global boot_gdtr
boot_gdtr:
    .word boot_gdt_end - boot_gdt - 1
    .quad boot_gdt

boot_gdtr_32:
    .word boot_gdt_end - boot_gdt - 1
    .long boot_gdt

/* Initial Page Tables (Mapped 1:1 for 1GB) */
.align 4096
.global boot_pml4
boot_pml4: .skip 4096
.global boot_pdpt
boot_pdpt: .skip 4096
.global boot_pd
boot_pd:   .skip 4096

.section .bss
.align 4096
.global _stack_bottom
_stack_bottom:
    .skip 0x20000    /* 128KB stack */
.global _stack_top
_stack_top:
