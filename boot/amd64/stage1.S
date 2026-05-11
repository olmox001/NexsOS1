/*
 * boot/amd64/stage1.S
 * AMD64 Bootloader - Stage 1 (32-bit)
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

    /* 1. CPU Check: Long Mode Support */
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz .no_long_mode

    /* 2. Setup Paging for Long Mode (Identity Map 1GB) */
    movl $page_table_l4, %edi
    movl $0, %eax
    movl $4096, %ecx
    rep stosl           /* Clear L4 */

    /* PML4[0] -> PDP */
    movl $page_table_l4, %edi
    movl $page_table_l3, %eax
    orl  $0x3, %eax     /* Present + Write */
    movl %eax, (%edi)

    /* PDP[0] -> PD (1GB map) */
    movl $page_table_l3, %edi
    movl $0x83, %eax    /* Present + Write + 1GB PS (if supported) or just 2MB chain */
    /* For safety across all CPUs, let's use 2MB pages in PD */
    movl $page_table_l2, %eax
    orl  $0x3, %eax
    movl %eax, (%edi)

    /* PD -> 2MB Pages (Identity map first 1GB) */
    movl $page_table_l2, %edi
    movl $0x83, %eax    /* Present + Write + 2MB PS */
    movl $512, %ecx
.fill_pd:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd

    /* 3. Enable PAE */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* 4. Load PML4 */
    movl $page_table_l4, %eax
    movl %eax, %cr3

    /* 5. Enable Long Mode in EFER MSR */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* 6. Enable Paging */
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    /* 7. Load 64-bit GDT and jump to stage2 */
    lgdt (boot_gdtr)
    ljmp $0x08, $stage2_entry_trampoline

.no_long_mode:
    hlt
    jmp .no_long_mode

.section .data
.align 4
.global multiboot_info_ptr
.global multiboot_magic
multiboot_info_ptr: .long 0
multiboot_magic:    .long 0

.section .bss
.align 4096
page_table_l4: .skip 4096
page_table_l3: .skip 4096
page_table_l2: .skip 4096
__boot_stack:  .skip 16384
__boot_stack_top:

/* GDT for transition */
.section .rodata
.align 16
boot_gdt:
    .quad 0x0000000000000000   /* Null */
    .quad 0x00AF9A000000FFFF   /* Kernel Code 64 */
    .quad 0x00CF92000000FFFF   /* Kernel Data 64 */
boot_gdt_end:

boot_gdtr:
    .word boot_gdt_end - boot_gdt - 1
    .quad boot_gdt
