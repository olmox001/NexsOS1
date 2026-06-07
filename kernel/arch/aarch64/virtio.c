/*
 * kernel/arch/aarch64/virtio.c
 * AArch64 VirtIO MMIO transport layer.
 *
 * Role:
 *   Implements the arch_virtio_* interface for the VirtIO MMIO transport used
 *   on the QEMU virt AArch64 machine.  Provides:
 *   - arch_virtio_scan(): enumerates all VirtIO MMIO device slots and populates
 *     the internal virtio_devices[] table.
 *   - arch_virtio_get_count() / arch_virtio_get_device(): query the table by
 *     device type ID (e.g. 2 = block, 1 = net, 9 = GPU).
 *   - virtio_setup_queue(): programs the MMIO registers to configure a virtqueue
 *     (VirtIO legacy MMIO spec, version 1).
 *
 * Transport ops:
 *   mmio_ops is a static virtio_transport_ops struct providing read32/write32/notify
 *   over hal_read32/hal_write32.  Every virtio_device installed by arch_virtio_scan
 *   points to this single shared ops table.
 *
 * MMIO register layout (VirtIO 1.0 MMIO, offsets from device base):
 *   0x000  MagicValue   (R)  — must be 0x74726976 ("virt").
 *   0x004  Version      (R)  — 1 = legacy, 2 = modern.
 *   0x008  DeviceID     (R)  — device type (0 = empty slot).
 *   0x00C  VendorID     (R)  — 0x554D4551 (QEMU) or 0x1AF4.
 *   0x020  QueueSel     (W)  — select active queue.
 *   0x024  QueueNumMax  (R)  — max queue size for selected queue.
 *   0x028  GuestPageSize(W)  — page size (legacy only; must be 4096 for 4KB).
 *   0x030  QueuePFN     (W)  — page-frame number of virtqueue (legacy).
 *   0x050  QueueNotify  (W)  — kick selected queue.
 *
 * NOTE: virtio_setup_queue() uses the VirtIO legacy MMIO interface (QueuePFN /
 * GuestPageSize) rather than the split-queue modern interface.  The avail_addr
 * and used_addr parameters are silently ignored — they are relevant only for
 * the modern interface.  This works on QEMU virt because QEMU defaults to
 * supporting the legacy interface when QueuePFN is used.
 */
#include <arch/arch.h>
#include <arch/platform.h>
#include <drivers/virtio.h>
#include <kernel/hal.h>

/*
 * virtio_devices[] - table of discovered VirtIO MMIO devices.
 * Populated by arch_virtio_scan(); indexed by discovery order.
 * Supports up to MAX_VIRTIO_DEVS non-empty device slots.
 */
#define MAX_VIRTIO_DEVS 16
static struct virtio_device virtio_devices[MAX_VIRTIO_DEVS];
static int virtio_dev_count = 0;

/* MMIO Implementation of Transport Ops.
 * These three functions form the virtio_transport_ops for every MMIO device.
 * They delegate to hal_read32/hal_write32 which perform a 32-bit MMIO access
 * at the device's base address + the given register offset. */

/* mmio_read32 - read a 32-bit MMIO register from a VirtIO device. */
static uint32_t mmio_read32(struct virtio_device *dev, uint32_t offset) {
    return hal_read32(dev->base + offset);
}

/* mmio_write32 - write a 32-bit value to a VirtIO device MMIO register. */
static void mmio_write32(struct virtio_device *dev, uint32_t offset, uint32_t val) {
    hal_write32(dev->base + offset, val);
}

/* mmio_notify - kick a virtqueue by writing its index to QUEUE_NOTIFY.
 * This signals the device that new descriptor chains are available in the
 * queue's available ring. */
static void mmio_notify(struct virtio_device *dev, uint32_t queue_idx) {
    mmio_write32(dev, VIRTIO_MMIO_QUEUE_NOTIFY, queue_idx);
}

/* Shared transport ops table; all MMIO devices point here. */
static const struct virtio_transport_ops mmio_ops = {
    .read32  = mmio_read32,
    .write32 = mmio_write32,
    .notify  = mmio_notify
};

/*
 * arch_virtio_scan - enumerate VirtIO MMIO device slots and fill virtio_devices[].
 *
 * Probes VIRTIO_COUNT slots starting at VIRTIO_MMIO_BASE, stepping by
 * VIRTIO_MMIO_STRIDE bytes per slot.  For each slot:
 *   - Reads MAGIC_VALUE (offset 0x000); non-matching value → skip.
 *   - Reads DEVICE_ID  (offset 0x008); 0 → empty slot, skip.
 *   - Records base, IRQ (48+i), device_id, and mmio_ops into virtio_devices[].
 *
 * IRQ: QEMU virt assigns SPI 0..31 to VirtIO MMIO slots 0..31.  GIC SPI 0
 * corresponds to IRQ 32 in the CPU interrupt numbering; however, QEMU's
 * virtio-mmio device uses IRQs 48..79 in the kernel's flat IRQ space
 * (GIC base 32 + platform-specific offset 16).
 *
 * Called once from the kernel device initialisation path; not thread-safe.
 * virtio_dev_count is reset to 0 at the start to support re-scan if needed.
 */
void arch_virtio_scan(void) {
    virtio_dev_count = 0;
    for (int i = 0; i < VIRTIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        uint32_t magic = hal_read32(base + VIRTIO_MMIO_MAGIC_VALUE);

        if (magic == 0x74726976) { /* "virt" little-endian: slot is a VirtIO device */
            uint32_t dev_id = hal_read32(base + VIRTIO_MMIO_DEVICE_ID);
            if (dev_id != 0 && virtio_dev_count < MAX_VIRTIO_DEVS) {
                virtio_devices[virtio_dev_count].base      = base;
                virtio_devices[virtio_dev_count].irq       = 48 + i;
                virtio_devices[virtio_dev_count].device_id = dev_id;
                virtio_devices[virtio_dev_count].ops       = &mmio_ops;
                virtio_dev_count++;
            }
        }
    }
}

/*
 * virtio_setup_queue - configure a legacy MMIO virtqueue for the given device.
 *
 * Parameters:
 *   dev        Handle to the VirtIO MMIO device (from arch_virtio_get_device).
 *   queue_idx  Index of the virtqueue to configure (0 = request queue for blk).
 *   desc_addr  Physical address of the virtqueue descriptor table.
 *              The legacy interface combines desc, avail, and used rings in a
 *              single contiguous buffer at this address; avail_addr and used_addr
 *              are not used and are silently discarded.
 *   avail_addr Ignored (legacy MMIO; rings are at fixed offsets from desc_addr).
 *   used_addr  Ignored (same reason).
 *
 * Register sequence (VirtIO 1.0, Section 4.2.3 — legacy MMIO):
 *   1. Write QueueSel (0x020) = queue_idx: select the target queue.
 *   2. Write GuestPageSize (0x028) = 4096: inform the device of our page size.
 *      This is a legacy-only register absent from the modern spec.
 *   3. Write QueuePFN (0x030) = desc_addr >> 12: the page frame number of the
 *      virtqueue buffer.  The device uses GuestPageSize to compute ring offsets.
 *
 * NOTE: The avail and used ring addresses are derived by the device from
 * QueuePFN and the queue size, per the legacy spec.  Any mismatch between
 * what the driver places at desc_addr and what the device computes leads to
 * silent descriptor corruption.  Callers must lay out the virtqueue exactly
 * as specified in VirtIO 1.0 Section 2.4 (legacy MMIO layout).
 */
void virtio_setup_queue(virtio_handle_t dev, uint32_t queue_idx, uint64_t desc_addr, uint64_t avail_addr, uint64_t used_addr) {
    (void)avail_addr; /* unused: legacy MMIO derives ring offsets from QueuePFN */
    (void)used_addr;  /* unused: same reason */
    virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
    /* VIRTIO_MMIO_GUEST_PAGE_SIZE is 0x028: tell device page size = 4096. */
    hal_write32(dev->base + 0x028, 4096);
    /* QueuePFN = physical frame number of the virtqueue buffer.
     * desc_addr >> 12 converts from byte address to 4KB page frame number. */
    virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_PFN, desc_addr >> 12);
}

/*
 * arch_virtio_get_count - count devices of a given VirtIO device type ID.
 *
 * Parameters:
 *   device_id  VirtIO device type (e.g. 1=net, 2=block, 9=GPU, 16=input).
 * Returns: number of devices of that type found by arch_virtio_scan().
 */
int arch_virtio_get_count(uint32_t device_id) {
    int count = 0;
    for (int i = 0; i < virtio_dev_count; i++) {
        if (virtio_devices[i].device_id == device_id) count++;
    }
    return count;
}

/*
 * arch_virtio_get_device - retrieve a specific VirtIO device by type and index.
 *
 * Parameters:
 *   device_id  VirtIO device type ID to search for.
 *   index      Zero-based index among devices of that type (0 = first found).
 *   out_dev    Output: pointer to the virtio_device handle, or unchanged if -1.
 *   out_irq    Output: IRQ number for the device, or unchanged if -1.
 * Returns: 0 if found, -1 if no device of that type at that index.
 */
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
