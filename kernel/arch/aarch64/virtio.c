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

#define MAX_VIRTIO_DEVS 8
struct virtio_mmio_dev {
    uintptr_t base;
    uint32_t irq;
    uint32_t device_id;
};
static struct virtio_mmio_dev virtio_devices[MAX_VIRTIO_DEVS];
static int virtio_dev_count = 0;

void arch_virtio_scan(void) {
    virtio_dev_count = 0;
    for (int i = 0; i < VIRTIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        if (virtio_read_reg(base, VIRTIO_MMIO_MAGIC_VALUE) == 0x74726976) {
            uint32_t dev_id = virtio_read_reg(base, VIRTIO_MMIO_DEVICE_ID);
            if (dev_id != 0 && virtio_dev_count < MAX_VIRTIO_DEVS) {
                virtio_devices[virtio_dev_count].base = base;
                virtio_devices[virtio_dev_count].irq = 48 + i;
                virtio_devices[virtio_dev_count].device_id = dev_id;
                virtio_dev_count++;
            }
        }
    }
}

/* Unified Hardware Discovery */

void virtio_setup_queue(uintptr_t base, uint32_t queue_idx, uint64_t desc_addr, uint64_t avail_addr, uint64_t used_addr) {
    (void)avail_addr;
    (void)used_addr;
    virtio_write_reg(base, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
    virtio_write_reg(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    virtio_write_reg(base, VIRTIO_MMIO_QUEUE_PFN, desc_addr >> 12);
}

int arch_virtio_get_count(uint32_t device_id) {
    int count = 0;
    for (int i = 0; i < virtio_dev_count; i++) {
        if (virtio_devices[i].device_id == device_id) count++;
    }
    return count;
}

int arch_virtio_get_device(uint32_t device_id, int index, uintptr_t *out_base, uint32_t *out_irq) {
    int current = 0;
    for (int i = 0; i < virtio_dev_count; i++) {
        if (virtio_devices[i].device_id == device_id) {
            if (current == index) {
                if (out_base) *out_base = virtio_devices[i].base;
                if (out_irq) *out_irq = virtio_devices[i].irq;
                return 0;
            }
            current++;
        }
    }
    return -1;
}

uintptr_t arch_virtio_register_pci(uint32_t bdf) {
    (void)bdf;
    return 0;
}
