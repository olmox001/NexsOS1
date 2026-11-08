# nxec — Verified Project Status

**Status date:** 2026-11-05  
**Document type:** evidence snapshot; this document does not override normative specifications.

## 1. Repository and source integrity

- Project root: /Users/shsh/Documents/github/nxec.
- GRUB source is a Git submodule at boot/grub/upstream.
- The nxec GitHub mirror is https://github.com/olmox001/grub-nxec.git.
- Original GRUB upstream is retained as a separate remote: https://gitlab.freedesktop.org/gnu-grub/grub.git.
- Pinned release: GRUB 2.14, commit d38d6a1a9b79427848976f53d474392cd29c2a71.
- Downstream GRUB modifications belong only in patch/grub/; the build is designed to apply them in a detached worktree.
- The root project has not been committed by the user yet. No commit is claimed by this work.

## 2. Implemented and tested

### Preboot

The preboot project contains HCD wire definitions, CRC-64, structural and semantic parsing, section catalog, caller-owned builder, ISA capability snapshot interfaces, a bounded physical extent planner, a host-testable UEFI memory-map/ExitBootServices retry helper, an in-place post-EBS UEFI memory-map normalizer, a caller-buffer Multiboot2 BIOS/E820 memory-map converter and a Multiboot2 EFI post-EBS map-tag extractor.
Verified in the current work session with `PATH=/usr/local/opt/llvm/bin:$PATH make -C preboot test`: host unit tests, AMD64/ARM64 freestanding cross-compilation and relocatable links, ASan/UBSan, clang-tidy and cppcheck passed, including both Multiboot2 memory-map adapters. No concrete firmware entry is wired. GRUB 2.14 source inspection resolved the EBS owner per entry path (DEC-UEFI-ENTRY-OWNERSHIP-001); firmware/QEMU validation remains outstanding.

### Kernel

The kernel now owns an independent HCD implementation under kernel/core/hcd/. Its headers and sources are separate copies maintained in the kernel project; kernel build files do not include or compile preboot/common.

Verified in the current work session:
- kernel host parser and admission-gate tests: PASS;
- kernel freestanding cross-compilation and relocatable link for x86_64-elf: PASS;
- kernel freestanding cross-compilation and relocatable link for aarch64-none-elf: PASS;
- no unresolved symbols in either relocatable kernel validation object: PASS;
- kernel ASan/UBSan test run: PASS.

The kernel HCD test fixture is kernel-owned and uses the kernel protocol definitions.

## 3. Implemented build infrastructure

- Independent Makefiles exist for preboot and kernel.
- Root Makefile exposes setup, repository synchronization, test profiles, GRUB build, QEMU run entry points and clean.
- `make grub` completed successfully for all three configured targets: i386-PC, x86_64-EFI and AArch64-EFI. The build used the clean pinned GRUB 2.14 submodule at `d38d6a1a` and applied only the ordered patches in `patch/grub/series` to a detached worktree.
- `make toolchain` passed after the build and generated `tools/env/toolchain-manifest.txt`.
- QEMU launcher entry points exist for AMD64 and ARM64. A bootable nxec image is not yet available, so a successful QEMU boot is not claimed.
- Repository synchronization initializes the GRUB submodule and configures the nxec mirror as origin and GNU GRUB as upstream.

## 4. Host toolchain status

The host is macOS Intel x86_64. The current `make toolchain` check passed, including host and cross tools, and generated `tools/env/toolchain-manifest.txt`. All three GRUB target installs contain `grub-mkimage`, `grub-mkrescue`, `grub-file` and `grub-script-check`.

No bootable nxec image or QEMU boot is implied by these toolchain/build checks.

## 5. Not implemented or not verified

- Firmware entry adapters and actual AMD64/ARM64 firmware boot.
- Wiring the Multiboot2 memory-map converter into the AMD64 BIOS entry and implementing EFI entry adapters using the resolved ownership contracts; runtime validation remains outstanding.
- Final physical/virtual memory bootstrap and kernel page-table handoff.
- NXB compressed kernel image format, authenticated manifest and decompressor.
- Kernel ELF loading, relocation validation and kernel entry.
- REG runtime, endpoint/ring implementation and formal concurrency proofs.
- Registry, capability engine, revocation, scheduler and memory manager.
- Sandboxed ELF system modules under kernel/modules/.
- Separate userland image, VFS, libc, dynamic linker and compatibility layers.
- QEMU boot tests and hardware qualification.
- Final project-wide license decision.

## 6. Integrity rules

The preboot and kernel are separate projects and trust domains. They share a documented binary protocol, not C structures or source code. Kernel HCD validation is independently implemented. Drivers and system services stored in kernel/modules/ run in user-mode sandboxes; directory placement does not grant MACHINE privilege. The separate userland/ tree is the userland image project.
