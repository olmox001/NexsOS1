#ifndef NEXS_EFI_KERNEL_IMAGE
#error "NEXS_EFI_KERNEL_IMAGE is required"
#endif
#ifndef NEXS_EFI_DTB_IMAGE
#error "NEXS_EFI_DTB_IMAGE is required"
#endif

.section .rdata,"dr"
.p2align 4
.globl nexs_embedded_kernel_start
nexs_embedded_kernel_start:
.incbin NEXS_EFI_KERNEL_IMAGE
.globl nexs_embedded_kernel_end
nexs_embedded_kernel_end:

.p2align 4
.globl nexs_embedded_dtb_start
nexs_embedded_dtb_start:
.incbin NEXS_EFI_DTB_IMAGE
.globl nexs_embedded_dtb_end
nexs_embedded_dtb_end:
