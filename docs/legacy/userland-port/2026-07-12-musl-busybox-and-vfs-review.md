# 2026-07-12 — musl/busybox registered; first-pass review of the Lua-port VFS delta

Increment record for `PLAN.md` phases 1 and 3-recon (documentation is
append-only).

## Phase 1 delivered: musl + busybox submodules

- `olmox001/musl` (fork of the actively synced `kraj/musl` mirror), branch
  `nexsos-port` pinned at **v1.2.6** → `user/sys/lib/musl`.
- `olmox001/busybox` (fork of the official `mirror/busybox`), branch
  `nexsos-port` pinned at **1.36.1** → `user/bin/busybox`.
- Both branches were created server-side at the release tags; trees
  unpatched. Porting follows the portability model: the musl OS seam will
  be a `syscall_arch.h`-shaped mapping onto the OS1 syscall ABI under
  `user/sys/lib/portability/musl/`; busybox config+glue likewise.
- Validated per protocol before commit: `make run` on both architectures,
  7/7 kernel tests, no panic (submodules are not yet in the build).
- Cosmetic note for the maintainer: the nxlua submodule branch is named
  `nexsos-por` (missing "t") on the fork and in `.gitmodules` —
  consistent, works, rename optional.

## First-pass review: VFS/ext4 delta from the Lua port (`861e6a6`)

Scope reviewed: `vfs_create` seam, ext4 write-path additions
(inode/block alloc+free, extent-tree free, dirent insert/remove,
create/unlink), S-STAB criteria (locking, bounds, error paths).

Positive findings:
- `vfs_create` has a clean, documented contract (authority and
  pre-existence checks explicitly assigned to the syscall layer, #126).
- ext4 mutations run under a per-fs spinlock (`fs->lock`), taken in the
  read (120/244) and write (1059/1131/1169) paths alike.
- Dirent walk loops carry `rec_len` sanity guards; the lookup path bounds
  both sides (`rec_len < 8 || rec_len > block - offset`), and the listing
  path's weaker guard (`rec_len < 8` only) is still safe because the
  `offset < 4096` loop condition is re-checked before any dereference.

Follow-ups queued (with the `vfs_umount` increment):
1. Listing-path guard tightened to match the lookup path (defence in
   depth, zero cost).
2. Alloc/free error-path audit: partial-failure sequences in
   create/unlink (inode allocated, dirent insert fails → inode must be
   freed exactly once; bitmap/superblock counters consistent) — the
   double-free/leak class the first pass could not exhaustively cover.
3. `vfs_umount` (mount is one-way today) — prerequisite for the ISO-boot
   RAM-copy rework (PLAN phase 4).
4. VFS-04: silent 255-char path truncation becomes an error.
5. ktest lifecycle coverage: mount → create → write → read → unlink →
   umount on both architectures.
