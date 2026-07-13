# Userland foundation programme — musl, busybox, VFS close-out

Programme record opened 2026-07-12 on maintainer direction. Same discipline
as `docs/graphics-port/`: controlled increments, append-only documentation
(dated files beside this plan), `make run` on BOTH architectures before
every commit, and for every ported library its OWN integrated test suite
must run.

## Ordering (maintainer: finish current phases first)

0. **Phase A — close the running increments.** The surface-speaking GPU
   contract is merged (`408e74f`); pending tree changes validated
   (maintainer-approved `make run`) and committed.
0b. **Phase B — graphics-chain close-out queue** (continues in
   `docs/graphics-port/`, interleaved with the phases below as increments
   complete). State: SDL2 software profile DONE (window, present, timers,
   full input, resize); D3D9 presentation chain DONE (demo3d client),
   personality pending; OpenGL front OPEN. Queue, in order:
   1. OSMesa+softpipe cross-build: meson cross-file over the OS1 libc +
      single-thread c11-threads shim in `portability/opengl/`
      (Mesa pinned at 25.0.7 — last release with OSMesa; tree unpatched);
   2. `glcube` — first real GL client through the `os1_gl_platform` seam;
   3. Craft (MIT Minecraft clone) port: fork + `nexsos-port` branch,
      GLFW→SDL2-shim, GLEW dropped, sqlite3 vendored, curl disabled;
   4. D3D9 personality evaluation: Gallium Nine (MIT, in the pinned Mesa
      tree) vs Wine d3d9 (LGPL, needs the dynamic-linking design first).
1. **Register musl and busybox submodules** — the established model:
   `olmox001` fork of the upstream mirror, `nexsos-port` branch, pinned
   gitlink; `user/sys/lib/musl`, `user/bin/busybox`.
2. **Port them like every other library**: implementation SEPARATED from
   API and BASED on the API — OS glue lives in `user/sys/lib/portability/
   {musl,busybox}/`, upstream trees stay unpatched in this repo (in-tree
   integration commits, when unavoidable, go on the fork branch,
   human-authored where upstream policy demands it). musl's syscall layer
   maps onto the OS1 syscall ABI exactly like SDL's config injection: one
   NexsOS `syscall_arch.h`-shaped seam, everything above it untouched.
3. **VFS close-out** (review of the Lua-port VFS work included, see
   below): mount/write/remove and the missing calls, then the internal
   POSIX-like API consolidated over registry/object — per ASTRA the file
   model converges on 9P-style namespace operations (walk/open/read/write/
   clunk over mounted providers), with POSIX as a userland veneer, not the
   kernel contract.
4. **/tmp and ISO/release boot**: today the release path copies the whole
   image into RAM and never frees it. After mount/unmount are stable, the
   root moves to a mounted provider and the boot copy is dropped/released;
   /tmp becomes a ram-backed mount instead of an implicit assumption.
5. **Dynamic linking design (ASTRA)** — after the above, before any D3D9
   personality code (licence pressure: Wine is LGPL; Gallium Nine, MIT,
   remains the lighter candidate).

## VFS review findings (2026-07-12 recon, to drive phase 3)

Present in `kernel/fs/` (vfs.c 499 + ext4.c 1614 + procfs + gpt):
`vfs_register_fs`, `vfs_mount_at` (Plan 9-style namespace mount),
`vfs_resolve_object`, `vfs_open/read/read_file`,
`vfs_write_allowed/write_file`, `vfs_list_dir`, `vfs_unlink`, `vfs_create`,
`vfs_stat`. The Lua port (`861e6a6`) added ~600 lines to ext4 (write-path
work) plus new fs syscalls — that delta is the review target: its write
paths must pass the same scrutiny as the graphics S-STAB pass (geometry vs
allocation, lock ordering, error propagation).

Known gaps to close, in increment order:
1. **`vfs_umount`** — does not exist; mount today is one-way. Required
   before the ISO-boot rework (phase 4) can release the RAM image.
2. **`vfs_rename`, `vfs_truncate`, `vfs_mkdir`** (mkdir today only via
   `vfs_create(type)` where providers support it) — the "other calls".
3. **VFS-04**: 255-char path buffers with SILENT truncation in
   normalisation (vfs.c) — a correctness hazard (two distinct deep paths
   can alias); must return an error instead.
4. Write-path coverage in ext4 for the calls above + kernel unit tests for
   mount/umount/unlink/rename lifecycles (ktest, both architectures).

## Integrated tests per library (maintainer requirement)

- **Lua (nxlua)**: the upstream `testes/` suite is in the submodule; the
  port must run it ON NexsOS (lua.elf executing testes) and record pass/
  fail per file in the increment doc.
- **musl**: upstream has no in-tree suite; the reference is libc-test
  (musl's companion) — a NexsOS-runnable subset gets vendored under the
  portability dir and wired as an in-OS test binary.
- **busybox**: its `testsuite/` runs applet-by-applet; a NexsOS profile
  starts with the shell-independent applets and grows with the VFS calls
  (rm/mv/mkdir map 1:1 onto phase-3 work).
- **SDL2** (already ported): `sdltest` is the integrated harness; SDL's
  own test programs (test/) become buildable targets as libc coverage
  grows via musl.

## Non-negotiables carried over

- No ambient-authority syscalls; new kernel surface only through the
  object/handle model (ASTRA 6.1/6.2).
- Upstream submodule trees are never patched in this repo's checkout.
- A feature is not "done" until built, booted and exercised on BOTH
  AArch64 VirtIO-MMIO and amd64 VirtIO-PCI, with its tests green.
