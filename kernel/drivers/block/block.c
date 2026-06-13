/*
 * kernel/drivers/block/block.c
 * Block-device contract dispatcher (see kernel/include/kernel/block.h).
 *
 * Holds the single active backend and routes block_read/block_write to it.
 * The FS stack calls these; it never learns whether the bytes come from a
 * virtio-blk device or a RAM-backed ramdisk module.  ASTRA: the `block` seam
 * named in docs/ASTRA.md §3 (B1) — virtio-blk -> block -> fs provider -> VFS.
 */
#include <kernel/block.h>
#include <kernel/printk.h>

/* Single root block device.  Set once during boot (virtio-blk, then ramdisk
 * override if a module is present); never torn down.  Touched only on the
 * boot CPU before SMP/userland, so no lock is needed. */
static const struct block_dev *active;

void block_register(const struct block_dev *dev) {
  if (!dev || !dev->read) {
    pr_err("%s", "block: rejecting malformed block_dev\n");
    return;
  }
  active = dev;
  pr_info("block: active backend '%s'\n", dev->name ? dev->name : "?");
}

const char *block_active_name(void) {
  return active ? active->name : NULL;
}

int block_read(void *buf, uint64_t sector, uint32_t count) {
  if (!active || !active->read)
    return -1;
  return active->read(buf, sector, count);
}

int block_write(void *buf, uint64_t sector, uint32_t count) {
  if (!active || !active->write)
    return -1;
  return active->write(buf, sector, count);
}
