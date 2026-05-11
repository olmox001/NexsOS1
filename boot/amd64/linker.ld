/*
 * boot/amd64/linker.ld
 * Linker script for AMD64 Bootloader
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)

ENTRY(_start)

SECTIONS
{
    /* Load at 2MB to avoid conflict with kernel at 1MB */
    . = 2M;

    .multiboot2_header : {
        KEEP(*(.multiboot2_header))
    }

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
