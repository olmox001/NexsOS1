/*
 * kernel/include/kernel/block.h
 * Block-device contract (ASTRA seam: virtio-blk -> block -> fs provider -> VFS).
 *
 * The FS stack (GPT, ext4, buffer cache) consumes ONLY this contract; it never
 * names a concrete backend.  Today's backends are virtio-blk (the real device,
 * `make run` and the dev loop) and the RAM-backed ramdisk (a GRUB multiboot2
 * module, the self-contained release ISO).  Sectors are 512 bytes.
 */
#ifndef _KERNEL_BLOCK_H
#define _KERNEL_BLOCK_H

#include <kernel/types.h>

struct block_dev {
  const char *name;
  int (*read)(void *buf, uint64_t sector, uint32_t count);
  int (*write)(void *buf, uint64_t sector, uint32_t count);
};

/* Make 'dev' the active block backend (last registration wins).  main.c
 * registers virtio-blk first and lets the ramdisk override it only when a
 * boot module is present, so the dev loop keeps using the real device. */
void block_register(const struct block_dev *dev);

/* Name of the active backend, or NULL if none registered (for logging). */
const char *block_active_name(void);

/* Read/write 'count' 512-byte sectors at 'sector' through the active backend.
 * Return 0 on success, negative on failure (no backend -> failure). */
int block_read(void *buf, uint64_t sector, uint32_t count);
int block_write(void *buf, uint64_t sector, uint32_t count);

#endif /* _KERNEL_BLOCK_H */
