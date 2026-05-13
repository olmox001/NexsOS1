/*
 * kernel/arch/amd64/boot/trampoline.S
 * Secondary CPU Trampoline (16-bit -> 64-bit)
 */
.section .trampoline, "ax"
.code16

.global trampoline_start
.global trampoline_end

trampoline_start:
    cli
    cld

    /* Set up segments */
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* Load temporary GDT */
    lgdtl (trampoline_gdt_ptr - trampoline_start + 0x1000)

    /* Enable Protected Mode */
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0

    /* Long jump to protected mode */
    ljmp $0x08, $(trampoline_pm - trampoline_start + 0x1000)

.code32
trampoline_pm:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* Enable PAE */
    mov %cr4, %eax
    or $(1 << 5), %eax
    mov %eax, %cr4

    /* Load PML4 */
    mov (trampoline_pml4 - trampoline_start + 0x1000), %eax
    mov %eax, %cr3

    /* Enable Long Mode in EFER */
    mov $0xC0000080, %ecx
    rdmsr
    or $(1 << 8), %eax
    wrmsr

    /* Enable Paging and Write Protect */
    mov %cr0, %eax
    or $(1 << 31 | 1 << 16), %eax
    mov %eax, %cr0

    /* Long jump to 64-bit mode */
    ljmp $0x18, $(trampoline_lm - trampoline_start + 0x1000)

.code64
trampoline_lm:
    /* Load stack pointer */
    mov (trampoline_stack - trampoline_start + 0x1000), %rsp
    
    /* Call C entry point */
    mov (trampoline_entry - trampoline_start + 0x1000), %rax
    call *%rax

    /* Should not return */
1:  hlt
    jmp 1b

.align 16
trampoline_gdt:
    .quad 0x0000000000000000 /* Null */
    .quad 0x00cf9a000000ffff /* 32-bit Code */
    .quad 0x00cf92000000ffff /* 32-bit Data */
    .quad 0x00af9a000000ffff /* 64-bit Code */
trampoline_gdt_end:

trampoline_gdt_ptr:
    .word (trampoline_gdt_end - trampoline_gdt - 1)
    .long (trampoline_gdt - trampoline_start + 0x1000)

.align 8
.global trampoline_pml4
.global trampoline_stack
.global trampoline_entry
trampoline_pml4:  .long 0
trampoline_stack: .quad 0
trampoline_entry: .quad 0

trampoline_end:
