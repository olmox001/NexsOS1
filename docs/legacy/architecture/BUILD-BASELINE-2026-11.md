# NEXS Build and Boot Baseline — 2026-11-02

## Source snapshot

- Superproject: `DEV-Resurrection`
- Baseline commit before this work: `b495da3a711e5af64dd826c29c6da4777c487074`
- Host used: macOS Intel, `Darwin x86_64`
- QEMU: 11.1.1
- Working directory: `/Users/shsh/Documents/github/NexsOS1/NexsOS1-Resurrection/NexsOS1`

## Bootstrap

`./tools/setup-project.sh` completed successfully (exit 0) on this host.
The second run also completed successfully after all submodules were initialized,
demonstrating idempotence for the current checkout.

All top-level and recursive submodules report the revisions pinned by their
superproject gitlinks. The Coreutils submodule requires the metadata-only nested
gnulib path correction described in `NEXS-NG-CLEANROOM.md`. Its source files and
gitlink revision are unchanged.

## Locked cross-toolchains

| Target | GCC | Binutils | Result |
|---|---:|---:|---|
| x86_64-elf | 16.1.0 | 2.46.1 | Verified on macOS Intel |
| aarch64-none-elf | 7.2.0 | 2.29.1 | Verified on macOS Intel |

`make check ARCH=amd64` and `make check ARCH=aarch64` both pass, including
target-triple and exact compiler/linker-version checks.

## Full clean builds

A clean `build/` was used for the x86-64 build; AArch64 was built into its
previously empty architecture output directory.

| Command | Exit | Result |
|---|---:|---|
| `make all ARCH=amd64` | 0 | kernel, bootloader, userland, libraries, rootfs, disk image |
| `make all ARCH=aarch64` | 0 | kernel, userland, libraries, rootfs, disk image |

Artifacts:
- `build/amd64/kernel.elf`, `build/amd64/disk.img`
- `build/aarch64/kernel.bin`, `build/aarch64/disk.img`
- Both disk images are 512 MiB.
- Both architectures produced the original userland executables, including
  `nxshell.elf`, `nxui.elf`, `nxlauncher.elf`, `doom.elf`, and the SDL/Lua/Gnulib
  libraries.

No userland source files were changed.

## QEMU boot and kernel tests

Both architectures were booted headlessly for a 15-second smoke-test window.
QEMU was intentionally terminated after the OS reached its interactive shell.

- x86-64: booted, all four CPUs online, init completed, NXShell TTY prompt active.
- AArch64: booted, all four CPUs online, VirtIO block/GPU/input initialized,
  Ext4 root mounted, init completed, NXShell TTY prompt active.
- Kernel unit tests: **8 passed, 0 failed** on each architecture.

Serial captures:
- `build/logs/qemu-amd64-serial.log`
- `build/logs/qemu-aarch64-serial.log`

Build captures:
- `build/logs/build-amd64.log`
- `build/logs/build-aarch64.log`
- `build/logs/setup-project-macos.log`

These are local build outputs and are not source-controlled.

## Legacy kernel backup

The original kernel was archived without modifying or deleting its source:

- Archive: `backups/legacy-kernel-b495da3a711e5af6.tar.gz`
- Source revision: `b495da3a711e5af64dd826c29c6da4777c487074`
- Files: 183 tracked files; no kernel gitlinks or local kernel changes.
- SHA-256: `069c18940ef98cec74424956060ca8c3872dcc31405584b582a0a47569c0daab`
- Manifest: `backups/legacy-kernel-b495da3a711e5af6.tar.gz.manifest.txt`
- Restore test: archive listing and extracted-tree `diff -qr` both passed.
- Reusable command: `tools/backup-legacy-kernel.sh`

This is a verified local archive inside the project directory. Before deleting or
replacing the original kernel, make a second independent copy or preserve the
archive in a separately backed-up repository/object store.

## Findings retained for follow-up

1. `make lockcheck` currently fails with 2 RULE-A and 5 RULE-B findings
   (allocator reachability under scheduler lock; page-free/current-process
   lifetime checks). The Makefile already documents this analysis as
   intentionally not wired into `all`; it needs manual triage, not suppression.
2. The separate `bootloader.elf` target emits an RWX LOAD-segment warning.
   It is not the loader used by current QEMU run or release targets. Its
   Multiboot2 entry-tag header/parser are inconsistent; see
   `BOOT-PROTOCOL-INVENTORY.md` before reusing it.
3. SDL2 emits an incompatible-pointer warning in
   `SDL_ScanUnsignedLongLong`.
4. Gnulib/Coreutils emit existing format-attribute, deprecated-string and
   maybe-uninitialized/return-path warnings. Several user applications emit
   unused-variable and strict-overflow warnings. These were not hidden by
   source edits; userland source was preserved.
5. Linux-host toolchain build and Apple Silicon host bootstrap have been updated
   but are **not yet executed on those hosts**. Apple Silicon's current AArch64
   toolchain package path explicitly requires Rosetta 2 and Intel Homebrew.
6. A true empty fresh clone has not yet been provisioned end-to-end; the
   idempotent setup path was tested on this initialized checkout. Fresh-clone
   CI validation remains a gate.

## Next safe steps

1. Exercise the pinned GNU archive SHA-512 checks on a clean Linux host.
2. Validate Linux setup/build in a clean Linux host or CI runner.
3. Validate Apple Silicon setup under the documented Rosetta/Intel Homebrew path.
4. Add a fresh-clone bootstrap CI job and dual-architecture build/boot jobs.
5. Triage the RWX bootloader segment, lockcheck findings and external-library
   warnings without changing userland semantics.
6. Make an independent second copy of the verified archive, then begin the
   parallel clean-room kernel tree. Keep the original kernel intact until the
   new core passes the dual-architecture compatibility gates.

**The original kernel is backed up locally, but has not been deleted or replaced.**
