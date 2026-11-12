# Build and Integration Inventory — 2026-11-08

## Build entrypoints

- `Makefile`: 1,231 lines; current top-level build and integration graph.
- `Makefile.linux`: host compatibility variables; includes the main Makefile.
- `Makefile.macos`: host compatibility variables; includes the main Makefile.
- `tools/setup-project.sh`: selects host setup by `uname -s`.
- `tools/setup-toolchain-linux.sh`: Linux dependency and pinned cross-toolchain bootstrap.
- `tools/setup-toolchain-macOS.sh`: macOS dependency and pinned cross-toolchain bootstrap.
- `tools/toolchain-versions.env`: shared compiler/binutils version and source-hash lock.

## Current target toolchains

| Target | Compiler | Binutils | Current lock |
|---|---|---|---|
| amd64 | x86_64-elf-gcc | x86_64-elf-binutils | GCC 16.1.0 / Binutils 2.46.1 |
| aarch64 | aarch64-none-elf-gcc | aarch64-none-elf-binutils | GCC 7.2.0 / Binutils 2.29.1 |

The AArch64 compiler is deliberately pinned; do not replace it with a modern distro compiler without proving equivalent freestanding code generation and runtime behavior on both host OS families.

## Current source ownership candidates

- Kernel C and assembly lists are explicit in the main Makefile and architecture-specific.
- Userland programs and services are listed in `SBIN_ELFS`, `SYS_ELFS`, and `BIN_ELFS`.
- Userland syscall/startup objects are selected from `user/arch/$(ARCH)/syscall.S`.
- NexsOS-owned library sources and portability overlays are currently under `user/sys/lib`.
- Public/shared header candidates are currently under `include/abi` and `include/api`.
- Upstream library submodules currently under `user/sys/lib` must be treated as independent dependencies, with the intended location `user/lib`; do not assign their source ownership to NX-VIOLA or NX-SDK by path alone.
- Userland application submodules remain independently versioned upstream/fork dependencies.

## Kernel build

- Architecture-specific boot source list: `BOOT_SOURCES`.
- Architecture-specific kernel assembly: `KERN_ASM_SOURCES`.
- Architecture-specific and common C sources: `KERN_C_SOURCES`.
- Current kernel outputs: `kernel.elf`, `kernel.bin`, map, symbols, object and dependency files.
- The current build must continue to use the existing `kernel/arch/<arch>/hal.c` implementation.
- NX-CORE's future standalone build must consume only the SDK's kernel-safe ABI surface and must not include userland libc/POSIX API headers or build userland/rootfs.

## Userland and SDK/API build boundary

Current linked user library objects:
- `user/sys/lib/lib.c`
- `user/sys/lib/math.c`
- `user/sys/lib/extensions/services/execsvc_client.c`
- `user/sys/lib/portability/os1_video_platform.c`
- `user/sys/lib/portability/d3d9/os1_d3d9_present.c`
- `user/sys/lib/portability/opengl/os1_gl_platform.c`
- `user/sys/lib/malloc.c`

Additional upstream integration:
- SDL2 static archive, with NexsOS SDL video/input/timer overlay.
- Lua core, Lua portability adapter, and OS1 Lua library.
- Gnulib static archive and NexsOS glue.
- Coreutils with NexsOS overlay.
- Mesa/OpenGL, Wine/Direct3D, and musl submodules exist; do not infer that they are all currently built or installed simply because they are checked out.

The third repository is intended to own NexsOS system-library and portability/API implementation. Its canonical checkout path is not assigned here: `include/` and `user/sys/lib/` currently contain candidate material, so the single-repository boundary must be designed after file/symbol/include ownership is complete. Do not invent a root-level `sdk/` mount point.

## Assembly and linker inputs

- Kernel assembly belongs to NX-CORE: boot headers/stages, entry, exception/ISR, syscall entry, context switching, trampoline, and architecture-specific primitives.
- User syscall/startup assembly is currently in `user/arch/<arch>/syscall.S`; its ownership must follow the public ABI/runtime contract, not the current directory.
- Preserve every required `.S`, linker script, generated symbol assembly, and per-ISA flag.
- Current generic `.S` compilation uses CFLAGS; dedicated assembler/boot rules must be introduced during build separation.
- Every assembly source must be listed explicitly per target architecture; do not use recursive wildcard discovery.

## Rootfs, disk, GRUB, release

- `rootfs` currently depends on userland, SDL2, Lua, Gnulib, and Coreutils.
- Rootfs copies executables, resources, static libraries, and API/ABI headers into the staging tree, including `/sys/lib/include`.
- `tools/mkdisk.c` is a host-side tool; its current recipe hardcodes `gcc`.
- Preserve current extents and legacy mkdisk layouts and their regression tests.
- AMD64 ISO: GRUB BIOS.
- AArch64 ISO: GRUB arm64-efi with project EFI loader and DTB.
- `release` builds both targets; `release-arch` packages ISO, disk image, and architecture-specific kernel/DTB/boot manifest.
- Add deterministic checksums, complete module/dependency provenance, and a versioned artifact manifest as part of release hardening.
- Do not conflate the GPT disk image produced by mkdisk with a bootable disk; current documented GRUB ISO remains the verified boot route.

## nxadb acceptance

- `tools/nxadb.py` controls QEMU through QMP and injects commands into the guest NXShell.
- It archives prior runtime logs and preserves serial, QEMU stdout, QEMU stderr, and run manifests.
- One-shot command result is inferred from shell output/prompt. Verify exact expected output rather than treating host-side exit status as guest command status.
- The split acceptance gate is: build target -> GRUB boot -> wait for NXShell prompt -> send a deterministic command -> verify expected output -> retain complete logs and run manifest.
- Run independently for amd64 and aarch64.

## Host/target matrix

| Host | Host CPU | Target amd64 | Target aarch64 |
|---|---|---|---|
| macOS | Intel x86_64 | build + GRUB + shell + nxadb | build + GRUB + shell + nxadb |
| macOS | Apple Silicon arm64 | build + GRUB + shell + nxadb | build + GRUB + shell + nxadb |
| Linux | x86_64 | build + GRUB + shell + nxadb | build + GRUB + shell + nxadb |
| Linux | arm64 | build + GRUB + shell + nxadb | build + GRUB + shell + nxadb |

Current local evidence covers macOS Intel only. Linux and Apple Silicon are not certified by this inspection.


## Build-boundary changes and clean verification — 2026-11-08

Public module entrypoints:
- `make -C kernel ARCH=amd64|aarch64`
- `make -C user ARCH=amd64|aarch64`

The root `make all` now delegates the kernel and userland build phases through those entrypoints. The module files currently delegate back to private root targets; they are a stable seam for the next extraction step, not yet independent implementations.

Compiler domains are now separate:
- Kernel C/C++ and kernel assembly use `KERNEL_CFLAGS` / `KERNEL_INCLUDE`.
- Boot assembly uses the kernel compile domain to preserve existing relocation mode.
- Userland C and syscall assembly use `USER_CFLAGS` / `USER_INCLUDE`, without `kernel/include` or `kernel/arch/<arch>/include`.
- Kernel still sees `include/api` for existing cross-layer dependencies; do not remove it until those declarations have been classified and migrated to the kernel's own contract.

Clean build evidence on macOS Intel:
- amd64: `build/runs/repository-split-clean-build-amd64-20261108.log`, exit code 0.
- aarch64: `build/runs/repository-split-clean-build-aarch64-20261108.log`, exit code 0.
- Both include rootfs and mkdisk image generation.
- Warning findings in upstream and userland code remain open. A successful build is not a boot acceptance test.
- nxadb shell interaction, Linux host, and Apple Silicon host are still unverified.


## Fresh GRUB/nxadb acceptance

See `BOOT-ACCEPTANCE.md` for the complete test record. Both freshly rebuilt targets booted through GRUB and reached NXShell on the macOS Intel host. A unique shell command marker was injected through nxadb and captured in each complete serial log. nxadb reported shell present and faults=0 for both. QEMU stderr files are empty. Guests were stopped after testing.

Layering audit after the build split:
- 19 kernel→userland header references remain.
- 0 userland→kernel code references.
- 0 generated userland dependency paths into kernel include roots.


## Service extension verification and portability ownership

The notification client and error policy are now separate from lib.c. The execsvc client has its own extension translation unit. Both target builds and GRUB boots passed after the change. nxadb notification smoke tests passed on amd64 and aarch64 by invoking /sys/bin/nxnotify; the service registry listed the submitted message. Logs are build/runs/repository-split-notify-service-smoke-20261108.log and build/runs/repository-split-notify-service-smoke-aarch64-20261108.log.

See LIBRARY-CLASSIFICATION.md and PORTABILITY-CLASSIFICATION.md for source ownership, external upstream boundaries and unresolved feature classifications.

## NX-CORE standalone extraction update — 2026-11-08

- Kernel source manifest and all kernel/bootloader compile/link rules are now
  owned by kernel/build.mk. Only kernel/Makefile includes this canonical file.
- The root Makefile no longer includes kernel/build.mk or owns kernel source
  lists, kernel flags, or kernel compile/link recipes. It delegates kernel and
  bootloader targets with make -C kernel, ARCH and the shared output root.
- NX-CORE has its own toolchain lock, macOS/Linux bootstrap scripts, symbol
  generator, GRUB BIOS and arm64-efi image builders, checksum gate and .gitignore.
- Standalone clean builds and exact toolchain checks pass for amd64 and
  aarch64. An isolated copy of kernel/ with no parent checkout also built and
  booted both architectures.
- AMD64 GRUB BIOS and AArch64 GRUB arm64-efi images built and booted. AArch64
  KTEST: 12 PASSED, 0 FAILED. Kernel-only boot correctly remains in K1+K2
  healthy mode because /sbin/nxinit is absent.
- The kernel uses only its local legacy contract snapshot and local ELF/compiler
  headers. It no longer sees top-level include/api or include/abi. The
  2026-07-18 maintainer ruling rejects a shared root include/abi directory;
  future SDK declarations must be spec-derived and versioned.
- NX-VIOLA and NX-SDK still need independent Makefiles and clean-checkout tests.
