# Build extraction map — source-of-truth audit

Date: 2026-11-08
Scope: existing root Makefile (1,264 lines), host wrappers, toolchain lock,
module trees, boot/image scripts and current repository contract documents.

This records the existing implementation before recipes are moved. It is not
permission to recreate or duplicate those recipes.

## Required four-project topology

| Project | Owns | Must build without |
|---|---|---|
| NX-CORE | Existing kernel, per-ISA HAL, kernel tests, boot assembly, linker scripts, symbol generation, kernel-only build and GRUB/direct QEMU boot | NexsOS userland and superproject; NX-SDK is development tooling only, never a kernel userland API/runtime input |
| NexsOS userland | Existing programs, init, shell, services, resources, app manifests and userland tests | NX-CORE checkout and superproject |
| NX-SDK | NexsOS-owned libc/runtime/native API, audited ABI/API headers, startup/syscall client objects, portability adapters, system-library archives, versioned development/build tools and SDK build/test metadata | NX-CORE implementation and userland sources |
| NexsOS1 integration | Pins the three repos and upstream dependencies; assembles rootfs, GRUB/EFI media, disk images and release; coordinates module builds and nxadb tests | No private module implementation duplicated here |

NX-SDK is a peer repository. NX-CORE consumes its development-tool plane (toolchain lock/setup/build/debug tooling) but not its userland API/runtime. Kernel/HAL source continues to use NX-CORE-owned contract snapshots; cross-repository compatibility is verified by the superproject.

## Existing root Makefile ownership map

| Existing lines/section | Existing content | Destination |
|---|---|---|
| 1–78 | Root entry, host wrapper selection, target/profile defaults, release vars | Integration orchestration; shared settings become explicit config inputs |
| 79–173 | Freestanding flags, architecture flags, compiler/binutils selection, include roots | Split compiler-domain flags into NX-CORE/NX-SDK; host config stays integration |
| 175–218 | mkdisk, GRUB, build paths, outputs, user paths | Module output paths local to modules; mkdisk/GRUB/image staging integration |
| 224–357 | Boot, ISA assembly, kernel C/C++ manifests, object/dependency derivation | NX-CORE; preserve source list exactly |
| 363–456 | Setup/check deps, all/dirs, bootloader/kernel, two-pass ksyms link | Kernel/bootloader/ksyms NX-CORE; integration coordinates |
| 458–465 | Linked SDK/client library object list | NX-SDK; exported object/library manifest consumed by NX-VIOLA |
| 476–538 | SDL2 upstream build and NexsOS overlay | SDK overlay + pinned upstream; archive build NX-SDK |
| 544–636 | Lua upstream build, portability shim and OS1 archive | SDK shim/archive + pinned upstream Lua |
| 654–680 | Gnulib overlay/archive | Generic overlay NX-SDK; Coreutils-specific glue NX-VIOLA |
| 692–735 | User program ELF manifests and user module boundary | NX-VIOLA |
| 737–901 | User source compilation, link rules and app fragments | NX-VIOLA; runtime objects come from NX-SDK |
| 902–988 | Rootfs staging and installation copies | Integration; consume module install manifests |
| 989–1035 | mkdisk, QEMU profiles, GRUB/EFI ISO | mkdisk/image orchestration integration; NX-CORE exports bootable kernel artifacts |
| 1041–1070 | Release assembly and release test | Integration |
| 1082–1150 | AArch64 DTB generation, run/debug modes | Integration QEMU orchestration; DTB handoff contract published to NX-CORE |
| 1156–1264 | lockcheck, disassembly, toolchain check, clean/help, DOOM fragment, FDT and nxadb tests | Kernel-local FDT/diagnostics NX-CORE; app tests NX-VIOLA; cross-module nxadb/release tests integration |

## Existing non-Makefile dependencies

- system/config/toolchain-versions.env: canonical NX-SDK-owned lock for both cross compilers. Root and NX-CORE setup entrypoints delegate to the SDK tooling instead of carrying drifting copies.
- tools/gen_ksyms.sh: kernel build input; canonical owner NX-CORE.
- kernel/tools/build-amd64-grub-iso.sh, kernel/tools/build-aarch64-uefi-image.sh and the GRUB target helpers are NX-CORE boot-media tooling. The pinned upstream source is the nested `kernel/external/grub` gitlink and `kernel/config/grub.env` records the source/build contract. The superproject consumes the resulting boot artifacts for integrated media/release assembly.
- tools/mkdisk.c: integration-owned host utility; recipe currently hardcodes
  gcc instead of HOST_CC.
- tools/nxqemu.py, nxadb.py, nxrun.py and capture/test scripts: integration
  orchestration and acceptance.
- tools/setup-dependencies.sh: root .gitmodules authority; integration only.
- Makefile.linux and Makefile.macos: host defaults, not independent build
  systems; they include/delegate to the root Makefile.
- user/sys/lib/portability overlays: NexsOS-owned generic overlays belong in
  NX-SDK; application-specific overlays remain NX-VIOLA.
- Existing external gitlinks under user/sys/lib and user/bin are upstream
  projects. Preserve URLs, commits, nested submodules and local dirty state.

## Blocking dependency defects

1. KERNEL_INCLUDE currently includes include/api; later INCLUDE is overwritten
   with kernel-private + ABI + API paths. Comments say kernel must not consume
   userland API headers, but 19 kernel-to-user header references were found.
   Classify each include and refactor actual consumers; do not copy whole trees.
2. include/abi/posix_types.h mixes ABI layouts with kernel helpers (atomics,
   page size, alignment, container macros and memory barriers). Split only
   after all consumers/layout assertions are inventoried. ABI layouts need one
   canonical spec and a published SDK contract package.
3. user/arch/amd64/syscall.S and user/arch/aarch64/syscall.S provide startup,
   syscall wrappers, setjmp and longjmp. Map exported symbols and consumers
   before assigning ownership. Keep assembly single-source.
4. user/sys/lib/lib.c combines libc/POSIX, native OS1 veneers and graphics
   compatibility. Split by exported symbols/callgraph, never arbitrary lines.
5. font_lib.c source inclusion and similar patterns require symbol ownership
   and duplicate-definition checks before relocation.
6. SDL/Gnulib/Lua manifests have upstream-specific warning/include policies.
   Preserve upstream source unmodified and keep NexsOS overlays in NX-SDK.
7. User ELF manifests and compile rules are tied to root build paths. NX-VIOLA
   needs an SDK-only link interface, with no kernel objects or private headers.
8. Rootfs currently copies headers and archives by hardcoded paths. Replace
   this with versioned install manifests exported by NX-SDK and NX-VIOLA.

## Mandatory independent-clone acceptance

### NX-CORE only
- Clone NX-CORE with no sibling repos present.
- Check and clean-build the existing kernel for amd64 and aarch64.
- Build its standalone boot artifact and boot it in QEMU; GRUB boot media is
  required where supported, direct boot remains diagnostic.
- Capture complete serial output and assert HAL init, kernel-ready marker and
  zero panic/fault state.
- Prove dependency graph has no user/, NX-SDK implementation or root path.

### NexsOS userland + NX-SDK only
- Build/copy the first-party userland with NX-SDK without NX-CORE or the superproject; external upstream modules remain superproject-managed.
- Build SDK headers, startup/runtime objects and all declared libraries.
- Compile and link a freestanding SDK sample for both target ISAs.
- Build first-party userland ELFs against only the SDK exported interface.
- Prove no dependency on kernel/, kernel-private include roots, kernel objects,
  root Makefile variables or rootfs outputs.

### Integration after both module gates pass
- Pin exact module commits and upstream gitlinks.
- Build rootfs from SDK/userland install manifests and the superproject-owned external module graph.
- Assemble GRUB BIOS and arm64-efi media and disk images.
- Run nxadb shell acceptance and release tests on both ISAs.
- Verify clean clone bootstrap without dirtying source trees.

A successful make all from the old superproject is not evidence for either
independent-build requirement.


## NX-CORE extraction implemented — 2026-11-08

The kernel build is now genuinely module-owned:

- The existing architecture-specific source manifest, object derivation,
  dependency discovery, bootloader link, two-pass kernel/ksyms link, binary
  conversion and kernel compile patterns were moved from the root Makefile to
  NX-CORE's canonical kernel/build.mk. There is one copy of each rule.
- kernel/Makefile is the standalone entrypoint. It sets its own target flags,
  compiler/binutils, output root, contract include roots, checks, clean target,
  boot-media targets and QEMU run targets.
- The integration Makefile no longer includes kernel/build.mk and no longer
  defines kernel source lists, kernel compiler flags or kernel compile/link
  recipes. Its kernel and bootloader targets invoke make -C kernel and pass
  only ARCH and the shared output root. Rootfs/disk/release/QEMU integration
  remains in the superproject.
- NX-CORE has its own .gitignore, pinned toolchain lock, macOS/Linux bootstrap
  scripts, symbol generator, GRUB BIOS and arm64-efi image builders, and local
  contract-snapshot checksum test.
- NX-CORE's legacy ABI declarations are a kernel-local snapshot. The
  maintainer's 2026-07-18 ruling supersedes any suggestion that root
  include/abi is the canonical shared source: the shared directory is
  conceptually wrong. Kernel-owned definitions stay in NX-CORE; the SDK will
  get spec-derived, versioned declarations and compatibility tests.
- The unreferenced C++ proof-of-concept driver was audited: it had no callsite,
  was not linked by KERN_OBJECTS, and was a stub. Its declaration, build
  remnants and source were removed instead of keeping a dead toolchain
  requirement.

### Verified results

- make -C kernel clean all ARCH=amd64: PASS.
- make -C kernel clean all ARCH=aarch64: PASS.
- make -C kernel check ARCH=amd64/aarch64: PASS, including exact GCC and
  Binutils versions and contract snapshot checksum.
- A clean rsync copy of kernel/ to /tmp/NX-CORE-isolated, with no superproject,
  userland or SDK tree present, clean-built kernel.elf/kernel.bin and
  bootloader.bin for both target architectures: PASS.
- Root make kernel and make bootloader delegated into NX-CORE and produced the
  expected artifacts for amd64 and aarch64: PASS.
- make check ARCH=amd64 and make check ARCH=aarch64 in the superproject: PASS.
- AMD64 GRUB BIOS ISO built and booted. Serial output reached HAL, SMP, VFS,
  scheduler and compositor initialization. With no rootfs/userland present,
  K3 reported missing /sbin/nxinit and explicitly continued in kernel-alone
  mode (K1+K2 healthy, no reboot); this is the expected kernel-only boot state.
- AArch64 GRUB arm64-efi ISO built from GNU GRUB 2.14 and booted through EDK2.
  Serial output reached kernel-alone mode; KTEST reported 12 PASSED, 0 FAILED.
- Direct QEMU diagnostic boots also succeeded on both architectures.
- Build artifacts are ignored by kernel/.gitignore. Root .gitignore remains
  untouched.

### Still open after NX-CORE

- Boot the isolated /tmp/NX-CORE-isolated artifacts themselves, not only the
  module working tree, as a final clean-checkout boot gate.
- Finish SDK extraction and the SDK-only sample-program build.
- Finish NX-VIOLA build extraction against the SDK export manifest.
- Finalize the SDK canonical mount path from the ownership matrix; the plan
  forbids inventing root sdk/ or nesting SDK inside NX-VIOLA.
- Move upstream gitlinks to their documented independent dependency location
  user/lib without changing commits, URLs, local dirty state or nested refs.
- Create the three independent Git repositories and pin them in the root
  integration superproject only after the remaining module acceptance gates.
