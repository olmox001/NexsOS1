/*
 * kernel/arch/aarch64/hal.c
 * AArch64 Hardware Abstraction Layer — platform bus scan, IRQ, and timer init.
 *
 * Role:
 *   Implements the three arch_* hooks that the generic kernel calls during
 *   device discovery:
 *   - arch_bus_scan(): probes VirtIO MMIO slots and registers found devices
 *     in the HAL device registry (hal_bus.c).
 *   - arch_irq_init(): delegates to gic_register() to install the GIC driver
 *     into the kernel IRQ controller abstraction.
 *   - arch_timer_init(): delegates to timer_init() to program the ARM Generic
 *     Timer (sys_timer.c) for the kernel tick.
 *
 * HAL over-abstraction note (HAL-01):
 *   hal_read32(base) expands to a compound-literal hal_device_t construction +
 *   heuristic bus-type detection + indirect call through hal_dev_read32.
 *   For a hot MMIO read this is significantly more expensive than a single
 *   memory-mapped load.  arch_bus_scan() calls hal_read32 only twice per slot
 *   (magic + device_id) during boot, so the overhead here is acceptable.
 *   Drivers that call hal_read32 in interrupt handlers should be aware of
 *   the overhead (see HAL-01 in docs/review/analysis/02-boot-arch-hal.md).
 */
#include <kernel/hal.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/platform.h>
#include <arch/platform.h>
#include <drivers/virtio.h>
#include <drivers/pci.h>

/* QEMU virt PCIe ECAM (compatible "pci-host-ecam-generic"). Base taken from the
 * board DTB pcie node: reg = <0x40 0x10000000 ...> -> 0x4010000000, 256 MB,
 * bus-range 0..255. Standard ECAM addressing: base + (bus<<20|dev<<15|fn<<12).
 *
 * NOTE(ASTRA-VIOLATION): base is hardcoded for the QEMU virt board; derive it
 * from the FDT pcie node once an FDT parser exists. Only the first buses are
 * populated on virt, so we map and bound the scan to a 16 MB window. */
#define VIRT_ECAM_BASE  0x4010000000UL
#define VIRT_ECAM_BUSES 16

static uint32_t ecam_off(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
  return ((uint32_t)bus << 20) | ((uint32_t)dev << 15) |
         ((uint32_t)fn << 12) | (off & 0xFFC);
}
static uint32_t ecam_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
  if (bus >= VIRT_ECAM_BUSES) return 0xFFFFFFFF;
  return *(volatile uint32_t *)phys_to_virt(VIRT_ECAM_BASE + ecam_off(bus, dev, fn, off));
}
static void ecam_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
  if (bus >= VIRT_ECAM_BUSES) return;
  *(volatile uint32_t *)phys_to_virt(VIRT_ECAM_BASE + ecam_off(bus, dev, fn, off)) = v;
}
static struct pci_config_ops ecam_ops = { ecam_read32, ecam_write32 };

/* arch_platform_get_boot_module (HAL contract, platform.h): aarch64 boots a
 * raw kernel + a virtio-blk disk, with no GRUB/initrd module today.  Returns
 * 0; a future DTB /chosen initrd would slot in here, keeping the ramdisk
 * block backend identical to amd64. */
int arch_platform_get_boot_module(uint64_t *base, uint64_t *size) {
  (void)base;
  (void)size;
  return 0;
}

/*
 * arch_bus_scan - probe VirtIO MMIO slots and register discovered devices.
 *
 * VirtIO MMIO layout on QEMU virt (aarch64):
 *   Base address: VIRTIO_MMIO_BASE (platform.h, typically 0x0A000000).
 *   Each device slot occupies VIRTIO_MMIO_STRIDE bytes (typically 0x200 = 512B).
 *   Number of slots: VIRTIO_COUNT (typically 32).
 *
 * Discovery sequence per slot:
 *   1. Read VIRTIO_MMIO_MAGIC_VALUE at offset 0x000.
 *      Valid VirtIO devices respond with 0x74726976 ("virt" in little-endian).
 *   2. If magic matches, read VIRTIO_MMIO_DEVICE_ID at offset 0x008.
 *      Device ID 0 means the slot is empty; skip it.
 *   3. Populate a hal_device descriptor and call hal_register_device().
 *
 * IRQ assignment: the QEMU virt board maps VirtIO MMIO IRQs starting at 48
 * (SPI 16 in GIC terms); device i uses IRQ 48 + i.
 * Vendor ID 0x1AF4 is the standard VirtIO PCI/MMIO vendor.
 */
void arch_bus_scan(void) {
    pr_info("%s", "HAL: Scanning Platform Bus (MMIO)...\n");

    for (int i = 0; i < VIRTIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        /* Read the VirtIO magic value; valid devices return "virt" (0x74726976). */
        uint32_t magic = hal_read32(base + VIRTIO_MMIO_MAGIC_VALUE);

        if (magic == 0x74726976) { /* "virt" in little-endian ASCII */
            uint32_t dev_id = hal_read32(base + VIRTIO_MMIO_DEVICE_ID);
            if (dev_id != 0) { /* device_id == 0 means slot is empty */
                struct hal_device dev;
                memset(&dev, 0, sizeof(dev));

                dev.bus_type  = HAL_BUS_TYPE_VIRTIO_MMIO;
                dev.vendor_id = 0x1AF4;           /* VirtIO standard vendor */
                dev.device_id = (uint16_t)dev_id; /* e.g. 1=net, 2=blk, 9=gpu */
                dev.base      = base;
                dev.irq       = 48 + i;           /* QEMU virt IRQ allocation */

                snprintf(dev.name, sizeof(dev.name), "VirtIO-%d", dev_id);

                hal_register_device(&dev);
            }
        }
    }

    /* Unify with amd64: bring up the PCIe ECAM bus so the same PCI providers
     * (xHCI/VGA/...) enumerate here too. virtio stays on MMIO above; this adds
     * the PCI side (e.g. -device qemu-xhci) behind the same HAL registry. */
    extern int arch_vmm_map_device(uint64_t base, uint64_t size);
    arch_vmm_map_device(VIRT_ECAM_BASE, (uint64_t)VIRT_ECAM_BUSES << 20);
    pci_set_config_ops(&ecam_ops);
    /* PCI MMIO windows from the virt DTB pcie ranges (32-bit @ 0x10000000,
     * 64-bit @ 0x80_00000000), used to assign BARs the (absent) firmware did
     * not program. */
    pci_set_mmio_windows(0x10000000UL, 0x2eff0000UL,
                         0x8000000000UL, 0x8000000000UL);
    pci_scan_and_register();
}

/*
 * arch_irq_init - install the platform interrupt controller driver.
 *
 * Calls gic_register() to register the GIC (Generic Interrupt Controller)
 * irq_ctrl_ops with the kernel IRQ subsystem.  The actual GIC hardware
 * initialisation (GICD enable, per-CPU GICC/GICR setup) happens inside
 * gic_register() or the GIC driver's init path.
 *
 * Must be called before any IRQ is enabled or any driver calls request_irq().
 */
void arch_irq_init(void) {
    extern void gic_register(void);
    gic_register();
}

/*
 * arch_timer_init - initialise the system tick timer.
 *
 * Delegates to timer_init() (kernel/drivers/sys_timer.c), which programs the
 * ARM Generic Timer (CNTP_CTL_EL0 / CNTP_TVAL_EL0 or equivalent) to fire
 * periodic interrupts for the kernel scheduler tick.
 */
void arch_timer_init(void) {
    extern void timer_init(void);
    timer_init();
}
