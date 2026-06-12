/*
 * Kernel linker script for AArch64
 * Loaded at PHYSICAL 0x40080000 (QEMU -kernel raw binary), linked in the
 * higher half at KERNEL_VIRT_BASE (0xFFFF000000000000) + load PA, so the
 * single direct-map offset converts every kernel VA (memlayout.h).
 * start.S runs PC-relative at the load PA until it enables the MMU
 * (TTBR0 identity + TTBR1 higher half) and jumps to the link address.
 */
ENTRY(_start)

SECTIONS
{
    . = 0xFFFF000040080000;
    
    /* Code section */
    .text : {
        __text_start = .;       /* W^X RX window start (vmm_map_ram_wx) */
        KEEP(*(.text.boot))     /* Boot code first */
        *(.text)
        *(.text.*)
    }
    _etext = .;
    
    . = ALIGN(4096);
    
    /* Read-only data */
    .rodata : {
        *(.rodata)
        *(.rodata.*)

        . = ALIGN(8);
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;

        /* In-kernel symbol table (tools/gen_ksyms.sh, two-pass link).
         * MUST be an allocated section inside .rodata: kernel.bin is produced
         * with objcopy -O binary, which keeps only allocated sections — this
         * is the only way the running aarch64 image gets live symbolization. */
        . = ALIGN(8);
        __ksyms_start = .;
        KEEP(*(.ksyms))
        __ksyms_end = .;
    }
    __erodata = .;

    . = ALIGN(4096);

    /* Read-write data */
    .data : {
        *(.data)
        *(.data.*)
    }
    
    . = ALIGN(4096);
    
    /* BSS (uninitialized data) */
    __bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    __bss_end = .;
    
    . = ALIGN(4096);
    
    /* End of kernel */
    __kernel_end = .;
    
    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.gnu*)
        *(.note*)
        *(.eh_frame*)
    }
}

/* Provide symbols for runtime (link/virtual addresses) */
PROVIDE(__kernel_start = 0xFFFF000040080000);
PROVIDE(__kernel_size = __kernel_end - __kernel_start);
