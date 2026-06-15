/*
 * kernel/drivers/block/ramdisk.c
 * RAM-backed block backend over a firmware boot module (the release rootfs).
 *
 * ASTRA: a provider behind the `block` contract (kernel/block.h), fully
 * arch-neutral.  The arch-specific "is there a boot module, and where" lives
 * behind the HAL contract arch_platform_get_boot_module() (platform.h),
 * implemented equivalently per arch — so this driver carries no #ifdef.  On
 * aarch64 the HAL reports no module and this provider stays inert.
 */
#include <kernel/block.h>
#include <kernel/bootmodule.h>
#include <kernel/memlayout.h>
#include <kernel/platform.h>
#include <kernel/printk.h>
#include <kernel/string.h>

/* Module extent (physical), 0 = none. */
static uint64_t mod_base;
static uint64_t mod_size;

/* The module bytes are reachable through the kernel direct map (phys_to_virt);
 * the region is reserved before the PMM (main.c) so it is never reused. */
static int ramdisk_io(void *buf, uint64_t sector, uint32_t count, int write) {
  uint64_t off = sector * 512ULL;
  uint64_t len = (uint64_t)count * 512ULL;
  if (!mod_base || off + len > mod_size)
    return -1; /* outside the module's bounds */
  uint8_t *disk = (uint8_t *)phys_to_virt(mod_base) + off;
  if (write)
    memcpy(disk, buf, len); /* RAM-backed: writes are volatile (lost on reboot) */
  else
    memcpy(buf, disk, len);
  return 0;
}

static int ramdisk_read(void *buf, uint64_t sector, uint32_t count) {
  return ramdisk_io(buf, sector, count, 0);
}

static int ramdisk_write(void *buf, uint64_t sector, uint32_t count) {
  return ramdisk_io(buf, sector, count, 1);
}

void ramdisk_init(void) {
  if (!arch_platform_get_boot_module(&mod_base, &mod_size))
    return;
  static const struct block_dev ramdisk_bdev = {"ramdisk", ramdisk_read,
                                                ramdisk_write};
  block_register(&ramdisk_bdev);
  pr_info("ramdisk: rootfs module at phys 0x%lx, %lu KB\n", mod_base,
          mod_size / 1024);
}
