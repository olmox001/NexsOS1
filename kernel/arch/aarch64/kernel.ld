/*
 * Kernel linker script for AArch64
 * Loaded at 0x40000000
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;
    
    /* Code section */
    .text : {
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
    }
    
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

/* Provide symbols for runtime */
PROVIDE(__kernel_start = 0x40000000);
PROVIDE(__kernel_size = __kernel_end - __kernel_start);
