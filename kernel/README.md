# NX-CORE

NX-CORE is the standalone NexsOS1 kernel repository. It owns the kernel,
architecture HALs, kernel drivers, kernel tests, kernel linker scripts,
bootloader inputs, symbol generation, and all kernel build rules.

It does not require NX-VIOLA, NX-SDK, or the NexsOS1 integration superproject
to compile and boot the kernel.

## Prerequisites

- macOS or Linux host.
- Pinned target cross-toolchain from config/toolchain-versions.env.
- QEMU for boot tests.
- GRUB BIOS + xorriso for amd64 boot media.
- For aarch64 UEFI media: curl, make, gawk, mtools, xorriso, the pinned AArch64
  cross-toolchain, and an EDK2/AAVMF code+vars firmware pair for QEMU execution.

Install/check the module toolchain:

    make setup-deps
    make check ARCH=amd64
    make check ARCH=aarch64

## Build independently

    make clean all ARCH=amd64
    make clean all ARCH=aarch64

Outputs are under build/amd64 and build/aarch64. The standalone build compiles
only NX-CORE and its bootloader inputs; it does not build userland, SDK
libraries, rootfs, disk images, or release packages.

## Boot independently through GRUB

    make boot-iso ARCH=amd64
    make run ARCH=amd64

    make boot-iso ARCH=aarch64
    make run ARCH=aarch64 UEFI_CODE=/path/to/edk2-aarch64-code.fd UEFI_VARS_TEMPLATE=/path/to/edk2-arm-vars.fd

The amd64 route uses GRUB BIOS. The AArch64 route builds GRUB arm64-efi and
the project EFI handoff image. Both run without a rootfs or userland image;
K3 reports the expected kernel-alone mode when /sbin/nxinit is absent.

For low-level QEMU diagnostics without GRUB, use run-direct.

## Build ownership

- build.mk is the single canonical kernel source manifest and rule set.
- Makefile is the standalone entrypoint and host/target configuration.
- The superproject invokes this Makefile; it does not include or duplicate
  kernel source manifests, flags, or compile/link rules.
- contracts/ contains the local legacy contract snapshot and checksum.
  Future public SDK declarations must be generated from versioned normative
  specs and verified with compatibility tests; do not revive a shared root
  include/abi directory.
- tools/ contains kernel-owned build helpers and host bootstrap scripts.

## Cleaning

    make clean ARCH=amd64
    make clean ARCH=aarch64

Only the selected architecture's module build output is removed.
