/*
 * kernel/arch/amd64/kernel.ld — x86-64 Linker Script (higher half)
 *
 * Physical layout (what GRUB / QEMU's PVH loader places in RAM, by p_paddr):
 *   1MB..2MB  boot region, linked VA == PA: multiboot/PVH headers, the
 *             32-bit entry + long-mode stub (.text.boot), the boot GDT and
 *             the boot page tables (.boot_bss).  This code runs before/
 *             during paging enable, so it must execute at its link address.
 *   2MB..     kernel proper, linked at KERNEL_VIRT_BASE + PA
 *             (0xFFFF800000000000 + 2MB — see kernel/memlayout.h) and
 *             loaded at the AT() physical address.  start.S jumps here
 *             after enabling paging with both the identity and the
 *             higher-half mappings.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start_32)

KVB = 0xFFFF800000000000;

/*
 * One segment per permission set.  The boot region used to be a single
 * FLAGS(7) R+W+X PT_LOAD holding both the pre-paging code and the boot page
 * tables, which is why the link emitted "LOAD segment with RWX permissions":
 * the image itself contradicted the charter's W^X claim before a single page
 * table was ever built.  The input sections were already correctly marked
 * ("ax" for .text.boot, "aw",@nobits for .boot_bss) — only the script fused
 * them, so splitting the segment costs nothing and makes the ELF say what the
 * sections already said.
 *
 * NOTE: this fixes the IMAGE half of HAL-0b.  The RUNTIME half — amd64 still
 * identity-maps all RAM in the low half alongside the higher-half map, and
 * kernel pages are mapped without NX (AMMU-01) — is untouched and remains
 * open in the plan.  Do not read the absence of this warning as W^X being
 * enforced at runtime.
 */
PHDRS
{
    boot      PT_LOAD FLAGS(5); /* R+X: pre-paging code + boot headers */
    note      PT_NOTE FLAGS(4); /* R:   PVH note, for the QEMU PVH loader */
    boot_data PT_LOAD FLAGS(6); /* R+W: boot GDT + boot page tables       */
    text      PT_LOAD FLAGS(5); /* R+X */
    data      PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS {
    /* ── Low boot region (VA == PA) ─────────────────────────────────── */
    . = 1M;

    .boot.text : ALIGN(8) {
        KEEP(*(.multiboot))
        KEEP(*(.text.boot))
    } :boot

    /* In BOTH segments on purpose.  PT_NOTE is how QEMU's PVH loader finds the
     * entry point, but PT_NOTE is not loadable — an SHF_ALLOC section that
     * belongs to no PT_LOAD is what produced "allocated section `.note.PVH'
     * not in segment".  Naming :boot as well places the bytes in a loadable
     * segment; :note still describes them to the loader. */
    .note.PVH : ALIGN(4) {
        KEEP(*(.note.PVH))
    } :boot :note

    .boot_bss : ALIGN(4096) {
        *(.boot_bss*)
    } :boot_data

    . = ALIGN(4096);
    __boot_phys_end = .;
    ASSERT(__boot_phys_end <= 2M, "boot region overflows the 1MB..2MB window")

    /* ── Kernel proper (higher half, loaded at PA = VA - KVB) ───────── */
    . = KVB + 2M;

    .text : AT(ADDR(.text) - KVB) ALIGN(4096) {
        __text_start = .;
        *(.text*)
        /* AP trampoline blob: copied to 0x1000 at SMP bring-up; fully
         * position-independent, never executed in place. */
        . = ALIGN(8);
        KEEP(*(.trampoline))
    } :text
    _etext = .;

    .rodata : AT(ADDR(.rodata) - KVB) ALIGN(4096) {
        *(.rodata*)
        . = ALIGN(16); /* GCC DATA_ALIGNMENT ups >=16-byte aggregates to 16 on
         * x86-64: with ALIGN(8) the linker could pad AFTER __ktests_start,
         * making the first walked descriptor garbage (S-ALIGN F9 fix). */
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;
        /* In-kernel symbol table (tools/gen_ksyms.sh, two-pass link).
         * Placed after .text so a populated table never shifts text
         * addresses between the two link passes. */
        . = ALIGN(8);
        __ksyms_start = .;
        KEEP(*(.ksyms))
        __ksyms_end = .;
    } :text
    __erodata = .;

    .data : AT(ADDR(.data) - KVB) ALIGN(4096) {
        *(.data*)
    } :data

    .bss : AT(ADDR(.bss) - KVB) ALIGN(4096) {
        __bss_start = .;
        *(.bss*)
        *(.lbss*)
        *(COMMON)
        __bss_end = .;
    } :data
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

/* __kernel_start is the VIRTUAL address of everything the loader placed in
 * RAM (boot region included): virt_to_phys(__kernel_start) == 1MB, so the
 * PMM reserves the whole image, boot sections and all. */
PROVIDE(__kernel_start = KVB + 1M);
PROVIDE(__kernel_size = __kernel_end - __kernel_start);
