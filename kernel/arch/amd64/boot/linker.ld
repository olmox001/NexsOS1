/*
 * boot/amd64/linker.ld
 * Linker script for AMD64 Bootloader
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

SECTIONS
{
    /* Parity with aarch64 bootloader address */
    . = 0x40000000;

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
