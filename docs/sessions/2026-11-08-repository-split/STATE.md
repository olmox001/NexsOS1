# Initial State — 2026-11-08

## Checkout and Git

- Root: `/Users/shsh/Documents/github/NexsOS1/NexsOS1-Resurrection/NexsOS1`
- Host: macOS 26.5.2, Intel x86_64.
- Branch: `DEV-Resurrection`.
- HEAD: `9f09f2e`, aligned with `origin/DEV-Resurrection` at inspection.
- Remote: `origin https://github.com/olmox001/NexsOS1.git`.
- Existing local modification: `user/bin/coreutils` gitlink.
- Existing staged deletion: `user/sys/lib/lib-2.c`, 4,645 lines.
- Existing untracked documentation: `docs/sessions/`.
- Recursive submodule inspection showed Coreutils' nested Gnulib at `user/bin/coreutils/gnulib`; preserve its exact state.
- No source, Makefile, .gitmodules, .gitignore, or build artifact was modified by the inspection.

## Current project topology

- `kernel/`: existing kernel code, including `kernel/arch/amd64` and `kernel/arch/aarch64`.
- `user/`: existing userland: `arch`, `bin`, `sbin`, `sys/bin`, `sys/lib`, and `home`.
- `include/abi` and `include/api`: public/shared header candidates requiring per-header ownership and dependency classification.
- `user/sys/lib`: contains NexsOS-owned libc/runtime/system-library sources and portability overlays mixed with upstream submodules.
- External library submodules currently include SDL, Mesa, Wine/Direct3D, Lua, musl, and Gnulib under `user/sys/lib`. External source must remain independent; the intended external dependency location is `user/lib`, but its Git ownership and nesting must be implemented without attributing upstream source to NX-VIOLA.
- Userland program submodules include base-nexs, BusyBox, Coreutils, Doom, and kilo.
- `tools/`: host-side setup, toolchain, image, QEMU, nxrun, nxadb, and mkdisk tools.

## Build and test observations

- Main Makefile: 1,231 lines; kernel, userland, upstream libraries, rootfs, mkdisk, disk, GRUB, QEMU, test, and release logic remain coupled.
- `Makefile.linux` and `Makefile.macos` currently provide host-specific defaults and include the main Makefile; they are not independent kernel/userland build systems.
- `make -n kernel ARCH=amd64`, `make -n user ARCH=amd64`, `make -n kernel ARCH=aarch64`, and `make -n user ARCH=aarch64` all exited successfully on the current macOS Intel host.
- This dry-run proves rule resolution only; it does not prove compilation, boot, shell readiness, or nxadb interaction.
- Existing output directories `build/amd64` and `build/aarch64` contain binaries, disk images, GRUB output, logs, and test artifacts. Do not use their presence as proof that the current source revision produced them.
- The latest documented full build/boot baseline is `docs/architecture/BUILD-BASELINE-2026-11.md`, dated 2026-11-02 and based on an earlier commit. It records macOS Intel builds and shell boot on both targets. It does not certify Linux or native Apple Silicon.
- The current host is Intel macOS. Linux x86_64, Linux arm64, and macOS arm64 need independent verification.

## Concrete build boundary defects

1. The main Makefile's global `INCLUDE` adds both `include/abi` and `include/api` to kernel and userland compile flags, despite comments claiming the API is userland-only.
2. `include/abi/posix_types.h` mixes wire/layout types with kernel helpers such as atomics, page size, alignment, container macros, and memory barriers. Ownership must be decided by actual consumers and binary layout, not filename.
3. `user/arch/amd64/syscall.S` and `user/arch/aarch64/syscall.S` are built as userland startup/runtime objects. They include `_start`, syscall wrappers, `setjmp`, and `longjmp`; every symbol and consumer must be mapped before ownership changes.
4. The generic `.S` rule uses CFLAGS. `ASFLAGS` and `CFLAGS_BOOT` are declared but not consumed by dedicated rules in the inspected Makefile.
5. The current HAL build selects `kernel/arch/<arch>/hal.c`. The existing HAL must be reused, not duplicated under another directory.
6. Userland's current linked library objects are `lib.c`, `math.c`, `execsvc_client.c`, portability video/D3D9/OpenGL glue, and `malloc.c`. The staged `lib-2.c` deletion is not part of the active build and must remain untouched.
7. `tools/mkdisk.c` is compiled with hardcoded `gcc`, rather than the declared host compiler variable.
8. Output paths use `build/<target-arch>`; they do not distinguish host OS/CPU, toolchain identity, profile, or module.
9. Rootfs copies API/ABI headers and selected libraries using hardcoded source paths rather than a module install manifest.
10. Release currently packages GRUB ISO and disk image, plus architecture-specific kernel/DTB/boot manifest files. A general manifest exists, but the inspected recipe does not generate checksums or complete build provenance.

## Boot and runtime baseline

- AMD64 release/development path: GRUB BIOS ISO.
- AArch64 release/development path: GRUB arm64-efi ISO, project EFI loader, kernel image, and DTB.
- Direct boot remains a diagnostic path and must not replace GRUB acceptance.
- Do not claim AMD64 UEFI support from the current BIOS ISO path.
- nxadb interacts with the active NXShell through QMP and preserves serial/stdout/stderr channels. One-shot command success is inferred from output/prompt, not from a structured guest exit status. Acceptance must check exact expected output and retain complete logs.

## NASA/JPL Power of Ten

The session uses the original ten rules, not an invented substitute checklist. Known legacy conflicts include:
- Rule 1: user syscall assembly exports setjmp/longjmp; Lua depends on this compatibility surface.
- Rule 3: kernel and userland allocators exist; runtime allocation lifetime and boundedness have not been fully audited.
- Rule 8: shared headers contain complex macros and mixed ownership.
- Rule 10: warning demotions exist for userland/upstream code; zero-warning evidence is not established across all modules.
- Rules 2, 4, 5, 6, 7, and 9 require full source-level audit.

No claim of full compliance is made. Existing deviations must be identified and resolved or explicitly approved before a compliance claim.

## Scope of this inspection

Read-only inspection and documentation correction only. No source migration, Git history rewrite, submodule relocation, build-system rewrite, or test boot was performed in this inspection pass.


## Maintainer direction update — 2026-11-08

- NX-SDK is part of the NX-VIOLA platform conceptually and is independently versioned. The superproject must pin it as a peer checkout so NX-CORE can consume only its kernel-safe contract subset without a repository dependency on NX-VIOLA.
- First deliverable is separate kernel/userland build entrypoints with a stable shared configuration contract; rootfs, GRUB, mkdisk, release and nxadb remain superproject integration responsibilities.
- The versioned HAL, kernel ABI and SDK compatibility model is specified in `CONTRACT-VERSIONING.md`; implementation and test gates are not yet present.
- Current `user/sys/lib/lib.c` is 4,985 lines. It contains POSIX/C runtime, OS1 low-level and high-level API veneers, service clients, graphics/video and compatibility shims. Existing docs explicitly separate library stratification (Phase 10a) from moving services (Phase 12); keep these as independently verifiable changes.
- `notify_send`/ `OS1_notify_*` resolves the boot-started notification service. `execsvc_client.c` is already a separate client module. The old R0 census is stale and must be regenerated; no arbitrary line-based split.
- Portability overlays currently exist for Coreutils, Gnulib, Lua, SDL2, D3D9 and OpenGL, plus shared video platform glue. These are NexsOS-owned adaptation code and need ownership/symbol/dependency maps before migration.
- Forced amd64 build completed with exit code 0 at 2026-11-08; this is build evidence only. Forced aarch64 build is still active at last poll, and userland/upstream warnings were observed.


## Clean-build result after build-boundary changes — 2026-11-08

- `make clean` completed; it removed both target build trees and generated release artifacts while preserving `build/runs` logs.
- `make all ARCH=amd64`: exit code 0. Generated amd64 kernel ELF, userland ELFs, rootfs and disk image.
- `make all ARCH=aarch64`: exit code 0. Generated AArch64 kernel raw binary, userland ELFs, rootfs and disk image.
- Both root builds traversed `make -C kernel module` and `make -C user module`. The module Makefiles are currently entrypoint facades; the root Makefile still owns the actual recipe definitions.
- Userland compilation succeeded after removing kernel-private include roots from its flags. Kernel compilation still includes `include/api` pending classification/migration of the existing kernel include dependencies.
- A first attempt failed because the new boot assembly rule used the legacy `CFLAGS_BOOT` variable (which adds `-m32` on amd64); the boot header requires the prior kernel compiler mode. The rule was corrected to `KERNEL_CFLAGS` and the subsequent clean builds passed.
- Known warnings remain; inspect the complete logs before changing or suppressing them.
- No boot-to-shell/nxadb result for this rebuilt pair yet.


## GRUB/nxadb acceptance update — 2026-11-08

- Fresh amd64 GRUB BIOS boot reached NXShell. nxadb reported `shell=yes`, `faults=0`. A shell command printed `NX_SPLIT_ACCEPTANCE_AMD64`; the exact marker is in `build/runtime/nxadb/amd64/serial.log`.
- Fresh aarch64 GRUB arm64-efi boot reached NXShell. nxadb reported `shell=yes`, `faults=0`. A shell command printed `NX_SPLIT_ACCEPTANCE_AARCH64`; the exact marker is in `build/runtime/nxadb/aarch64/serial.log`.
- Both QEMU stderr files are empty (0 bytes); the serial scan found no panic, page fault, protection fault or AArch64 lower-EL data-abort marker. Both guests were stopped.
- Full acceptance transcript: `build/runs/repository-split-nxadb-acceptance-20261108.log`; full per-architecture captures remain under `build/runtime/nxadb/` and `build/runs/<arch>/`.
- The script attempted unsupported `nxadb bootlog --no-follow`; it printed usage after the shell test. This host-side command mistake does not invalidate the independently verified boot/shell marker and status.
- Fresh layering audit: 19 kernel→userland header references, 0 userland→kernel code references, 0 userland depfile paths into kernel include roots. The 19 kernel-side references remain open and need requirement ownership classification.
- Scope is macOS Intel host only; Linux and Apple Silicon host certification remain open.


## Service-extension and portability update — 2026-11-08

- Notification client moved out of lib.c into extensions/services/notify_client.c; OS1_report_error moved with its service-backed reporting policy. execsvc_client.c moved under the same extension boundary. Server binaries remain in NX-VIOLA.
- Notification client smoke test passed on amd64 and aarch64: /sys/bin/nxnotify send ... returned “sent”; /sys/bin/nxnotify list showed the message from PID 9; nxadb status reported shell=yes and faults=0 on both.
- Bare nxnotify is not resolved from /bin; the system binary is at /sys/bin/nxnotify. Use the absolute path in tests until shell PATH resolution is separately verified.
- The aarch64 notification list displays a truncated message suffix; service ring/display length must be specified and tested.
- execsvc protocol has EXECSVC_VERSION=2 and nxinit starts/supervises nxexec. captest constructs the wire protocol directly. No in-tree callsite of execsvc_spawn was found; the public client function needs a direct integration test before FULL support can be claimed.
- Portability ownership findings are in PORTABILITY-CLASSIFICATION.md. SDK candidates: video platform, D3D9/OpenGL presentation adapters, SDL2 driver overlay, generic Gnulib adapter and Lua runtime binding. Coreutils-specific overlay remains NX-VIOLA. External upstream sources remain separate.
- lib.c remains monolithic for standard C/POSIX, OS1 native API and graphics; font_lib.c is still source-included. The next library increments must split these by symbols/call graph, not by line ranges.


## Corrective build-boundary audit — 2026-11-08

- The attempted module Makefiles were facades that delegated back to the root
  Makefile. They did not satisfy independent-build requirements and have been
  removed rather than represented as completed work.
- A second attempted standalone NX-CORE Makefile duplicated source manifests
  and was removed immediately; its generated build directory was removed.
  No duplicate HAL or kernel implementation was retained.
- The root Makefile's kernel/user targets now depend directly on their existing
  implementation targets, so removing the facade files does not leave stale
  recursive calls to nonexistent module Makefiles.
- Added BUILD-EXTRACTION-MAP.md with the actual 1,264-line Makefile ownership
  map, tool/host dependencies, blocking header and assembly dependencies, and
  clean-clone acceptance requirements.
- The root Makefile remains the only build-rule source of truth today. This is
  an intermediate correction, not completion of repository separation.
- No .gitignore, .gitmodules, external submodule source, Coreutils gitlink,
  or remote repository was changed by this corrective audit.


## NX-CORE standalone build delivered — 2026-11-08

- The canonical kernel source manifest and all kernel/bootloader build rules
  now live in kernel/build.mk. Only kernel/Makefile includes this fragment.
- The root Makefile invokes kernel/Makefile through make -C kernel; it no
  longer owns kernel source lists, kernel flags, or kernel compile/link rules.
- Kernel standalone builds and exact toolchain checks pass on amd64 and
  aarch64. A clean isolated copy of kernel/ built both architectures without
  any parent project files.
- Root delegated kernel and bootloader builds pass for both architectures.
  Root make check passes for both architectures.
- AMD64 GRUB BIOS and AArch64 GRUB arm64-efi images built and booted. AArch64
  kernel self-tests: 12 passed, 0 failed. Both kernel-only boots correctly
  report missing /sbin/nxinit and remain in K1+K2 healthy mode without reboot.
- The old unreferenced cpp_test.cpp stub was removed after confirming no
  caller, no link input and no observable test behavior; no C++ compiler is
  required by NX-CORE's actual C/assembly kernel build.
- The kernel's local legacy contract snapshot is explicitly transitional.
  The 2026-07-18 maintainer ruling against a shared root include/abi directory
  is authoritative; future SDK headers must be spec-derived and versioned.
- The root .gitignore and all upstream submodule source trees remain untouched.
- NX-VIOLA and NX-SDK are not yet independent build repositories. Work now
  continues on the SDK library/runtime export and then the VIOLA app build.
