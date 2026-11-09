/*
 * kernel/HAL/arch/aarch64/hal.c
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
#include <hal/diagnostic_protocol.h>
#include <hal/device_name.h>
#include <hal/device_protocol.h>
#include <hal/pci.h>
#include <arch/pt_regs.h>
#include <hal/core.h>
#include <hal/provider.h>
#include <kernel/hal.h>
#include <kernel/platform.h>
#include <arch/platform.h>
#include <drivers/virtio.h>

/* QEMU virt PCIe ECAM (compatible "pci-host-ecam-generic"). Base taken from the
 * board DTB pcie node: reg = <0x40 0x10000000 ...> -> 0x4010000000, 256 MB,
 * bus-range 0..255. Standard ECAM addressing: base + (bus<<20|dev<<15|fn<<12).
 *
 * NOTE(ASTRA-VIOLATION): base is hardcoded for the QEMU virt board; derive it
 * from the FDT pcie node once an FDT parser exists. Only the first buses are
 * populated on virt, so we map and bound the scan to a 16 MB window. */
#define VIRT_ECAM_BASE UINT64_C(0x4010000000)
#define VIRT_ECAM_BUSES NXHAL_PCI_BUS_COUNT

static uint32_t ecam_off(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
  return ((uint32_t)bus << 20) | ((uint32_t)dev << 15) |
         ((uint32_t)fn << 12) | (off & 0xFFC);
}
static uint32_t ecam_read32(uint8_t bus, uint8_t dev, uint8_t fn,
                            uint8_t off) {
  uint32_t value;

  if (hal_mmio_read32(VIRT_ECAM_BASE + ecam_off(bus, dev, fn, off),
                      &value) != 0)
    return NXHAL_PCI_CONFIG_INVALID;
  return value;
}

static void ecam_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                         uint8_t off, uint32_t value) {
  if (hal_mmio_write32(VIRT_ECAM_BASE + ecam_off(bus, dev, fn, off),
                       value) != 0)
    nxhal_diag_error("%s", "HAL: PCI ECAM write rejected\n");
}
static const struct nxhal_pci_config_ops ecam_ops = { ecam_read32, ecam_write32 };

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
static int aarch64_publish_virtio_slot(uint32_t slot) {
    struct nxhal_device_desc dev = {0};
    uint64_t base = (uint64_t)VIRTIO_MMIO_BASE +
                    ((uint64_t)slot * (uint64_t)VIRTIO_MMIO_STRIDE);
    uint32_t magic;
    uint32_t device_id;

    if (hal_mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE, &magic) != 0)
        return -1;
    if (magic != UINT32_C(0x74726976))
        return 0;
    if (hal_mmio_read32(base + VIRTIO_MMIO_DEVICE_ID, &device_id) != 0)
        return -1;
    if (device_id == 0U)
        return 0;
    dev.bus_type = NXHAL_BUS_VIRTIO_MMIO;
    dev.vendor_id = UINT16_C(0x1af4);
    dev.device_id = (uint16_t)device_id;
    dev.base = (uintptr_t)base;
    dev.irq = UINT32_C(48) + slot;
    nxhal_device_name_virtio(dev.name, sizeof(dev.name), device_id);
    nxhal_device_publish(&dev);
    return 0;
}

void nxhal_provider_bus_scan(void) {
    uint32_t slot;

    nxhal_diag_info("%s", "HAL: Scanning Platform Bus (MMIO)...\n");
    for (slot = 0U; slot < (uint32_t)VIRTIO_COUNT; ++slot) {
        if (aarch64_publish_virtio_slot(slot) != 0) {
            nxhal_diag_error("%s", "HAL: VirtIO MMIO probe failed\n");
            return;
        }
    }

    /* Unify with amd64: bring up the PCIe ECAM bus so the same PCI providers
     * (xHCI/VGA/...) enumerate here too. virtio stays on MMIO above; this adds
     * the PCI side (e.g. -device qemu-xhci) behind the same HAL registry. */
    if (hal_vmm_map_device(VIRT_ECAM_BASE,
                           (uint64_t)VIRT_ECAM_BUSES << 20U) != 0) {
        nxhal_diag_error("%s", "HAL: PCI ECAM mapping failed\n");
        return;
    }
    if (nxhal_pci_set_config_ops(&ecam_ops) != 0) {
        nxhal_diag_error("%s", "HAL: PCI ECAM backend registration failed\n");
        return;
    }
    /* Temporary QEMU-virt window source. This is replaced by the HCD/FDT
     * resource record in the next hardware-resource migration step. */
    if (nxhal_pci_set_mmio_windows(0x10000000UL, 0x2eff0000UL,
                                   0x8000000000UL, 0x8000000000UL) != 0 ||
        nxhal_pci_scan_and_register() != 0)
        nxhal_diag_error("%s", "HAL: PCI resource scan failed\n");
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
void nxhal_provider_irq_init(void) {
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
void nxhal_provider_timer_global_init(void) {
    extern void timer_init(void);
    timer_init();
}

void nxhal_provider_uaccess_enter(uint64_t user_root_phys,
                                  struct nxhal_uaccess_hw_state *state) {
    if (state == NULL)
        return;
    state->saved_root = hal_vmm_get_pgd();
    hal_vmm_set_pgd(user_root_phys);
    hal_tlb_flush_all();
    hal_isb();
}

void nxhal_provider_uaccess_exit(
    const struct nxhal_uaccess_hw_state *state) {
    if (state == NULL)
        return;
    hal_vmm_set_pgd(state->saved_root);
    hal_tlb_flush_all();
    hal_isb();
}

void nxhal_provider_uaccess_fault_abort(void) {
    /* Preserve the historical aarch64 fixup behaviour: allow IRQ progress
     * before the scheduler restores the successor frame. */
    hal_irq_enable();
}
