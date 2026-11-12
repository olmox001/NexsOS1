# nxec — Independent Projects and Build Orchestration

**ID:** BUILD-GRAPH-001  
**Version:** 0.2.0-DESIGN  
**Status:** DESIGN; preboot and the kernel HCD admission path are the first implemented components.

## 1. Rule

Every component is an independent project with its own README, build file, toolchain/target, dependency and license manifest, versioned ABI, tests and output artifacts. Root orchestration coordinates and verifies artifacts; it does not duplicate subsystem implementations.

Directories are created only when real source, tests, manifests or documentation require them.

## 2. Projects

| Project | Responsibility | Build | Status |
|---|---|---|---|
| preboot/ | ISA HAL, firmware discovery, HCD production, physical-memory bootstrap and kernel-image loading | Independent Makefile | HCD/planner tested; UEFI retry/post-EBS normalization, Multiboot2 EFI tag extraction and BIOS map conversion host-tested; concrete firmware entry adapters pending |
| kernel/ | MACHINE HAL/Core, hypervisor control, registry, capability engine, scheduler, memory manager, REG and ELF loader | Independent Makefile | Independent HCD gate tested |
| kernel/modules/ | Independent ELF system modules packaged with the kernel image; drivers/services run in user-mode sandboxes | Per-module build + module catalog | Design |
| userland/ | Separate userland image, libc, runtimes, VFS, filesystem services, compatibility and applications | Per-package/service build | Design |
| boot/grub/ | Pristine pinned GNU GRUB source, target builds and configuration | Upstream build wrapper + patch series | Pinned source and complete i386-PC, x86_64-EFI, AArch64-EFI build/install passed |
| tools/ | Toolchain, repository sync, validators, QEMU and orchestration | Versioned scripts | Partial |

A subproject must not depend on another project's build output without an explicit, versioned artifact ABI.

## 3. Build graph

1. Toolchain manifest and dependency verification.
2. Preboot common/HCD.
3. Preboot ISA adapters.
4. Kernel-owned HCD validator and kernel ABI.
5. Kernel image, decompressor and ELF loader.
6. Kernel-packaged system modules under kernel/modules/.
7. Userland image builder.
8. Userland libc, services and compatibility layers.
9. Image packaging, integrity and signing.
10. GRUB/UEFI integration.
11. AMD64/ARM64 QEMU and hardware test matrix.

Every edge declares artifact inputs, outputs, version and hash. Root orchestration fails on stale or incompatible artifacts. Unimplemented targets are marked DESIGN_ONLY; no placeholder build is allowed to imply implementation.

## 4. Progressive directories

Do not pre-create empty directory trees. A project or module directory is created together with its first real source, build file, manifest, test or required document.

## 5. GRUB isolation and licensing

GRUB is an independent upstream project, not nxec kernel source. The official GNU GRUB repository is mirrored into the nxec GitHub account for controlled transport and availability. The submodule is pinned to an exact upstream release commit. The original GNU repository remains a separate upstream remote.

All nxec-specific GRUB changes are maintained as patch files in patch/grub/. The build creates a detached worktree from the pristine pinned submodule, applies the ordered patch series and builds there. The submodule branch itself is never modified by nxec patches.

GRUB is GPL-3.0-or-later. The nxec project license does not relicense GRUB. Preserve copyright, license, revision, source correspondence and distribution obligations. Review combined image distribution obligations before release.

## 6. Reproducibility

Each artifact records source revision, content hash, toolchain manifest, target triple, compiler/linker flags, configuration and test report. Normalize timestamps and build paths where possible. The first-run setup initializes submodules before building.

## 7. Gate

Before distributing a bundle: audit licenses, dependencies, corresponding source, GRUB revision, signatures, reproducible builds and tests for all declared targets.
