#include <kernel/arch.h>
#include <drivers/virtio.h>

uint32_t virtio_read_reg(uintptr_t base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

void virtio_write_reg(uintptr_t base, uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(base + offset) = val;
}

void virtio_notify(uintptr_t base, uint32_t queue_idx) {
    virtio_write_reg(base, VIRTIO_MMIO_QUEUE_NOTIFY, queue_idx);
}

int arch_virtio_probe(uint32_t device_id, uintptr_t *out_base, uint32_t *out_irq) {
    for (int i = 0; i < VIRTIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        if (virtio_read_reg(base, VIRTIO_MMIO_MAGIC_VALUE) == 0x74726976) {
            if (virtio_read_reg(base, VIRTIO_MMIO_DEVICE_ID) == device_id) {
                if (out_base) *out_base = base;
                if (out_irq) *out_irq = 48 + i; // QEMU virt machine IRQs start at 32 + 16
                return 0;
            }
        }
    }
    return -1;
}

uintptr_t arch_virtio_register_pci(uint32_t bdf) {
    (void)bdf;
    return 0;
}
