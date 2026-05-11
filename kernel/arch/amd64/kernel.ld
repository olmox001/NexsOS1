/*
 * kernel/arch/amd64/kernel.ld — x86-64 Linker Script
 * Kernel loaded at 1MB by Multiboot2
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
    /* Multiboot2 loads at 1MB */
    . = 1M;
    __kernel_start = .;

    .multiboot : ALIGN(8) {
        KEEP(*(.multiboot))
    }
    .multiboot2 : ALIGN(8) {
        KEEP(*(.multiboot2))
    }

    .text : ALIGN(4096) {
        *(.text.boot)
        *(.text*)
    }
    _etext = .;

    .rodata : ALIGN(4096) {
        *(.rodata*)
    }

    .data : ALIGN(4096) {
        *(.data*)
    }

    .bss : ALIGN(4096) {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }
    __kernel_end = .;

    /* Kernel test section (for ktest) */
    __ktests_start = .;
    .ktests : {
        *(.ktests)
    }
    __ktests_end = .;

    /* Kernel stack (defined in boot.S BSS) */
    /* _stack_bottom and _stack_top are provided by boot.S */

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
