.section .rdata,"dr"
.p2align 4
.globl nexs_embedded_kernel_start
nexs_embedded_kernel_start:
.incbin "build/aarch64/kernel.bin"
.globl nexs_embedded_kernel_end
nexs_embedded_kernel_end:

.p2align 4
.globl nexs_embedded_dtb_start
nexs_embedded_dtb_start:
.incbin "build/aarch64/virt.dtb"
.globl nexs_embedded_dtb_end
nexs_embedded_dtb_end:
