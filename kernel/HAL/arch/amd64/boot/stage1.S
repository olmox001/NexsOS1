/*
 * boot/amd64/stage1.S
 * AMD64 Bootloader - Stage 1 (32-bit)
 * Parity with aarch64 implementation
 */

.code32
.section .text

.global _start
_start:
    /* Disable interrupts */
    cli

    /* Save multiboot info */
    movl %ebx, (multiboot_info_ptr)
    movl %eax, (multiboot_magic)

    /* Setup 32-bit stack */
    movl $__boot_stack_top, %esp

    /* 1. Parse Multiboot2 Info */
    call parse_multiboot2_info

    /* 2. CPU Check: Long Mode Support */
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz .no_long_mode

    /* 3. Setup Paging (Identity Map 1GB) */
    movl $page_table_l4, %edi
    xorl %eax, %eax
    movl $4096, %ecx
    rep stosl

    /* PML4[0] -> PDP */
    movl $page_table_l4, %edi
    movl $page_table_l3, %eax
    orl  $0x3, %eax
    movl %eax, (%edi)

    /* PDP[0] -> PD */
    movl $page_table_l3, %edi
    movl $page_table_l2, %eax
    orl  $0x3, %eax
    movl %eax, (%edi)

    /* PD -> 2MB Pages */
    movl $page_table_l2, %edi
    movl $0x83, %eax
    movl $512, %ecx
.fill_pd:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd

    /* 4. Enable PAE */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* 5. Load PML4 */
    movl $page_table_l4, %eax
    movl %eax, %cr3

    /* 6. Enable Long Mode */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* 7. Enable Paging */
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    /* 8. Jump to stage2 (64-bit) */
    lgdt (boot_gdtr)
    ljmp $0x08, $stage2_entry_trampoline

.no_long_mode:
    hlt
    jmp .no_long_mode

/* Multiboot2 Parser */
parse_multiboot2_info:
    movl (multiboot_info_ptr), %ebx
    testl %ebx, %ebx
    jz .parse_done

    /* Skip total size and reserved */
    addl $8, %ebx
    
.parse_loop:
    movl (%ebx), %eax        /* type */
    movl 4(%ebx), %ecx       /* size */
    
    testl %eax, %eax         /* end tag? */
    jz .parse_done
    
    cmpl $9, %eax            /* ENTRY_ADDRESS tag? */
    jne .next_tag
    
    /* Found Entry Address */
    movl 8(%ebx), %eax
    movl %eax, (kernel_entry_point)
    
.next_tag:
    addl $7, %ecx            /* align to 8 bytes */
    andl $0xfffffff8, %ecx
    addl %ecx, %ebx
    jmp .parse_loop

.parse_done:
    ret

.section .data
.align 4
.global multiboot_info_ptr
.global multiboot_magic
.global kernel_entry_point
multiboot_info_ptr:  .long 0
multiboot_magic:     .long 0
kernel_entry_point: .quad 0

.section .bss
.align 4096
page_table_l4: .skip 4096
page_table_l3: .skip 4096
page_table_l2: .skip 4096
__boot_stack:  .skip 16384
__boot_stack_top:

.section .rodata
.align 16
boot_gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
boot_gdt_end:

boot_gdtr:
    .word boot_gdt_end - boot_gdt - 1
    .quad boot_gdt
