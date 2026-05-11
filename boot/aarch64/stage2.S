.section .text

.global stage2_entry
.type stage2_entry, @function

stage2_entry:
    // Setup ambiente kernel
    bl setup_kernel_environment
    
    // Inizializza dispositivi base
    bl init_basic_devices
    
    // Salta al kernel entry point
    adrp x0, kernel_multiboot_info
    add x0, x0, :lo12:kernel_multiboot_info
    ldr x0, [x0]
    adrp x1, kernel_entry_address
    add x1, x1, :lo12:kernel_entry_address
    ldr x1, [x1]
    cbz x1, stage2_halt  // Se non c'è entry point, halt
    br x1

stage2_halt:
    wfe
    b stage2_halt

// Setup ambiente kernel
setup_kernel_environment:
    // Setup stack kernel
    adrp x0, __kernel_stack_top
    add x0, x0, :lo12:__kernel_stack_top
    mov sp, x0
    
    // Prepara multiboot info strutturata per kernel
    bl prepare_kernel_multiboot_info
    
    ret

// Prepara info multiboot per kernel
prepare_kernel_multiboot_info:
    // Crea struttura multiboot info semplificata per kernel
    adrp x0, kernel_multiboot_info_struct
    add x0, x0, :lo12:kernel_multiboot_info_struct
    
    // Copia info rilevanti dal multiboot originale
    adrp x1, multiboot_mmap_tag
    add x1, x1, :lo12:multiboot_mmap_tag
    ldr x1, [x1]
    str x1, [x0, #8]  // memory map
    
    // Setup flags
    mov x1, #0x1      // basic memory info
    str x1, [x0]
    
    ret

// Inizializzazione dispositivi base
init_basic_devices:
    // Qui si inizializzerebbero dispositivi essenziali
    // UART, timer, ecc.
    ret

.section .data
.align 8

kernel_multiboot_info:
    .quad 0

kernel_entry_address:
    .quad 0

kernel_multiboot_info_struct:
    .quad 0    // flags
    .quad 0    // memory map pointer
    .quad 0    // boot device
    .quad 0    // cmdline
    .quad 0    // mods count
    .quad 0    // mods addr
    .quad 0    // elf sections
    .quad 0    // memory map length
    .quad 0    // boot loader name
    .quad 0    // apm table
    .quad 0    // vbe info

.section .bss
.align 16

// Stack kernel
.balign 16
__kernel_stack:
    .skip 65536  // 64KB stack kernel
__kernel_stack_top:
