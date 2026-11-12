.text
.p2align 2
.globl nexs_efi_handoff

/*
 * void nexs_efi_handoff(kernel_image_pa, dtb_pa, kernel_span, dtb_span,
 *                        firmware_map_pa, firmware_record_count, ebs_evidence)
 *
 * Called only after ExitBootServices(). Clean loaded/handoff ranges to PoC,
 * invalidate the instruction cache, disable the active EL's MMU/caches,
 * mask interrupts and enter the ARM64 Image header at its physical base.
 * The Image header branches to the kernel's _start with x0=DTB.
 */
nexs_efi_handoff:
    mov     x19, x0                  /* kernel image physical base */
    mov     x20, x1                  /* DTB physical base */
    mov     x21, x2                  /* kernel memory span incl. BSS */
    mov     x22, x3                  /* DTB span */
    mov     x23, x4                  /* normalized firmware map base */
    mov     x24, x5                  /* normalized firmware record count */
    mov     x25, x6                  /* low32 attempts, high32 growths */
    mov     x26, #40
    mul     x26, x24, x26            /* normalized map byte span */
    msr     daifset, #0xf

    /* D-cache line size = 4 << CTR_EL0.DminLine. */
    mrs     x9, ctr_el0
    ubfx    x10, x9, #16, #4
    mov     x11, #4
    lsl     x11, x11, x10
    sub     x12, x11, #1

    /* Clean kernel image range. */
    bic     x13, x19, x12
    add     x14, x19, x21
.Lclean_kernel:
    cmp     x13, x14
    b.hs    .Lclean_dtb_start
    dc      cvac, x13
    add     x13, x13, x11
    b       .Lclean_kernel

    /* Clean DTB range. */
.Lclean_dtb_start:
    bic     x13, x20, x12
    add     x14, x20, x22
.Lclean_dtb:
    cmp     x13, x14
    b.hs    .Lclean_map_start
    dc      cvac, x13
    add     x13, x13, x11
    b       .Lclean_dtb

    /* Clean normalized UEFI memory-map records written after EBS. */
.Lclean_map_start:
    cbz     x23, .Lcache_sync
    bic     x13, x23, x12
    add     x14, x23, x26
.Lclean_map:
    cmp     x13, x14
    b.hs    .Lcache_sync
    dc      cvac, x13
    add     x13, x13, x11
    b       .Lclean_map

.Lcache_sync:
    dsb     sy
    ic      iallu
    dsb     sy
    isb

    /* Disable translation and caches at the current exception level. */
    mrs     x9, CurrentEL
    cmp     x9, #0x8
    b.eq    .Ldisable_el2
    cmp     x9, #0x4
    b.eq    .Ldisable_el1
    cmp     x9, #0xc
    b.eq    .Ldisable_el3
    b       .Ldead

.Ldisable_el1:
    mrs     x10, sctlr_el1
    bic     x10, x10, #1              /* SCTLR_EL1.M */
    bic     x10, x10, #4              /* SCTLR_EL1.C */
    bic     x10, x10, #0x1000         /* SCTLR_EL1.I */
    msr     sctlr_el1, x10
    tlbi    vmalle1
    b       .Lafter_disable

.Ldisable_el2:
    mrs     x10, sctlr_el2
    bic     x10, x10, #1              /* SCTLR_EL2.M */
    bic     x10, x10, #4              /* SCTLR_EL2.C */
    bic     x10, x10, #0x1000         /* SCTLR_EL2.I */
    msr     sctlr_el2, x10
    tlbi    alle2
    b       .Lafter_disable

.Ldisable_el3:
    mrs     x10, sctlr_el3
    bic     x10, x10, #1              /* SCTLR_EL3.M */
    bic     x10, x10, #4              /* SCTLR_EL3.C */
    bic     x10, x10, #0x1000         /* SCTLR_EL3.I */
    msr     sctlr_el3, x10
    tlbi    alle3

.Lafter_disable:
    dsb     sy
    isb
    mov     x0, x20                  /* DTB physical address */
    mov     x1, x22                  /* validated DTB available span */
    mov     x2, x23                  /* normalized UEFI memory map */
    mov     x3, x24                  /* normalized memory record count */
    mov     x4, x25                  /* EBS attempt/growth evidence */
    br      x19                      /* execute Image header at load PA */

.global nexs_efi_post_exit_halt
nexs_efi_post_exit_halt:
.Ldead:
    wfe
    b       .Ldead
