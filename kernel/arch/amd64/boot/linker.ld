/*
 * boot/amd64/linker.ld
 * Linker script for AMD64 Bootloader
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

/* Keep the Multiboot2 header in its own read-only load segment.  The early
 * entry code is executable, while boot tables/stacks and saved handoff data
 * are writable but never executable.  This mirrors the permission split in
 * kernel/arch/amd64/kernel.ld without changing the physical boot layout. */
PHDRS
{
    mb2     PT_LOAD FLAGS(4); /* R: Multiboot2 header */
    text    PT_LOAD FLAGS(5); /* R+X: bootstrap instructions */
    rodata  PT_LOAD FLAGS(4); /* R: immutable tables/constants */
    data    PT_LOAD FLAGS(6); /* R+W: handoff state, page tables and stacks */
}

SECTIONS
{
    /* Parity with aarch64 bootloader address */
    . = 0x40000000;

    .multiboot2_header : ALIGN(8) {
        KEEP(*(.multiboot2_header))
    } :mb2

    . = ALIGN(4096);
    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } :data

    . = ALIGN(4096);
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
