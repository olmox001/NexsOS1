# NexsOS1 Four-Project Separation Plan

Date: 2026-11-08
Status: NX-CORE standalone build/GRUB boot delivered and verified on both target ISAs; NX-SDK and NX-VIOLA build extraction and repository creation remain in progress.

## 1. Objective

Keep NexsOS1 as the main integration superproject and split project-owned code into three independently versioned repositories:

1. **NX-CORE** — the existing kernel rooted at `kernel/`.
2. **NX-VIOLA** — the existing userland rooted at `user/`.
3. **NX-SDK / SDK-API** — the NexsOS-owned system libraries and portability/API implementation. This is the third repository requested by the maintainer. Its canonical mount point and final repository name must be established from the actual source ownership graph. A root-level `sdk/` directory must not be invented.

The superproject remains responsible for cross-module integration, external dependency pins, host tools, rootfs, disk images, GRUB, release orchestration, end-to-end tests, and provenance.

The four projects are NX-CORE, NX-VIOLA, NX-SDK and the NexsOS1 integration superproject. This is a repository and build-system separation, not a rewrite of the kernel, HAL, libc, userland, or external libraries. Every implementation must have one canonical owner and one compiled definition.

## 2. Repository responsibilities

### 2.1 NX-CORE

Own:
- Existing kernel implementation and kernel-private headers.
- Existing amd64 and aarch64 architecture code, HAL implementations, boot/entry assembly, trap/interrupt assembly, context switching, MMU, CPU, and platform code.
- Kernel drivers, memory management, scheduler, syscall dispatch, object/capability kernel implementation, kernel filesystem code, and kernel tests that remain kernel-resident.
- Kernel linker scripts, kernel symbol generation inputs, and kernel build/test rules.

Do not:
- Add a second HAL or reimplement existing primitives.
- Include POSIX/libc userland headers.
- Build userland, upstream application libraries, rootfs, mkdisk, or release media.
- Depend on private build variables or source paths inside NX-VIOLA.

### 2.2 NX-VIOLA

Own:
- Existing userland programs, system services, init, shell, application code, user resources, and userland tests.
- Application-specific resources and install metadata.
- Userland build rules and target-specific program manifests.

Do not:
- Own NexsOS system-library implementation merely because a source currently sits under `user/sys/lib`.
- Own third-party upstream source merely because a submodule is currently nested under `user/`.
- Compile kernel C implementation files directly into userland.
- Maintain duplicate syscall/runtime implementations.

### 2.3 NX-SDK / SDK-API

Own:
- Versioned kernel/user ABI contracts and public API contracts after per-header classification.
- NexsOS system-library implementations (OS1 API, POSIX/C compatibility layer, libc/runtime, allocator and standard/system libraries).
- NexsOS-owned portability layers and platform integration glue.
- Userland syscall/startup/runtime objects only after every exported symbol, architecture path, and consumer is inventoried.
- SDK metadata, public include roots, library archives, startup objects, feature declarations, and compatibility tests.

Do not:
- Absorb third-party upstream projects as if they were NexsOS-owned code.
- Duplicate kernel or application implementations.
- Expose kernel-private helpers as public SDK contracts.
- Rely on the superproject's current Makefile variable state to build.

### 2.4 NexsOS1 integration superproject

Own:
- Pinned revisions for NX-CORE, NX-VIOLA, NX-SDK, and external dependencies.
- Host setup, toolchain bootstrap, host tools, and build orchestration.
- Rootfs staging and installation composition.
- GRUB BIOS/EFI integration and boot media composition.
- `mkdisk`, disk image creation, release packaging, checksums, provenance, and release manifests.
- `nxrun`, `nxqemu`, `nxadb`, and end-to-end dual-architecture acceptance.

The superproject must not contain duplicate copies of module-owned source or private compilation recipes for module internals.

## 3. Path and dependency rules

- Keep the current `kernel/` mount point for NX-CORE.
- Keep the current `user/` mount point for NX-VIOLA.
- Do not add a root-level `sdk/` directory without an explicit maintainer decision.
- The SDK/API repository must have one canonical Git root. Its present candidate sources are spread across `include/abi`, `include/api`, and NexsOS-owned parts of `user/sys/lib`. The final checkout path must be chosen only after the ownership matrix is complete.
- External upstream libraries are independent dependencies, not NX-VIOLA or NX-SDK source. Their intended location is `user/lib`; migration must preserve exact upstream commits, URLs, branches, local modifications, and nested submodules. Do not infer Git ownership from path.
- The current external submodules include SDL, Mesa, Wine/Direct3D, Lua, musl, and Gnulib. Userland application submodules include base-nexs, BusyBox, Coreutils, Doom, and kilo.
- Preserve Coreutils' nested Gnulib relationship and local checkout state. Do not modify the Coreutils gitlink or its nested state during this work.
- The SDK/API checkout path is an architectural decision, not a reason to create an arbitrary new directory or to put SDK source inside NX-VIOLA.

## 4. Dependency direction

The required compile-time dependency graph is:

```text
NexsOS1 integration superproject
  ├── NX-CORE ─────┐
  ├── NX-VIOLA ────┼── NX-SDK / SDK-API contracts
  └── external dependencies (independently pinned)
```

Rules:
- NX-CORE consumes only kernel-safe ABI definitions and toolchain contracts.
- NX-VIOLA consumes SDK public APIs, system libraries, and explicitly declared upstream dependencies.
- NX-SDK does not depend on application code or kernel-private implementation headers.
- External upstream projects are built with their own upstream build systems plus NexsOS-owned overlays/adapters.
- No module imports another module's private Makefile or reaches into undocumented internal source paths.
- Avoid cycles: SDK headers cannot depend on userland services; userland cannot supply symbols required to link the kernel; kernel internals cannot be pulled into SDK libraries.

## 5. SDK/API standard (normative contract to implement)

The SDK must be a real interface, not just a relocated include directory. It must expose a versioned and machine-verifiable contract.

### 5.1 Contract layers

1. **Kernel-safe ABI**: syscall numbers, calling convention, argument registers, clobbers, return/error convention, object/capability wire types, fixed-width layouts, alignment, endianness, and ABI version.
2. **OS1 native API**: canonical OS1 functions and semantics, separated from POSIX naming.
3. **POSIX/C API**: actual adapters over OS1 primitives. Compatibility names must have real implementations and tests; aliases that merely rename symbols are not a substitute.
4. **System libraries**: NexsOS-owned libc/runtime, allocation, string/format/math, exec service client, graphics/input interfaces, and other libraries established by symbol census.
5. **Portability**: NexsOS-owned OS adaptation for upstream projects, kept outside upstream source trees.
6. **Startup/runtime**: per-ISA syscall stubs, `_start`, CRT/startup objects, linker contract, stack/heap initialization, and any required assembly.
7. **SDK build metadata**: public include roots, library names, startup object list, target ABI version, supported architectures, feature flags, and dependency requirements.

### 5.2 Required SDK artifacts

Each SDK release must provide:
- Versioned SDK manifest.
- ABI version and compatibility declaration.
- Public headers and their dependency closure.
- Per-architecture library archives and startup objects.
- Link requirements and linker-script contract, where applicable.
- Feature/capability declarations.
- Header self-containment and ABI layout test reports.
- Exported symbol inventory and compatibility report.

The SDK manifest is the single input consumed by NX-CORE and NX-VIOLA. Neither consumer reconstructs SDK paths or library lists independently.

### 5.3 Header and ABI classification

Do not move all of `include/` as one unit. For each header:
- Identify all direct and transitive consumers.
- Separate wire ABI definitions from kernel-private helpers and userland convenience APIs.
- Identify all structures shared across privilege boundaries and prove size, alignment, offsets, and integer widths on amd64 and aarch64.
- Remove accidental API visibility from kernel compile flags.
- Preserve the existing syscall numbers and semantics unless a separately reviewed ABI migration explicitly changes them.
- Compile each public header in isolation under the supported C dialect and any supported C++ mode.
- Avoid hidden include dependencies, circular headers, implicit padding, and macros that conceal unsafe pointer or control-flow behavior.

Known current issue: `include/abi/posix_types.h` combines ABI data with kernel helpers including atomics, page constants, alignment helpers, memory barriers, and `container_of`. The Makefile also passes `include/api` to kernel compilation. Both must be corrected through source-level classification, not by creating duplicate replacement headers.

### 5.4 Assembly and runtime boundary

The user assembly files `user/arch/amd64/syscall.S` and `user/arch/aarch64/syscall.S` currently include `_start`, syscall wrappers, `setjmp`, and `longjmp`. They are runtime/ABI surfaces, not merely syscall trampolines.

Before transfer:
- List every exported symbol and all link-time/runtime consumers.
- Record architecture-specific register, stack, and error conventions.
- Prove the userland linker includes each required object exactly once.
- Define a separate assembly compilation rule and per-ISA flags.
- Keep kernel boot/trap/context assembly in NX-CORE.
- Keep external upstream assembly in its upstream dependency.
- Resolve the existing Rule 1 conflict caused by `setjmp`/`longjmp`; do not silently remove Lua compatibility or claim compliance.

## 6. Independent build systems

### 6.1 NX-CORE build

Inputs:
- NX-CORE sources and private headers.
- SDK manifest and kernel-safe ABI headers.
- Explicit host tools and target cross-toolchain.

Outputs:
- Kernel ELF and binary.
- Link map, symbol table, dependency files.
- Build manifest, compiler identity, warning report, and test report.

It must not build userland, system libraries, external application libraries, rootfs, disk images, or release media.

### 6.2 NX-SDK build

Inputs:
- SDK/API source and headers.
- Explicit per-ISA startup/assembly sources.
- Target toolchain and declared external build interfaces where required.

Outputs:
- Public headers.
- System-library archives.
- Per-ISA startup objects.
- SDK manifest, symbol inventory, ABI/header test results, and warning/static-analysis reports.

It must not compile the kernel or applications as a side effect.

### 6.3 NX-VIOLA build

Inputs:
- NX-VIOLA source.
- A specific compatible SDK manifest.
- Explicit external dependency manifests and upstream build configurations.

Outputs:
- Userland ELF binaries and services.
- User resources and install manifest.
- Per-application dependency/link reports.
- Test and warning/static-analysis reports.

It must not compile kernel implementation sources or duplicate SDK library objects.

### 6.4 NexsOS1 orchestrator

The root Makefile becomes an orchestration interface:
- Resolve host OS/CPU, target ISA, toolchain, profile, and module revisions.
- Invoke the independent module builds.
- Validate SDK/ABI compatibility before linking.
- Stage the rootfs from module install manifests.
- Build GRUB media and invoke mkdisk.
- Package releases and provenance.
- Run nxrun/nxadb end-to-end gates.
- Preserve existing user-facing targets where practical (`all`, `kernel`, `user`, `rootfs`, `disk`, `boot-iso`, `release`, `test`).

No module build may depend on hidden variables set only by the top-level Makefile.

## 7. Host and cross-compilation standard

Host OS, host CPU, target ISA, toolchain, and build profile are independent dimensions.

Required host matrix:
- macOS Intel x86_64.
- macOS Apple Silicon arm64.
- Linux x86_64.
- Linux arm64.

Every host must build both targets:
- amd64 (x86_64-elf).
- aarch64 (aarch64-none-elf).

Requirements:
- Separate `HOST_CC`, `HOST_CXX`, `HOST_AR` from target `TARGET_CC`, `TARGET_CXX`, `TARGET_AS`, `TARGET_LD`, `TARGET_AR`, `TARGET_OBJCOPY`, and `TARGET_OBJDUMP`.
- Keep compiler/binutils versions and source checksums locked in one shared toolchain manifest.
- Detect host OS and CPU explicitly; never infer target architecture from host architecture.
- Do not use distro-native Linux ABI compilers for the freestanding kernel.
- Verify Apple Silicon native bootstrap without assuming Rosetta is equivalent to native support.
- Verify Linux bootstrap and both target toolchains on Linux x86_64 and arm64.
- Make setup idempotent and safe to re-run; dependency updates must not silently move pinned submodules.
- Separate build output by host OS, host CPU, target ISA, profile, toolchain identity, and module to prevent object reuse across incompatible configurations.
- Record compiler commands and versions in build manifests.

Suggested output key (final directory spelling to be settled in build design):
`build/<host-os>-<host-cpu>/<target-isa>/<profile>/<module>/`.

## 8. Assembly, linker, and generated sources

- Each module owns explicit per-architecture source manifests.
- No recursive wildcard discovery for critical assembly, boot, linker, or startup sources.
- C and assembly must receive consistent target ABI and CPU feature flags.
- Use dedicated rules for `.S`, `.s`, generated assembly, boot assembly, and host assembly tools.
- Current `ASFLAGS` and `CFLAGS_BOOT` must have real consumers or be removed only after proving they are obsolete.
- Preserve all linker scripts and generated symbol assembly required by each target.
- Add compile-only and link-only tests for every startup object and linker contract.
- Verify the final userland ELF entry point and required runtime symbols on both architectures.

## 9. Rootfs, mkdisk, GRUB, and release system

### 9.1 Rootfs interface

- NX-VIOLA and NX-SDK each publish an install manifest.
- The superproject stages files using those manifests; it must not hardcode private source paths.
- The SDK manifest defines runtime libraries, headers intended for developer/runtime installation, and their versions.
- Preserve the existing runtime layout (including `/sys/lib`) until tests prove any layout change safe.
- Detect file collisions, missing dependencies, duplicate libraries, and unresolved symbols before disk creation.

### 9.2 mkdisk

- `tools/mkdisk.c` remains a host tool in NexsOS1.
- Build it with `HOST_CC`, not a hardcoded `gcc`.
- Its input is the staged rootfs plus explicit boot/kernel/image inputs.
- Preserve extents and legacy layout modes and all existing regression cases.
- Test deterministic image output, inode/block bounds, and failure paths.
- Do not conflate the current root filesystem disk image with a bootable GRUB medium.

### 9.3 GRUB and boot

Preserve current verified paths:
- amd64: GRUB BIOS ISO.
- aarch64: GRUB arm64-efi ISO and project EFI loader with kernel/DTB handoff.

Keep direct-kernel QEMU boot as a diagnostic path only. Do not claim AMD64 UEFI support unless a complete UEFI boot chain is implemented and verified.

### 9.4 Release

The superproject release pipeline must:
- Pin exact commits for all three code modules and every external dependency.
- Reject incompatible SDK/ABI/module combinations.
- Build both target architectures from clean, isolated output directories.
- Package ISO, disk image, kernel/DTB/boot manifest as applicable.
- Include a machine-readable release manifest with source revisions, toolchain versions, host/target, profile, and build identifiers.
- Generate and verify SHA-256 (or stronger) checksums for every release artifact.
- Preserve debug symbols/maps as separately identified artifacts where required.
- Test the packaged release image, not only the development build directory.
- Never overwrite an unrelated version or architecture release.

## 10. Runtime acceptance

The required real verification for each target is:
1. Build the selected architecture from the selected host.
2. Build the GRUB ISO and disk image through the new module interfaces.
3. Boot the GRUB ISO with nxrun/nxadb.
4. Verify kernel boot completion and NXShell prompt.
5. Send a deterministic shell command through nxadb.
6. Verify the exact expected output and shell prompt return.
7. Preserve full serial, QEMU stdout, QEMU stderr, QMP/run manifest, and build log.
8. Repeat for amd64 and aarch64.

The host certification matrix repeats this target gate on all four host classes. A successful dry-run, compilation alone, QEMU process launch, or kernel boot marker is not equivalent to an interactive shell test.

## 11. NASA/JPL Power of Ten

Apply the original ten rules to NexsOS-owned C and other in-scope safety-critical code. Compliance is measured per module, target, and commit.

- Rule 1: simple control flow; no goto, setjmp/longjmp, or recursion.
- Rule 2: statically bounded loops.
- Rule 3: no dynamic allocation after initialization.
- Rule 4: short functions, with the original one-page reviewability criterion.
- Rule 5: assertions that validate real invariants, with the original density requirement.
- Rule 6: minimum data scope.
- Rule 7: validate parameters and check every non-void return.
- Rule 8: minimal, simple preprocessor use.
- Rule 9: restricted pointer indirection and no function pointers.
- Rule 10: maximum practical compiler warnings, zero unexplained warnings, and recurring static analysis.

Required evidence:
- Reproducible checker/tool versions and configuration.
- Per-rule findings with file, symbol, line, severity, and disposition.
- No suppression flags or cast-away diagnostics to make the gate appear green.
- Any required deviation must be explicit, bounded, risk-assessed, tested, assigned, and approved; no implicit exception is assumed.
- The current `setjmp`/`longjmp`, allocator, macro, pointer, and warning findings remain open until resolved and verified.

## 12. Execution phases and gates

### F0 — Baseline and complete inventory
- Record Git state, all gitlinks, nested dependencies, source hashes, and current build artifacts.
- Generate a file/symbol/include ownership matrix from the real source tree and Makefile.
- Trace each compilation unit to its target, flags, link inputs, and output.
- Classify all headers, system libraries, portability glue, assembly, linker scripts, and generated inputs.
- Inspect current nxadb logs and perform a fresh baseline build/boot/shell interaction on both architectures before structural changes.
- Compare historical plans and mark claims superseded by current code.

Gate: every tracked project-owned file has one proposed owner; every external source has an independent dependency record; no ambiguous file is moved.

### F1 — SDK/API standard
- Freeze the current ABI baseline and enumerate all syscall/API symbols.
- Define SDK version, ABI version, compatibility policy, feature model, and manifest schema.
- Separate wire ABI, kernel-private headers, OS1 API, POSIX/C headers, libraries, portability, and startup objects.
- Add ABI layout, header self-containment, symbol, assembly, and consumer compile tests for both targets.

Gate: kernel and userland can consume the same declared SDK release without private path access or duplicate implementation.

### F2 — Independent build systems, without moving source
- Extract the NX-CORE build and prove parity with the current kernel build.
- Extract the SDK/API build and prove its libraries and startup objects are produced exactly once.
- Extract the NX-VIOLA build and prove parity for all current userland executables.
- Replace hidden root Makefile state with explicit module manifests and SDK interfaces.
- Keep the root Makefile as orchestrator.

Gate: both architectures build with equivalent symbols, link maps, executable entry points, and rootfs content; no duplicate compilation/linking.

### F3 — Git repository split
- Create/split repository history from the verified ownership matrix.
- Keep `kernel/` as NX-CORE and `user/` as NX-VIOLA.
- Mount the third SDK/API repository only at the path explicitly selected from the completed ownership map; do not invent root-level `sdk/`.
- Move external submodule mount points to the approved external dependency location while preserving URLs, commits, branches, and nested state.
- Update `.gitmodules`, module manifests, and dependency setup atomically.
- Verify recursive clone, clean initialization, and pinned checkout reproducibility.

Gate: each owned source has exactly one repository owner; the superproject reconstructs all three modules and external dependencies from a clean clone.

### F4 — Rootfs, disk, GRUB, and release integration
- Consume module install manifests.
- Update mkdisk to use host-toolchain variables and staged rootfs interfaces.
- Preserve GRUB boot paths and all required assembly/EFI inputs.
- Generate release manifests, checksums, and provenance.
- Test the packaged artifacts.

Gate: clean release builds for both target architectures, boots through GRUB, and passes nxadb shell interaction.

### F5 — Host portability certification
- Verify macOS Intel, macOS arm64, Linux x86_64, and Linux arm64.
- Build and test both target architectures on each host.
- Repair the Linux bootstrap/build path if it is currently broken.
- Verify native Apple Silicon rather than relying solely on Rosetta.
- Publish a host/target evidence matrix with logs and exact toolchain versions.

Gate: no platform is called supported without reproducible build + GRUB boot + shell + nxadb evidence for both target architectures.

## 13. Immediate next actions

1. Complete the source-to-owner matrix and dependency graph.
2. Resolve the single canonical checkout path for the SDK/API repository without creating root-level `sdk/`.
3. Inspect the current build/boot/nxadb evidence and run fresh baseline acceptance on amd64 and aarch64.
4. Write the SDK/API v1 contract and module build-manifest schema.
5. Implement independent builds while all sources remain in their current locations.
6. Only after build parity, perform the Git history split and submodule migration.

No source migration or repository creation is authorized by this plan alone.


## Maintainer direction update — 2026-11-08

This amendment is authoritative where earlier wording in this plan conflicts with it.

- NX-SDK is logically part of the NX-VIOLA platform, but remains an independently versioned repository. The superproject checks out NX-CORE, NX-VIOLA and NX-SDK as peers. NX-CORE consumes only the SDK's explicit kernel-safe contract subset; it must not depend on the NX-VIOLA checkout or userland runtime.
- The order is mandatory: first establish independent and reproducible kernel and userland build entrypoints and isolate integration orchestration; then implement executable versioned contracts; then extract libraries/portability into NX-SDK; only then create/pin the three repositories.
- Versioning must cover HAL↔kernel and kernel↔userland/SDK. The authoritative design is `CONTRACT-VERSIONING.md`. Version compatibility is directional and proven by a complete test matrix, not inferred from matching version numbers.
- Each requirement has a stable ID, a support classification (`FULL`, `SOFTWARE_ONLY`, `CONDITIONAL`, `REQUIRES`, `PARTIAL`, `EXPERIMENTAL`, `DEPRECATED`, `UNSUPPORTED`) and an independent verification state (`PASS`, `FAIL`, `NOT_RUN`, `BLOCKED`, `NOT_APPLICABLE`). Release manifests cannot claim a feature supported without passing evidence for the exact profile.
- The HAL contract must be implemented against the existing `kernel/arch/<arch>/hal.c` implementations. Do not create a second HAL.
- The current `user/sys/lib/lib.c` must be classified and split into standard C/POSIX runtime, NexsOS native API, service clients/extensions, and graphics/input/font/video portability. The old `docs/R0-CENSUS-LIB-SPLIT.md` is a historical symbol census and explicitly superseded in part; regenerate symbol/call-site/link maps from the current 4,985-line source. Do not move by old line numbers.
- Service-backed extensions are not standard libc. `notify_send`/`OS1_notify_*` is a confirmed client of the boot-started notification service; `execsvc_client.c` is already separate. Do not label graphics/window, registry/environment or VFS paths as service clients until current call graphs and kernel ownership prove it.
- Existing portability implementations under `user/sys/lib/portability/` belong in the source inventory. Preserve upstream trees and overlays separately; never patch upstream sources merely to relocate NexsOS glue.
- The 2026-11-08 forced amd64 build completed successfully (exit 0). A forced aarch64 build is running; warnings already observed in userland/upstream overlays mean no warning-zero claim is permitted. Neither build is yet a boot-to-shell/nxadb acceptance result.


## Build-system increment 1 — verified clean build

- Added module-facing entrypoints `kernel/Makefile` and `user/Makefile`. The superproject calls these from `all`; the entrypoints delegate to unique private targets in the existing root Makefile. This is an intentional compatibility seam, not yet a full extraction of the module recipes.
- Split compiler include/flag domains: `KERNEL_CFLAGS` and `KERNEL_INCLUDE` for kernel, boot and kernel assembly; `USER_CFLAGS` and `USER_INCLUDE` for userland and its assembly. Userland no longer receives `kernel/include` or per-architecture kernel include roots. Kernel still temporarily sees `include/api` because its 16 include dependencies have not yet been migrated into the versioned kernel contract.
- Removed duplicate compilation in the `user/sys/lib/%.o` rule and changed the nxfont source rule to use userland flags.
- First clean build exposed that the old generic assembly rule used kernel CFLAGS, not the previously unused `CFLAGS_BOOT`. Applying `-m32` to the amd64 boot header caused unsupported R_64 relocation. The boot assembly rule now uses the same kernel compile domain as the old rule; the clean build passed after this correction.
- After `make clean`, `make all ARCH=amd64` and `make all ARCH=aarch64` both completed with exit code 0 on the macOS Intel host. Both generated kernel, user ELFs, rootfs and disk image through the new module entrypoints.
- Full logs: `build/runs/repository-split-clean-build-amd64-20261108.log` and `build/runs/repository-split-clean-build-aarch64-20261108.log`.
- Warnings remain in SDL2, Lua, nxempire, Gnulib and Coreutils. They are recorded as open findings; no warning-zero or NASA compliance claim.
- Boot-to-shell and interactive nxadb acceptance are NOT YET VERIFIED for this clean build. No Linux-host or Apple-Silicon-host certification has been performed.

Next: finish extracting module-owned recipes and toolchain config from the root Makefile, then add machine-readable spec manifests and conformance tests before source/repository migration.


## Build and shell gate update — 2026-11-08

The clean-build gate and the required shell interaction gate now pass on the current checkout for both targets on the macOS Intel host. See `BOOT-ACCEPTANCE.md` for commands, markers, complete logs and limits. This supersedes the earlier pending-shell note above.
- amd64: clean build + GRUB BIOS + NXShell + nxadb command marker + faults=0.
- aarch64: clean build + GRUB arm64-efi + NXShell + nxadb command marker + faults=0.
- The layering checker now reports 19 kernel→userland header findings, zero userland→kernel code findings, and zero generated userland dependency paths into kernel include trees.
- Linux and Apple Silicon host matrix cells remain unverified. Warning-zero remains open.


## SDK library extraction increment — 2026-11-08

- Moved notification client and error-reporting policy out of lib.c into user/sys/lib/extensions/services/notify_client.c; moved execution client to extensions/services/execsvc_client.c; updated the explicit link object list and public header source reference.
- Both architecture builds, refreshed GRUB images, boot-to-shell and nxadb command tests passed after extraction. Separate notification service smoke tests passed on both targets.
- Portability ownership and known capability gaps are recorded in PORTABILITY-CLASSIFICATION.md. No upstream dependency tree was modified.
- Current API versions include BootInfo v1, sysstats v1, execsvc v2 and a private video ABI v1. There is no global HAL spec version, kernel ABI identity or notification protocol version. The old ABI matrix is an older source-baseline inventory, not current conformance evidence.
- Remaining before repository creation: fully extract kernel/user build recipes and shared host/target configuration; implement machine-readable spec manifests and conformance test runner; finish libc/native/graphics split; resolve 19 kernel→userland include findings; verify the full compatibility matrix.

## 2026-11-08 implementation status update

NX-CORE is now an independent build system: the existing source manifest and
kernel/bootloader rules live in kernel/build.mk, kernel/Makefile is standalone,
and the root Makefile only delegates. Clean builds, exact toolchain checks,
GRUB image creation, GRUB boot, and isolated-copy build/boot have passed for
amd64 and aarch64. See BUILD-EXTRACTION-MAP.md and BOOT-ACCEPTANCE.md.

The 2026-07-18 maintainer ruling in HANDOFF-2026-07-18.md is authoritative:
a shared root include/abi directory is conceptually wrong. The kernel-local
legacy snapshot is transitional; the SDK must receive versioned, spec-derived
declarations and compatibility fixtures. The earlier neutral shared-header
proposal is superseded.

Next, finish the source/symbol/header ownership matrix for NX-SDK, move
third-party upstream gitlinks to their documented independent dependency
location user/lib without changing refs, and extract the SDK build. Then
extract NX-VIOLA's application build against the SDK manifest. Do not create a
root sdk/ directory or nest NX-SDK inside NX-VIOLA; select the peer checkout
path from the completed ownership matrix.
