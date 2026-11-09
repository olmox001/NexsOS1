/*
 * kernel/HAL/arch/amd64/hal.c
 * AMD64 Hardware Abstraction Layer — PCI Bus Scan and Timer Init
 *
 * Responsibilities:
 *   - amd64_pci_callback: called by pci_enumerate for each discovered PCI
 *     function; enables Bus Master + I/O + Memory access, reads BAR0 and BAR4,
 *     translates VirtIO device IDs, assigns the IRQ, and registers the device
 *     with the HAL device table via hal_register_device.
 *   - arch_bus_scan: invokes pci_enumerate(amd64_pci_callback) to build the
 *     HAL device list.
 *   - arch_irq_init: initialises the legacy 8259 PIC.
 *   - arch_timer_init: runs LAPIC calibration once on the BSP.
 *   - timer_init_percpu: starts the per-CPU LAPIC periodic timer at HZ.
 *
 * Known issues:
 *   DRV-VIRTIO-01 (W5 BUG) pci_get_bar (pci.c:106) returns uint32_t.  For
 *     64-bit BAR entries (type bits [2:1] = 0b10), the high 32 bits of the
 *     MMIO base address are in the NEXT BAR register and are not read.  When
 *     QEMU places the virtio-blk BAR above 4 GB ('-m 4G'), the low 32 bits
 *     returned by nxhal_pci_get_bar(bdf, 4) are 0 or an alias, so dev->base is
 *     wrong.  The virtio driver reads QUEUE_NUM_MAX from the wrong address and
 *     gets 0 → "Invalid queue size (0)!" → downstream divide-by-zero crash.
 *     Cross-ref: DRV-PCI-02 (pci.c:106-111).
 *     Fix: in amd64_pci_callback, after reading b4, check bits [2:1]; if 10b,
 *     read BAR5 and compose a uint64_t base address.
 */
#include <hal/diagnostic_protocol.h>
#include <hal/device_name.h>
#include <hal/device_protocol.h>
#include <hal/pci.h>
#include <arch/pt_regs.h>
#include <hal/core.h>
#include <hal/provider.h>
#include <arch/amd64/apic.h>
#include <drivers/timer.h>
#include <kernel/multiboot2.h>
#include <kernel/platform.h>

/* arch_platform_get_boot_module (HAL contract, platform.h): walk the GRUB
 * multiboot2 tag chain for a MODULE (the release rootfs disk.img loaded into
 * RAM).  Cached on first call (invoked early, before the identity map is torn
 * down) so later callers — the ramdisk backend after vmm_init — still get it.
 * mb_magic / arch_get_boot_info() come from this arch's boot stub. */
extern uint64_t mb_magic;
uint64_t arch_get_boot_info(void);

int arch_platform_get_boot_module(uint64_t *base, uint64_t *size) {
  static int probed = 0;
  static uint64_t mod_base, mod_size;
  if (!probed) {
    probed = 1;
    if (mb_magic == MB2_MAGIC) {
      uint8_t *d = (uint8_t *)arch_get_boot_info();
      if (d) {
        struct mb2_tag *tag = (struct mb2_tag *)(d + 8);
        while (tag->type != MB2_TAG_TYPE_END) {
          if (tag->type == MB2_TAG_TYPE_MODULE) {
            struct mb2_tag_module *m = (struct mb2_tag_module *)tag;
            mod_base = m->mod_start;
            mod_size = (uint64_t)m->mod_end - m->mod_start;
            break;
          }
          tag = (struct mb2_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7u));
        }
      }
    }
  }
  if (mod_base) {
    if (base)
      *base = mod_base;
    if (size)
      *size = mod_size;
    return 1;
  }
  return 0;
}

static void amd64_pci_fill_identity(struct nxhal_device_desc *dev,
                                    uint32_t bdf, uint16_t vendor,
                                    uint16_t device_id) {
  uint32_t class_word = nxhal_pci_get_class(bdf);

  dev->bus_type = NXHAL_BUS_PCI;
  dev->vendor_id = vendor;
  dev->device_id = device_id;
  dev->pci_bdf = bdf;
  dev->class_code = (uint8_t)(class_word >> 24U);
  dev->subclass = (uint8_t)(class_word >> 16U);
  dev->prog_if = (uint8_t)(class_word >> 8U);
  dev->header_type = nxhal_pci_get_header_type(bdf);
}

static int amd64_pci_prepare_bar(uint32_t bdf, int modern,
                                 struct nxhal_device_desc *dev) {
  struct nxhal_pci_bar bar;
  uint32_t index = modern != 0 ? 4U : 0U;

  if (nxhal_pci_get_bar(bdf, index, &bar) != 0)
    return -1;
  dev->base = (uintptr_t)bar.base;
  if (bar.is_io == 0U && bar.base != 0U && bar.size != 0U &&
      hal_vmm_map_device(bar.base, bar.size) != 0)
    return -1;
  return 0;
}

static void amd64_pci_enable(uint32_t bdf) {
  uint8_t bus = (uint8_t)(bdf >> 16U);
  uint8_t device = (uint8_t)(bdf >> 8U);
  uint8_t function = (uint8_t)bdf;
  uint32_t command = nxhal_pci_config_read(bus, device, function, 0x04U);

  nxhal_pci_config_write(bus, device, function, 0x04U,
                         command | UINT32_C(7));
}

static void amd64_pci_translate_virtio(struct nxhal_device_desc *dev,
                                       uint16_t raw_device_id) {
  uint8_t bus;
  uint8_t device;
  uint8_t function;
  uint32_t subsystem;

  if (dev->vendor_id != UINT16_C(0x1af4))
    return;
  if (raw_device_id >= UINT16_C(0x1041)) {
    dev->device_id = (uint16_t)(raw_device_id - UINT16_C(0x1040));
    return;
  }
  bus = (uint8_t)(dev->pci_bdf >> 16U);
  device = (uint8_t)(dev->pci_bdf >> 8U);
  function = (uint8_t)dev->pci_bdf;
  subsystem = nxhal_pci_config_read(bus, device, function, UINT8_C(0x2c));
  dev->device_id = (uint16_t)(subsystem >> 16U);
}

static uint32_t amd64_pci_irq(uint32_t bdf) {
  uint8_t line = nxhal_pci_get_interrupt(bdf);

  if (line != UINT8_C(0xff) && line < UINT8_C(16))
    return UINT32_C(32) + line;
  nxhal_diag_warn("HAL: PCI %02x:%02x.%x lacks usable INTx line 0x%x\n",
                  (unsigned)(bdf >> 16U), (unsigned)((bdf >> 8U) & 0xffU),
                  (unsigned)(bdf & 7U), (unsigned)line);
  return UINT32_C(224) + (bdf & UINT32_C(7));
}

static int amd64_pci_callback(uint32_t bdf, uint16_t vendor,
                              uint16_t device_id) {
  struct nxhal_device_desc dev = {0};
  int modern = vendor == UINT16_C(0x1af4) &&
               device_id >= UINT16_C(0x1041);

  amd64_pci_fill_identity(&dev, bdf, vendor, device_id);
  amd64_pci_enable(bdf);
  if (amd64_pci_prepare_bar(bdf, modern, &dev) != 0)
    return -1;
  amd64_pci_translate_virtio(&dev, device_id);
  dev.irq = amd64_pci_irq(bdf);
  if (vendor == UINT16_C(0x1af4))
    nxhal_device_name_virtio(dev.name, sizeof(dev.name), dev.device_id);
  else
    nxhal_device_name_pci(dev.name, sizeof(dev.name), vendor, device_id);
  nxhal_device_publish(&dev);
  return 0;
}

void nxhal_provider_bus_scan(void) {
  nxhal_diag_info("%s", "HAL: Scanning PCI Bus...\n");
  if (nxhal_pci_enumerate(amd64_pci_callback) != 0)
    nxhal_diag_error("%s", "HAL: PCI enumeration failed\n");
}

void nxhal_provider_irq_init(void) {
  extern void pic_init(void);
  pic_init();
}

void nxhal_provider_timer_global_init(void) {
  /* BSP Global Timer initialization.
   * We perform LAPIC calibration here once using the legacy PIT. */
  nxhal_diag_info("HAL: Initializing global timer state (LAPIC calibration)\n");
  lapic_timer_calibrate();
}

void nxhal_provider_timer_percpu_init(void) {
  /* Initialize local APIC timer for the current CPU at configured HZ.
   * This is called by every CPU during its local initialization. */
  lapic_timer_setup(HZ);
}

void nxhal_provider_uaccess_enter(uint64_t user_root_phys,
                                  struct nxhal_uaccess_hw_state *state) {
  (void)user_root_phys;
  if (state != NULL)
    state->saved_root = 0U;
}

void nxhal_provider_uaccess_exit(
    const struct nxhal_uaccess_hw_state *state) {
  (void)state;
}

void nxhal_provider_uaccess_fault_abort(void) {
  /* amd64 keeps the fault path IRQ state masked until the restored frame. */
}
