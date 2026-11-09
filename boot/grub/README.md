# NexsOS1 GRUB source, build and prebuilt contract

`boot/grub/upstream` is the only GRUB source checkout owned by this project.
It is a git submodule fetched from `https://github.com/olmox001/grub.git`
(branch `nexsos-port`), pinned by the top-level gitlink and locked again by
`boot/grub/manifest.env`.  The official GNU GRUB GitLab repository remains the
`upstream` remote inside that checkout; NexsOS1 consumes the GitHub repository
exactly like the other project-owned external submodules.

Current source lock:

- GNU GRUB 2.14
- commit `d38d6a1a9b79427848976f53d474392cd29c2a71`
- gnulib `9f48fb992a3d7e96610c4ce8be969cff2d61a01b`
- targets: `i386-pc`, `x86_64-efi`, `arm64-efi`

The upstream submodule is never patched in place. Project compatibility changes
live under `kernel/tools/patches/grub-2.14/`; the sorted patchset hash is part
of the generated build fingerprint.

## Ownership

- `boot/grub/upstream`: immutable pinned source submodule.
- `boot/grub/manifest.env`: source/pipeline ABI lock.
- `kernel/tools/build-grub-target.sh`: one source builder for all supported
  GRUB targets.
- `build/grub/<target>/`: disposable generated build tree.
- `build/cache/grub/prebuilt/<target>/`: verified host-specific installed
  stage. This directory survives `make clean`.
- `release/grub/<version>/<host>/`: distributable prebuilt archives and
  SHA-256 files.

Boot-image builders consume only the verified prebuilt cache. They do not
consume generated object files from `build/grub` and do not depend on a
globally installed GRUB.

## Make targets

```text
make grub-i386-pc-tools
make grub-x86_64-efi-tools
make grub-arm64-efi-tools
make grub-tools

make grub-prebuilt
make grub-prebuilt-target GRUB_TARGET=x86_64-efi
make grub-prebuilt-install GRUB_ARCHIVE=/path/to/archive.tar.gz

make ARCH=amd64 boot-iso
make ARCH=amd64 boot-uefi
make ARCH=aarch64 boot-uefi
```

The target builder first validates the cached prebuilt fingerprint. When it
matches the locked GRUB commit, gnulib revision, compatibility patchset,
pipeline revision, host and target, the builder exits without configuring or
compiling GRUB. Set `NEXS_GRUB_FORCE_REBUILD=1` only when intentionally
requalifying the source build.

A prebuilt archive is accepted only when its version, git commit, pipeline
revision, host tag and tree hash match the local lock. Installing an archive
places it under `build/cache/grub/prebuilt/<target>`; source checkout and
generated build trees are not modified.

## Runtime qualification

The GRUB build itself is not an OS acceptance result. Release qualification
still requires the corresponding NexsOS1 boot lane to reach an interactive
NXShell and pass the normal `nxadb` timer/heartbeat/KTEST/fault gates.

- amd64 BIOS: GRUB `i386-pc`
- amd64 UEFI: GRUB `x86_64-efi` + OVMF/EDK2
- aarch64 UEFI: GRUB `arm64-efi` + AAVMF/EDK2

AMD64 must not gain a DTB requirement. Its platform discovery remains the
ACPI/PCI path when no FDT is supplied.
