.section .text

// Entry point dal GRUB multiboot2
.global _start
.type _start, @function

_start:
    // Salva il multiboot info pointer e magic
    mov x19, x0    // multiboot2 info pointer
    mov x20, x1    // multiboot2 magic
    
    // Verifica magic number
    ldr x0, =0x36d76289
    cmp x20, x0
    bne halt
    
    // Disabilita interrupts
    msr daifset, #0xf
    
    // Setup stack primario
    adrp x0, __stack_top
    add x0, x0, :lo12:__stack_top
    mov sp, x0
    
    // Salva core ID
    mrs x21, mpidr_el1
    and x21, x21, #0xff
    
    // Solo core primario procede
    cbz x21, primary_core_init
    
    // Core secondari attendono
    b secondary_core_wait

primary_core_init:
    // Parse multiboot2 info
    bl parse_multiboot2_info
    
    // Setup paginazione
    bl setup_paging
    
    // Setup MMU
    bl enable_mmu
    
    // Inizializza core secondari
    bl init_secondary_cores
    
    // Salta a stage2
    ldr x0, =stage2_entry
    br x0

halt:
    wfe
    b halt

// Parse multiboot2 information structure
parse_multiboot2_info:
    mov x0, x19    // multiboot2 info pointer
    
    // Verifica versione (deve essere 0)
    ldr w1, [x0, #4]
    cbz w1, parse_continue
    b halt
    
parse_continue:
    // Calcola lunghezza totale
    ldr w2, [x0]    // total_size
    // Estendi w2 a x2 per l'aritmetica a 64-bit
    uxtw x2, w2
    add x3, x0, x2  // end pointer
    
    // Inizio tags
    add x4, x0, #16
    
parse_loop:
    cmp x4, x3
    bge parse_done
    
    // Leggi tag type e size
    ldr w5, [x4]     // type
    ldr w6, [x4, #4] // size
    
    // Allinea size a 8 byte
    add w6, w6, #7
    and w6, w6, #0xfffffff8
    
    // Switch su type
    cmp w5, #6    // MEMORY_MAP
    beq parse_memory_map
    
    cmp w5, #9    // ENTRY_ADDRESS
    beq parse_entry_address
    
    // Prossimo tag
    uxtw x6, w6
    add x4, x4, x6
    b parse_loop

parse_memory_map:
    // Processa memory map - usa adrp/add per indirizzamento
    adrp x7, multiboot_mmap_tag
    add x7, x7, :lo12:multiboot_mmap_tag
    str x4, [x7]
    uxtw x6, w6
    add x4, x4, x6
    b parse_loop

parse_entry_address:
    // Processa entry address
    ldr x5, [x4, #8]
    adrp x7, kernel_entry_point
    add x7, x7, :lo12:kernel_entry_point
    str x5, [x7]
    uxtw x6, w6
    add x4, x4, x6
    b parse_loop

parse_done:
    ret

// Setup paginazione 4-level
setup_paging:
    // Azzera le tabelle di paginazione
    adrp x0, page_table_l0
    add x0, x0, :lo12:page_table_l0
    mov x1, #0
    mov x2, #4096 * 4  // 4 pagine per tabelle
clear_tables:
    str xzr, [x0], #8
    subs x2, x2, #8
    bne clear_tables
    
    // Crea identity mapping per i primi 4GB
    adrp x0, page_table_l0
    add x0, x0, :lo12:page_table_l0
    adrp x1, page_table_l1
    add x1, x1, :lo12:page_table_l1
    orr x1, x1, #0x3  // valid + page table
    str x1, [x0]
    
    // Setup L1 table (1GB block entries)
    // Per blocchi da 1GB usiamo descriptor di tipo block (bits [1:0] = 01)
    adrp x0, page_table_l1
    add x0, x0, :lo12:page_table_l1
    mov x1, #0        // start address 0x0
    mov x2, #4        // 4 entries (4GB)
    
    // Attributi per block descriptor: AF=1, nG=0, AP=00, SH=10 (outer shareable)
    // Lower attrs: 0x401 (valid block) + 0x400 (AF bit)
    movz x8, #0x0401
    
setup_l1_loop:
    orr x3, x1, x8    // address + attributes
    str x3, [x0], #8
    // Incrementa di 1GB (0x40000000)
    mov x9, #0x4000
    lsl x9, x9, #16   // x9 = 0x40000000
    add x1, x1, x9
    subs x2, x2, #1
    bne setup_l1_loop
    
    ret

// Abilita MMU
enable_mmu:
    // Setup MAIR_EL1 (Memory Attribute Indirection Register)
    // Attr0: Device-nGnRnE, Attr1: Normal Write-Back
    movz x0, #0x0044
    movk x0, #0xff00, lsl #16
    msr mair_el1, x0
    
    // Setup TCR_EL1 (Translation Control Register)
    // T0SZ=25 (39-bit VA), IRGN0=1, ORGN0=1, SH0=3, TG0=0 (4KB)
    movz x0, #0x0019
    movk x0, #0x8080, lsl #16
    movk x0, #0x0035, lsl #32
    msr tcr_el1, x0
    
    // Setup TTBR0_EL1 (Translation Table Base Register)
    adrp x0, page_table_l0
    add x0, x0, :lo12:page_table_l0
    msr ttbr0_el1, x0
    
    // Invalida TLB
    tlbi vmalle1
    dsb sy
    isb
    
    // Setup SCTLR_EL1 (System Control Register)
    mrs x0, sctlr_el1
    orr x0, x0, #0x1    // M bit (MMU enable)
    orr x0, x0, #0x4    // C bit (cache enable)
    orr x0, x0, #0x1000 // I bit (instruction cache)
    msr sctlr_el1, x0
    isb
    
    ret

// Inizializza core secondari
init_secondary_cores:
    mov x0, #1  // start from core 1
    
init_core_loop:
    cmp x0, #8  // max 8 cores
    bge init_cores_done
    
    // Scrivi entry point per core secondario
    adrp x1, secondary_core_entry
    add x1, x1, :lo12:secondary_core_entry
    adrp x2, cpu_entry_addrs
    add x2, x2, :lo12:cpu_entry_addrs
    str x1, [x2, x0, lsl #3]
    
    // Scrivi stack per core secondario
    adrp x1, __stack_base
    add x1, x1, :lo12:__stack_base
    add x1, x1, x0, lsl #16  // 64KB per core
    add x1, x1, #0x4000      // stack pointer a fine area
    adrp x2, cpu_stacks
    add x2, x2, :lo12:cpu_stacks
    str x1, [x2, x0, lsl #3]
    
    add x0, x0, #1
    b init_core_loop
    
init_cores_done:
    ret

// Core secondari attendono
secondary_core_wait:
    // Setup stack per core secondario
    mrs x0, mpidr_el1
    and x0, x0, #0xff
    
    adrp x1, cpu_stacks
    add x1, x1, :lo12:cpu_stacks
    ldr x2, [x1, x0, lsl #3]
    mov sp, x2
    
    // Abilita MMU
    mrs x1, sctlr_el1
    orr x1, x1, #0x1
    msr sctlr_el1, x1
    isb
    
    // Attendi entry point
secondary_wait_loop:
    wfe
    adrp x1, cpu_entry_addrs
    add x1, x1, :lo12:cpu_entry_addrs
    ldr x2, [x1, x0, lsl #3]
    cbz x2, secondary_wait_loop
    br x2

secondary_core_entry:
    // Core secondario entry point
    b secondary_core_main

secondary_core_main:
    // Qui arrivano i core secondari
    // In un sistema reale, qui si integrerebbe con il kernel
    wfe
    b secondary_core_main

.section .data
.align 8

.global multiboot_mmap_tag
multiboot_mmap_tag:
    .quad 0

.global kernel_entry_point
kernel_entry_point:
    .quad 0

.section .bss
.align 16

// Tabelle di paginazione
.balign 4096
page_table_l0:
    .skip 4096

.balign 4096
page_table_l1:
    .skip 4096

.balign 4096
page_table_l2:
    .skip 4096

.balign 4096
page_table_l3:
    .skip 4096

// Stack per i core
.balign 16
__stack_base:
    .skip 8 * 65536  // 8 core * 64KB ciascuno
__stack_top:

// Entry points e stack per core secondari
.balign 8
cpu_entry_addrs:
    .skip 8 * 8  // 8 entry points

.balign 8
cpu_stacks:
    .skip 8 * 8  // 8 stack pointers
