/*
 * boot/amd64/header.S
 * Multiboot2 Header for AMD64
 * Parity with aarch64 implementation
 */

.set MB2_MAGIC,        0xE85250D6
.set MB2_ARCH_I386,    0
.set MB2_HEADER_LEN,   (mb2_header_end - mb2_header_start)
.set MB2_CHECKSUM,     -(MB2_MAGIC + MB2_ARCH_I386 + MB2_HEADER_LEN)

.section .multiboot2_header
.align 8
mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long MB2_HEADER_LEN
    .long MB2_CHECKSUM

    /* Entry Address Tag (Type 5) */
    .short 5            /* type */
    .short 0            /* flags */
    .long 16            /* size (2+2+4+8 = 16) */
    .quad _start        /* entry point (64-bit) */

    /* End Tag */
    .short 0
    .short 0
    .long 8
mb2_header_end:
