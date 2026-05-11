/*
 * kernel/arch/amd64/boot/boot.S
 * x86-64 Multiboot2 Boot Entry
 *
 * Enters in 32-bit protected mode (Multiboot2 spec).
 * Sets up paging for Long Mode, then jumps to 64-bit kernel_main.
 */

/* Multiboot2 header constants */
.set MB2_MAGIC,        0xE85250D6
.set MB2_ARCH_I386,    0
.set MB2_HEADER_LEN,   (mb2_header_end - mb2_header)
.set MB2_CHECKSUM,     -(MB2_MAGIC + MB2_ARCH_I386 + MB2_HEADER_LEN)

/* Paging constants */
.set PAGE_PRESENT,  0x01
.set PAGE_WRITE,    0x02
.set PAGE_SIZE_BIT, 0x80   /* 2MB pages */

/* Control Register bits */
.set CR0_PE,  (1 << 0)
.set CR0_PG,  (1 << 31)
.set CR4_PAE, (1 << 5)
.set CR4_PGE, (1 << 7)
.set EFER_MSR, 0xC0000080
.set EFER_LME, (1 << 8)    /* Long Mode Enable */
.set EFER_SCE, (1 << 0)    /* System Call Enable */

/* ═══════════════════════════════════════════════════════════════════
   MULTIBOOT2 HEADER (must be in first 32KB of the binary)
   ═══════════════════════════════════════════════════════════════════ */

.section .multiboot2, "a"
.align 8
mb2_header:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long MB2_HEADER_LEN
    .long MB2_CHECKSUM
    /* End tag */
    .word 0    /* type */
    .word 0    /* flags */
    .long 8    /* size */
mb2_header_end:

/* ═══════════════════════════════════════════════════════════════════
   32-BIT ENTRY POINT
   ═══════════════════════════════════════════════════════════════════ */

.section .text.boot
.code32
.global _start
_start:
    /* Disable interrupts */
    cli

    /* Save multiboot info pointer (EBX) and magic (EAX) */
    movl %ebx, (mb_info_ptr)
    movl %eax, (mb_magic)

    /* Set up temporary 32-bit stack */
    movl $_stack_top, %esp

    /* Clear BSS */
    movl $_bss_start, %edi
    movl $_bss_end, %ecx
    subl %edi, %ecx
    shrl $2, %ecx
    xorl %eax, %eax
    rep stosl

    /* ─── Setup Identity-Mapped Paging for Long Mode ─── */

    /* PML4[0] -> PDPT */
    movl $boot_pdpt, %eax
    orl  $(PAGE_PRESENT | PAGE_WRITE), %eax
    movl %eax, (boot_pml4)

    /* PDPT[0] -> PD */
    movl $boot_pd, %eax
    orl  $(PAGE_PRESENT | PAGE_WRITE), %eax
    movl %eax, (boot_pdpt)

    /* PD: Identity map first 1GB with 2MB pages (512 entries) */
    movl $boot_pd, %edi
    movl $(PAGE_PRESENT | PAGE_WRITE | PAGE_SIZE_BIT), %eax
    movl $512, %ecx
.fill_pd:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax    /* Next 2MB page */
    addl $8, %edi
    decl %ecx
    jnz .fill_pd

    /* ─── Enable PAE ─── */
    movl %cr4, %eax
    orl  $(CR4_PAE | CR4_PGE), %eax
    movl %eax, %cr4

    /* ─── Load PML4 into CR3 ─── */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* ─── Enable Long Mode via IA32_EFER MSR ─── */
    movl $EFER_MSR, %ecx
    rdmsr
    orl  $(EFER_LME | EFER_SCE), %eax
    wrmsr

    /* ─── Enable Paging (CR0.PG) ─── */
    movl %cr0, %eax
    orl  $(CR0_PG | CR0_PE), %eax
    movl %eax, %cr0

    /* ─── Load 64-bit GDT and far jump to Long Mode ─── */
    lgdt (boot_gdtr)
    ljmp $0x08, $long_mode_entry

/* ═══════════════════════════════════════════════════════════════════
   64-BIT ENTRY
   ═══════════════════════════════════════════════════════════════════ */

.code64
long_mode_entry:
    /* Reload data segments */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Set up 64-bit stack */
    movabs $_stack_top, %rsp

    /* Load multiboot info pointer as argument to kernel_main */
    movl (mb_info_ptr), %edi
    /* kernel_main(uint32_t mb_info_phys) */

    /* Clear direction flag */
    cld

    /* Call kernel */
    call kernel_main

    /* If kernel_main returns, halt */
    cli
.halt_loop:
    hlt
    jmp .halt_loop

/* ═══════════════════════════════════════════════════════════════════
   BOOT DATA
   ═══════════════════════════════════════════════════════════════════ */

.section .data

/* Saved multiboot pointers */
.global mb_info_ptr
.global mb_magic
mb_info_ptr: .long 0
mb_magic:    .long 0

/* ─── Minimal 64-bit GDT for boot ─── */
.align 16
boot_gdt:
    .quad 0x0000000000000000   /* 0x00: Null */
    .quad 0x00AF9A000000FFFF   /* 0x08: Kernel Code (64-bit, DPL=0) */
    .quad 0x00CF92000000FFFF   /* 0x10: Kernel Data */
boot_gdt_end:

boot_gdtr:
    .word boot_gdt_end - boot_gdt - 1
    .long boot_gdt

/* ═══════════════════════════════════════════════════════════════════
   PAGE TABLES (BSS — zeroed)
   ═══════════════════════════════════════════════════════════════════ */

.section .bss
.align 4096

.global boot_pml4
boot_pml4:   .skip 4096
boot_pdpt:   .skip 4096
boot_pd:     .skip 4096

/* Kernel stack (128 KB) */
.align 4096
.global _stack_bottom
_stack_bottom:
    .skip 0x20000   /* 128 KB */
.global _stack_top
_stack_top:
