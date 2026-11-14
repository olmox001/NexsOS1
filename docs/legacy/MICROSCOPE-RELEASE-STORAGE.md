# MICROSCOPE — Release, boot & storage rework

> A microscopic, evidence-based plan for the release/boot/storage phase.
> Method: **ASTRA** (`docs/ASTRA.md`) — kernel core consumes contracts;
> every hardware/format is a *provider* behind a contract; arch dirs hold
> only ISA/boot-protocol glue.  Branch `comprehensive-review`.
> Paused work is archived in `docs/B3-POLISH-QUEUE.md`.

## 1. The bug (reproduced, root-caused)

`make ARCH=amd64 release-arch` builds a GRUB El-Torito ISO that boots the
kernel via `multiboot2 /boot/kernel.elf` and passes the rootfs as
`module2 /boot/disk.img diskimg`.  Booting the ISO (`-cdrom`, headless):

```
[INFO] VirtIO: No block device found
[INFO] Partition: Failed to read LBA 1
[INFO] GPT: Done.
[ERROR] VFS: no mountable filesystem found on any partition
[ERROR] ELF: File not found: /sys/bin/init
*** KERNEL PANIC ***  Failed to load /init
```

**Root cause:** the released ISO is self-contained, but the kernel only ever
looks for the rootfs on a **virtio-blk GPT/ext4 block device**.  In the ISO
boot there is none — the rootfs (`disk.img`) is a **multiboot2 module in RAM**
that the kernel never reads (`platform.c` counts `nr_modules` but does not
parse the MODULE tag).  So GPT finds nothing, VFS mounts nothing, `/sys/bin/init`
is absent → panic.  Exactly the reported "block device / disk.img partitions /
poorly-supported PCI hardware" failure.

Secondary issues observed:
- The release `dd`-zeroes the ISO's first 512 bytes ("to be readable from
  macOS"), which **destroys the hybrid MBR** → the ISO no longer boots as a
  hard disk (`virtio-blk`), only as El-Torito CD.  `test-release` then attaches
  it as `virtio-blk-pci`, which cannot boot → blank serial.
- `disk.img` is built by `mkdisk` as **bootloader + kernel + rootfs** (GPT) —
  the kernel is **redundant** in the module path (GRUB already loaded
  `kernel.elf`); it only bloats the module.

## 2. Target architecture (ASTRA)

The fix is to make the **GRUB module a block provider**, so the *existing*
GPT→ext4→VFS path mounts it with zero changes to the FS stack:

```
GRUB ──multiboot2──> kernel.elf
     └─module2──────> disk.img (standard GPT+ext4 rootfs, in RAM)
                          │
   platform (arch glue): parse MB2 MODULE tag → (mod_start, mod_end)
                          │
   ramdisk block provider (RAM-backed) registered behind the BLOCK contract
                          │
   GPT probe → ext4 mount → VFS root  ← unchanged, already works
```

Layering (simplicity at the centre, complexity at the edges):
- **Arch boot glue** (`arch/amd64/platform`): parse the multiboot2 MODULE tag,
  hand `(base,size)` to the core via a contract call.  aarch64 has no GRUB; it
  keeps the virtio-blk path (and later a DT/`-initrd` equivalent).
- **Block contract**: add a **ramdisk** block device (read-only, RAM-backed)
  that the GPT/buffer-cache layer consumes exactly like virtio-blk.  The kernel
  core never learns "module vs disk".
- **FS providers** behind the VFS contract: keep **ext4** (read path), add
  **tmpfs** (RAM-backed, for `/tmp`, `/sys/log`, runtime state), and add
  **xfs** as the on-disk fs for real persistence.  VFS already has
  `fs_ops`+mount table (B1) — these slot in as providers.
- **Memory drivers**: extend the PMM/region layer so a RAM-backed disk and
  tmpfs share one accounting path (reserve the module region, expose it as a
  zone the ramdisk/tmpfs allocate from).

## 3. Phased plan

**R1 — block contract + ramdisk module boot (fixes the panic).**

Chain analysis (verified in source, no assumptions):
- `tools/mkdisk.c`: GPT, 3 partitions — BOOT (LBA 34–2081), KERNEL
  (2082–34849, **redundant** on the module path), DATA/ext4 rootfs at
  **partition index 2** (34850+).  512-byte sectors.
- **No block abstraction exists**: `kernel/fs/gpt.c`, `kernel/fs/ext4.c`,
  `kernel/mm/buffer.c` call `virtio_blk_read/write(buf, sector, count)`
  **directly** — 20 call sites, `int` return (0=ok).  This is the missing
  contract.
- `kernel/arch/amd64/platform/platform.c` walks MB2 tags but handles **only
  MMAP** (type 6); the **MODULE tag (type 3) is ignored** (`nr_modules`
  counted, never used).  `multiboot2.h` doesn't even define MODULE.
- MB2 module tag = `{ u32 type=3, u32 size, u32 mod_start, u32 mod_end,
  char string[] }`.

Steps:
1. **Block contract** — new `kernel/drivers/block/block.{c,h}`:
   `block_read/block_write(buf, sector, count)` dispatch to an active
   `struct block_backend { int (*read)(...); int (*write)(...); }`.  virtio-blk
   registers as the default backend.  Replace the 20 direct
   `virtio_blk_read/write` calls in gpt/ext4/buffer with `block_read/write`.
   **Checkpoint:** `make run` (virtio-blk) must be byte-identical behaviour on
   both arches before adding the ramdisk.
2. **MODULE parse** — `multiboot2.h`: add `MB2_TAG_TYPE_MODULE 3` +
   `struct mb2_tag_module`.  `platform.c`: capture `(mod_start, mod_end)` and
   reserve the region (MEM_REGION_RESERVED) so the PMM never hands it out.
3. **ramdisk backend** — `kernel/drivers/block/ramdisk.c`: read/write over
   `phys_to_virt(mod_start) + sector*512`.  `main.c`: if a module is present,
   activate the ramdisk backend (else virtio-blk) BEFORE partition/VFS init;
   GPT→ext4→VFS mount it unchanged.
4. **mkdisk/Makefile**: a **rootfs-only** `disk.img` for the release (drop the
   boot+kernel partitions; keep GPT+ext4 so the block path is identical).
5. **Release**: stop `dd`-zeroing the MBR; `test-release` boots via `-cdrom`.
   Acceptance: the ISO boots to the shell headless.

**R2 — tmpfs.** RAM-backed fs provider behind VFS; mount `/tmp` and
`/sys/log` (the per-process `log <pid>` tee from queue item 2 writes here).
No disk needed; uses the extended memory path.

**R3 — xfs (disk persistence).** Add an xfs read/write provider behind VFS for
real on-disk storage; ext4 stays for the legacy/rootfs image.  (Large — its
own sub-plan; allocation groups, B+trees, log.)

**R4 — memory drivers.** Unify RAM-disk + tmpfs + PMM zones; growable
reservations; document the memory map per arch.

## 4. Constraints & notes
- aarch64 toolchain pinned (GCC 7.2.0); amd64 uses `x86_64-elf-`.
  `i686-elf-grub-mkrescue` is at `/usr/local/bin`.
- Headless release test: `-cdrom <iso>` (NOT `-drive ... virtio-blk` after the
  MBR is zeroed).  Kernel serial appears once GRUB hands off (multiboot2).
- Keep `make run` (dev) on virtio-blk + `disk.img` so the daily loop is
  unchanged; only the **release** path moves to the module/ramdisk.
- ASTRA: no FS/format code in `arch/`; the module parse is the only arch glue,
  everything else is a provider behind the block/VFS contract.

## 5. Status

**R1 — DONE (2026-06-13).** The amd64 release ISO boots fully self-contained.
- `fd720be` block contract (ASTRA seam `virtio-blk → block → fs → VFS`);
  virtio-blk is a provider, the FS stack consumes `block_read/block_write`.
- `be85d52` mkdisk → **userland-only** single ext4 rootfs partition (boot/kernel
  partitions dropped; they were never read back).
- `d2ac0fc` RAM-backed **ramdisk** over a GRUB multiboot2 MODULE, behind the HAL
  contract `arch_platform_get_boot_module()` (amd64 walks the MB2 MODULE tag in
  `hal.c`; aarch64 returns none — no `#ifdef` in the driver, frozen platform.c
  untouched).  Two boot-path bugs fixed along the way: pmm metadata was placed
  on top of the module (now skips RESERVED regions) and `vmm_init` mapped only
  a fixed 128 MB bootstrap window (now maps up to `pmm_metadata_top()`, so a
  module that pushes the metadata high is still reachable after the CR3 switch).
  Result: `GRUB → kernel + disk.img module → ramdisk → GPT → ext4 mounted →
  shell`, 0 panics; `make run` still mounts via virtio-blk on both arches.

**Open / next (the rest of the user's release-phase list):**
- Free the module RAM after boot (the rootfs is read on demand from the module;
  reclaim once a tmpfs/page-cache copy exists — ties into R2).
- **Unify aarch64**: a DTB `/chosen` initrd → the same ramdisk backend; GRUB
  ISO for aarch64 (`grub` via brew) so both arches ship one image.
- R2 tmpfs · R3 xfs · R4 memory drivers (unchanged from §3).
- UTM: the release boots with minimal flags (only gpu-pci) to the shell; the
  apparent "stuck" was PS/2 input (UTM is PS/2-only) — fixed, see below.

**Out of band (not storage, landed same day):** PS/2 keyboard+mouse driver
(amd64) `09ef8d5` + `807a68b` — was non-functional: handlers registered the
bare ISA IRQ line instead of the PIC vector (32+n), and mouse AUX commands
lacked the `0xD4` prefix.  Both fixed and verified.

- Paused B3-polish queue: see `docs/B3-POLISH-QUEUE.md` (resume after R2–R4).
