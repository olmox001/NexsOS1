// Header multiboot2 per GRUB
.section .multiboot2_header
.align 8

multiboot2_start:
    // Magic number
    .long 0xe85250d6              // magic
    .long 0                       // architecture (0 = i386, but GRUB handles AArch64)
    .long multiboot2_end - multiboot2_start  // header_length
    // checksum
    .long -(0xe85250d6 + 0 + (multiboot2_end - multiboot2_start))

    // Entry address tag
    .short 5                      // type = entry_address
    .short 0                      // flags
    .long 16                      // size (type + flags + size + entry_addr = 2+2+4+8)
    .quad _start                  // entry point (64-bit)

    // Framebuffer tag
    .short 8                      // type = framebuffer
    .short 0                      // flags
    .long 20                      // size
    .long 0                       // width
    .long 0                       // height
    .long 0                       // depth

    // End tag
    .short 0                      // type = end
    .short 0                      // flags
    .long 8                       // size

multiboot2_end:
