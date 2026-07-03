/*
 * kernel/arch/amd64/kernel.ld — x86-64 Linker Script (higher half)
 *
 * Physical layout (what GRUB / QEMU's PVH loader places in RAM, by p_paddr):
 *   1MB..2MB  boot region, linked VA == PA: multiboot/PVH headers, the
 *             32-bit entry + long-mode stub (.text.boot), the boot GDT and
 *             the boot page tables (.boot_bss).  This code runs before/
 *             during paging enable, so it must execute at its link address.
 *   2MB..     kernel proper, linked at KERNEL_VIRT_BASE + PA
 *             (0xFFFF800000000000 + 2MB — see kernel/memlayout.h) and
 *             loaded at the AT() physical address.  start.S jumps here
 *             after enabling paging with both the identity and the
 *             higher-half mappings.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start_32)

KVB = 0xFFFF800000000000;

PHDRS
{
    boot      PT_LOAD FLAGS(7); /* R+W+X: pre-paging code + boot tables */
    note      PT_NOTE FLAGS(4); /* R */
    text      PT_LOAD FLAGS(5); /* R+X */
    data      PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS {
    /* ── Low boot region (VA == PA) ─────────────────────────────────── */
    . = 1M;

    .boot.text : ALIGN(8) {
        KEEP(*(.multiboot))
        KEEP(*(.text.boot))
    } :boot

    .note.PVH : ALIGN(4) {
        KEEP(*(.note.PVH))
    } :note

    .boot_bss : ALIGN(4096) {
        *(.boot_bss*)
    } :boot

    . = ALIGN(4096);
    __boot_phys_end = .;
    ASSERT(__boot_phys_end <= 2M, "boot region overflows the 1MB..2MB window")

    /* ── Kernel proper (higher half, loaded at PA = VA - KVB) ───────── */
    . = KVB + 2M;

    .text : AT(ADDR(.text) - KVB) ALIGN(4096) {
        __text_start = .;
        *(.text*)
        /* AP trampoline blob: copied to 0x1000 at SMP bring-up; fully
         * position-independent, never executed in place. */
        . = ALIGN(8);
        KEEP(*(.trampoline))
    } :text
    _etext = .;

    .rodata : AT(ADDR(.rodata) - KVB) ALIGN(4096) {
        *(.rodata*)
        . = ALIGN(16); /* GCC DATA_ALIGNMENT ups >=16-byte aggregates to 16 on
         * x86-64: with ALIGN(8) the linker could pad AFTER __ktests_start,
         * making the first walked descriptor garbage (S-ALIGN F9 fix). */
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;
        /* In-kernel symbol table (tools/gen_ksyms.sh, two-pass link).
         * Placed after .text so a populated table never shifts text
         * addresses between the two link passes. */
        . = ALIGN(8);
        __ksyms_start = .;
        KEEP(*(.ksyms))
        __ksyms_end = .;
    } :text
    __erodata = .;

    .data : AT(ADDR(.data) - KVB) ALIGN(4096) {
        *(.data*)
    } :data

    .bss : AT(ADDR(.bss) - KVB) ALIGN(4096) {
        __bss_start = .;
        *(.bss*)
        *(.lbss*)
        *(COMMON)
        __bss_end = .;
    } :data
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

/* __kernel_start is the VIRTUAL address of everything the loader placed in
 * RAM (boot region included): virt_to_phys(__kernel_start) == 1MB, so the
 * PMM reserves the whole image, boot sections and all. */
PROVIDE(__kernel_start = KVB + 1M);
PROVIDE(__kernel_size = __kernel_end - __kernel_start);
