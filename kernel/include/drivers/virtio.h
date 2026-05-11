/*
 * kernel/include/drivers/virtio.h
 * VirtIO Common Definitions
 */
#ifndef _DRIVERS_VIRTIO_H
#define _DRIVERS_VIRTIO_H

#include <kernel/types.h>

/* VirtIO MMIO Register Layout */
#define VIRTIO_MMIO_MAGIC_VALUE 0x000
#define VIRTIO_MMIO_VERSION 0x004
#define VIRTIO_MMIO_DEVICE_ID 0x008
#define VIRTIO_MMIO_VENDOR_ID 0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_SEL 0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM 0x038
#define VIRTIO_MMIO_QUEUE_ALIGN 0x03c
#define VIRTIO_MMIO_QUEUE_PFN 0x040
#define VIRTIO_MMIO_QUEUE_READY 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064
#define VIRTIO_MMIO_STATUS 0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0a0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0fc
#define VIRTIO_MMIO_CONFIG 0x100

/* VirtIO Device IDs */
#define VIRTIO_DEV_NET 1
#define VIRTIO_DEV_BLOCK 2
#define VIRTIO_DEV_CONSOLE 3
#define VIRTIO_DEV_GPU 16
#define VIRTIO_DEV_INPUT 18

/* VirtIO Status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED 128

/* QEMU virt machine MMIO base addresses */
#define VIRTIO_MMIO_BASE 0x0a000000
#define VIRTIO_MMIO_SIZE 0x200
#define VIRTIO_MMIO_STRIDE 0x200
#define VIRTIO_COUNT 32

/* VirtIO Ring Descriptors */
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2
#define VRING_DESC_F_INDIRECT 4

struct vring_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct vring_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[];
};

struct vring_used_elem {
  uint32_t id;
  uint32_t len;
};

struct vring_used {
  uint16_t flags;
  uint16_t idx;
  struct vring_used_elem ring[];
};

/* Arch abstraction */
uint32_t virtio_read_reg(uintptr_t base, uint32_t offset);
void virtio_write_reg(uintptr_t base, uint32_t offset, uint32_t value);
void virtio_notify(uintptr_t base, uint32_t queue_idx);

uintptr_t arch_virtio_register_pci(uint32_t bdf);
void arch_virtio_scan(void);
void virtio_setup_queue(uintptr_t base, uint32_t queue_idx, uint64_t desc,
                        uint64_t avail, uint64_t used);
int arch_virtio_get_count(uint32_t device_id);
int arch_virtio_get_device(uint32_t device_id, int index, uintptr_t *out_base,
                           uint32_t *out_irq);

#endif /* _DRIVERS_VIRTIO_H */
