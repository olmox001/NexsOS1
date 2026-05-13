#include <arch/arch.h>
#include <arch/platform.h>
#include <drivers/virtio.h>
#include <kernel/hal.h>


#define MAX_VIRTIO_DEVS 16
static struct virtio_device virtio_devices[MAX_VIRTIO_DEVS];
static int virtio_dev_count = 0;

/* MMIO Implementation of Transport Ops */
static uint32_t mmio_read32(struct virtio_device *dev, uint32_t offset) {
    return hal_read32(dev->base + offset);
}

static void mmio_write32(struct virtio_device *dev, uint32_t offset, uint32_t val) {
    hal_write32(dev->base + offset, val);
}

static void mmio_notify(struct virtio_device *dev, uint32_t queue_idx) {
    mmio_write32(dev, VIRTIO_MMIO_QUEUE_NOTIFY, queue_idx);
}

static const struct virtio_transport_ops mmio_ops = {
    .read32 = mmio_read32,
    .write32 = mmio_write32,
    .notify = mmio_notify
};

void arch_virtio_scan(void) {
    virtio_dev_count = 0;
    for (int i = 0; i < VIRTIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        uint32_t magic = hal_read32(base + VIRTIO_MMIO_MAGIC_VALUE);
        
        if (magic == 0x74726976) {
            uint32_t dev_id = hal_read32(base + VIRTIO_MMIO_DEVICE_ID);
            if (dev_id != 0 && virtio_dev_count < MAX_VIRTIO_DEVS) {
                virtio_devices[virtio_dev_count].base = base;
                virtio_devices[virtio_dev_count].irq = 48 + i;
                virtio_devices[virtio_dev_count].device_id = dev_id;
                virtio_devices[virtio_dev_count].ops = &mmio_ops;
                virtio_dev_count++;
            }
        }
    }
}

void virtio_setup_queue(virtio_handle_t dev, uint32_t queue_idx, uint64_t desc_addr, uint64_t avail_addr, uint64_t used_addr) {
    (void)avail_addr;
    (void)used_addr;
    virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
    // VIRTIO_MMIO_GUEST_PAGE_SIZE is 0x028
    hal_write32(dev->base + 0x028, 4096);
    virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_PFN, desc_addr >> 12);
}

int arch_virtio_get_count(uint32_t device_id) {
    int count = 0;
    for (int i = 0; i < virtio_dev_count; i++) {
        if (virtio_devices[i].device_id == device_id) count++;
    }
    return count;
}

int arch_virtio_get_device(uint32_t device_id, int index, virtio_handle_t *out_dev, uint32_t *out_irq) {
    int current = 0;
    for (int i = 0; i < virtio_dev_count; i++) {
        if (virtio_devices[i].device_id == device_id) {
            if (current == index) {
                if (out_dev) *out_dev = &virtio_devices[i];
                if (out_irq) *out_irq = virtio_devices[i].irq;
                return 0;
            }
            current++;
        }
    }
    return -1;
}
