/*
 * kernel/arch/amd64/kernel.ld — x86-64 Linker Script
 * Parity with aarch64 implementation
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start_32)

PHDRS
{
    multiboot PT_LOAD FLAGS(5); /* R+X */
    note      PT_NOTE FLAGS(4); /* R */
    text      PT_LOAD FLAGS(5); /* R+X */
    data      PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS {
    /* PVH and Multiboot headers at 1MB */
    . = 1M;
    __kernel_start = .;

    .multiboot : ALIGN(8) {
        KEEP(*(.multiboot))
    } :multiboot

    .note.PVH : ALIGN(4) {
        KEEP(*(.note.PVH))
    } :note

    .text : ALIGN(4096) {
        KEEP(*(.text.boot))
        *(.text*)
    } :text
    _etext = .;

    .rodata : ALIGN(4096) {
        *(.rodata*)
        . = ALIGN(8);
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;
    } :text

    .data : ALIGN(4096) {
        *(.data*)
    } :data

    .boot_bss : ALIGN(4096) {
        *(.boot_bss*)
    } :data

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss*)
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

PROVIDE(__kernel_size = __kernel_end - __kernel_start);
