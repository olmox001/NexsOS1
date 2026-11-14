# NEXS Next Generation — Clean-Room Migration Specification

**Status:** proposed architecture; new kernel implementation has not started.
**Current baseline:** DEV-Resurrection, commit b495da3 (audit snapshot).
**Compatibility invariant:** existing userland source files and their behavior are preserved. No broad userland rewrite is permitted.

Current detailed contracts:
- ABI: [ABI-COMPATIBILITY-MATRIX.md](ABI-COMPATIBILITY-MATRIX.md)
- Core design, scheduler, capabilities, IPC and WCET: [NEXS-NG-CORE-DESIGN.md](NEXS-NG-CORE-DESIGN.md)
- Hardware/ISA/IOMMU/timing: [NEXS-NG-HARDWARE-AND-ISOLATION.md](NEXS-NG-HARDWARE-AND-ISOLATION.md)
- Current boot path and BootInfo contract: [BOOT-PROTOCOL-INVENTORY.md](BOOT-PROTOCOL-INVENTORY.md)
- Verified build/boot and legacy-kernel backup: [BUILD-BASELINE-2026-11.md](BUILD-BASELINE-2026-11.md)

## Current execution status — 2026-11-02

- macOS Intel bootstrap completed successfully using tools/setup-project.sh.
- Recursive submodules are pinned and initialized; setup is idempotent on this checkout.
- Toolchain versions are locked in tools/toolchain-versions.env, including GNU
  source SHA-512 values for Linux source builds.
- Full x86-64 and AArch64 builds succeeded on the Intel Mac; both QEMU boots
  reached the shell and passed 8 kernel tests.
- ABI compatibility and hardware-isolation specifications are now recorded.
- The original kernel has a locally verified 183-file archive; source remains
  present and unchanged.
- A true empty-clone bootstrap, Linux-host build, and Apple Silicon-host build
  remain unverified. The local kernel archive still needs an independent second
  copy before the original kernel can be removed.


## 1. Repository and build strategy

The current checkout remains a monorepo during migration. Its existing Git submodules are retained as external projects, with their exact revisions pinned by the superproject's gitlinks. Bootstrap must work from a fresh clone without relying on an already-populated developer machine.

Future repository boundaries are planned as:

- `nexs-kernel`: minimal hypervisor/microkernel, ISA backends, platform HAL and boot path.
- `nexs-userland`: applications, init, system services and root filesystem composition.
- `nexs-sdk`: public ABI, headers, libc integration, compiler/linker specs, SDK tools and tests.
- `nexs-external`: dependency manifest/lock metadata and integration overlays for separately maintained third-party projects. Individual upstream/fork repositories remain independent.

Until an explicit split migration, source ownership stays in the existing paths. Build modules must expose documented inputs/outputs and must not reach into another module's private implementation directories. Shared contracts belong in the SDK/ABI boundary.

### Fresh-clone bootstrap contract

1. Clone the superproject including its Git history.
2. Run `tools/setup-project.sh` on macOS or Linux.
3. Synchronize submodule URLs and initialize recursive submodules at the exact gitlink commits; never follow floating branch heads during normal setup.
4. Install/check host prerequisites and the pinned cross-toolchain.
5. Run `make check-deps`, `make check ARCH=aarch64`, and `make check ARCH=amd64`.
6. Build both architectures and run available automated/QEMU tests.

Setup is idempotent. It must fail on an unavailable dependency or mismatched required compiler rather than continue with a partially initialized tree. Optional release tooling must be explicitly reported as optional. No script may silently downgrade a failed dependency fetch to success.

## 2. Current platform/build facts to preserve

- Target architectures: AArch64 and x86-64 (Makefile names the latter `amd64`).
- Host build systems: macOS Intel, macOS Apple Silicon, Linux.
- Existing target outputs: bootloader, kernel ELF/raw image, userland ELFs, rootfs, disk image, release artifacts and QEMU run/debug paths.
- Existing userland libraries and applications remain external submodules where already maintained separately (SDL, Mesa, Wine/D3D, Lua, musl, Gnulib, BusyBox, Coreutils, DOOM, kilo, base-nexs).
- Third-party revisions are determined by Git gitlinks, not the `branch` hints in `.gitmodules`.
- The existing ABI in `include/abi` and `include/api` is the initial compatibility contract. Changes require versioning and explicit adapter layers.

A compiler version is not considered supported merely because it exists on PATH. Each host/target pair must pass compiler identity checks, compile/link probes, full clean builds, and QEMU boot tests. The current lock is:
- x86_64-elf GCC 16.1.0 / Binutils 2.46.1.
- aarch64-none-elf GCC 7.2.0 / Binutils 2.29.1.

Exact versions are enforced by make check. Linux source archives are SHA-512
verified before extraction; macOS installs the matching Homebrew/osxct packages.
The lock is verified on macOS Intel. Linux-host and Apple Silicon-host
validation remain open gates.

## 3. Target trust boundary

The new trusted computing base is deliberately small:

- CPU entry/trap handling and context switching.
- Address-space and page-table primitives.
- Capability validation and object lifetime primitives.
- Scheduler and thread/process state transitions.
- IPC and bounded notification/wakeup mechanisms.
- Interrupt routing and per-CPU timer primitives.
- Minimal boot/platform discovery and fault containment.

The hypervisor function is optional per hardware capability. On x86-64, VMX or SVM is detected and enabled only through a verified platform backend. On AArch64, EL2 availability and stage-2 translation are detected before enabling virtualization. Absence of hardware virtualization must not prevent ordinary microkernel operation. No assumption is made that a given host firmware exposes virtualization.

## 4. Hardware description and ISA dispatch

Use a normalized immutable hardware description assembled from platform-specific sources:

- AArch64: firmware Device Tree (FDT), with ACPI where firmware provides it.
- x86-64: ACPI tables, PCI/PCIe enumeration, UEFI/boot protocol data; FDT only where the platform actually supplies it.
- Both: CPU topology/features, memory map, interrupt controllers, timer sources, DMA/IOMMU, power-management interfaces and discovered devices.

The normalized tree records provenance, validity, resource ownership and capability requirements. Unknown or malformed nodes are quarantined; they are not guessed into existence.

Separate mandatory ISA baseline code from optional feature implementations. Runtime dispatch may select an optimized implementation only after feature detection and OS state enablement (e.g. XSAVE state management on x86). Every optimized algorithm has a tested baseline fallback. SIMD state is per-thread and context-switch safe; kernel SIMD use is opt-in and bounded.

## 5. Objects, capabilities, memory and SMP

- Handles are indices plus generation tags; authority is checked against kernel-owned capability records. User-visible integers/pointers are never treated as authority.
- Capability slots are bounds-checked and generation-checked; stale handles fail closed. Hardware pointer authentication, where available, is defense-in-depth and never replaces software bounds/rights checks.
- Kernel pointers are not exposed to userland. Shared-memory mappings are explicit capabilities with range, rights, cacheability and lifetime metadata.
- IPC uses bounded message descriptors and explicit ownership transfer. Zero-copy is allowed only for pinned, mapped buffers with validated lifetime and cache-coherency rules.
- Per-CPU state avoids global cache-line contention. Locks have a documented ordering; atomics specify memory ordering. TLB shootdowns and IPI acknowledgements have bounded retry/timeout paths.
- SMP bring-up and CPU hotplug/power transitions are serialized through explicit state machines. A CPU is not scheduled until its MMU, interrupt, timer and per-CPU state are verified.
- Cache maintenance is architecture-specific behind HAL operations; DMA buffers declare coherency and alignment requirements.

## 6. Services and driver isolation

Move non-essential facilities out of the kernel in dependency order:

1. Device registry and device-tree service (kernel retains immutable boot snapshot and secure resource handoff).
2. Object registry / namespace service.
3. VFS and filesystem servers (block/root access remains boot-critical until initramfs or recovery path exists).
4. Non-boot-critical drivers (input, audio, network, graphics).
5. Compositor/window services and remaining SRL components.
6. Allocator policy and general-purpose heaps; the kernel retains only minimal page/frame primitives and tightly bounded metadata allocation.

A driver sandbox receives only explicit MMIO, IRQ, DMA and IPC capabilities. IOMMU isolation is used when present; without it, DMA-capable drivers require a trusted broker or bounce-buffer policy. Driver crash/restart must not corrupt kernel-owned memory or leave hardware resources assigned ambiguously.

Linux/BSD driver compatibility is a separate adapter layer, not code linked into the kernel core. Each imported driver requires license review, API-surface mapping, DMA/locking audit and a device-specific test. Begin with a deliberately small driver subset.

## 7. Deterministic accounting and timing

Every kernel operation must declare:

- Maximum input size and maximum loop/iteration count.
- Maximum allocation count/size and failure behavior.
- Lock acquisition order and bounded contention policy.
- Maximum queue depth, retries and IPC payload.
- Worst-case path for success, timeout and hardware failure.
- Instrumentation points and counters.

Cycle budgets are platform-specific. A universal fixed cycle bound across arbitrary CPUs, caches, DRAM and firmware is not claimed. For operations involving external hardware or shared memory, specify both a software execution bound and a timeout/upper-bound policy for the external response. WCET claims require a named CPU/board/firmware/compiler configuration and evidence from static analysis plus measurement under stated cache/interrupt assumptions.

Timer services use hardware counters and calibrated frequencies, never random sleeps as synchronization. Power/voltage control is mediated by a platform power service with safe defaults, validated firmware tables and bounded command completion. Frequency changes invalidate or update calibration data atomically.

## 8. Diagnostics, verification and safety

- Structured boot and runtime logs with stable subsystem/event IDs.
- Per-CPU trace buffers with bounded writes and explicit overflow counters.
- Panic/fault records include CPU, privilege level, address-space identity, fault address, register frame and symbolization metadata.
- Assertions guard invariants; errors propagate rather than being silently discarded.
- Static analysis, sanitizers where host-testable, fuzzing of parsers/ABI boundaries, model/property tests for capability and scheduler state machines.
- NASA/JPL Power of Ten-inspired rules: bounded loops, no recursion in critical core paths, bounded memory, explicit return/error handling, small functions, assertions, compile-time layout checks and strict warnings.
- Build artifacts include compiler/toolchain identity, source revision, dependency gitlinks and configuration manifest.

## 9. Migration gates

No destructive kernel replacement until all gates pass:

- G0: reproducible clean-clone bootstrap on macOS Intel, macOS Apple Silicon and Linux.
- G1: unchanged userland sources build for both target ISAs; record all remaining porting/ABI blockers.
- G2: archive the old kernel and verify SHA-256, extracted file manifest and restore test.
- G3: new kernel boots in QEMU with serial diagnostics and baseline memory/CPU/interrupt/timer tests.
- G4: capability, IPC, address-space, SMP and fault-isolation tests pass.
- G5: service migration begins one subsystem at a time, preserving a bootable fallback.
- G6: original userland runs unchanged against the compatibility ABI, with adapter changes confined to SDK/runtime boundaries.

The old kernel remains available until G6 is demonstrated on both architectures. Kernel removal is a separate, explicitly reviewed operation after backup verification.

## 10. Immediate implementation sequence

1. **Complete on current checkout:** strict, idempotent recursive dependency
   initialization and Coreutils metadata repair. Empty-clone validation remains.
2. **Partially complete:** cross-toolchain lock, exact-version checks and GNU
   SHA-512 source locks. Linux and Apple Silicon hosts still need execution tests.
3. **Complete on macOS Intel:** clean x86-64 and AArch64 builds, userland
   compilation and QEMU boot smoke tests. Dedicated ABI conformance tests remain.
4. **Complete:** syscall, object, ELF, process and userland ABI inventory.
5. **In progress:** module boundaries and hardware/driver contracts documented;
   source trees have not yet been moved or split.
6. **Complete locally:** original kernel archived and restore-verified. A second
   independent backup copy is required before removal.
7. **Next:** implement the minimal new core in a parallel tree; do not overwrite
   or delete the legacy kernel during bring-up.

## 11. Known dependency metadata repair

The pinned Coreutils fork contains a nested `gnulib` gitlink at path
`gnulib`, while its nested `.gitmodules` still records the old monorepo path
`user/sys/lib/gnulib`. This makes Git's recursive submodule update fail on a
fresh clone. `tools/setup-dependencies.sh` applies a narrowly scoped,
idempotent metadata-only correction to that path before recursive checkout.
No Coreutils source code or pinned gitlink revision is changed. The corrected
nested `.gitmodules` remains a local metadata modification inside the
Coreutils submodule and is intentional until the upstream fork fixes it.


## 12. Graphics service boundary — 2026-11-03

The graphics migration is specified in
`docs/architecture/GRAPHICS-USERLAND-MIGRATION.md`: compositord and all
rendering/API libraries are userland; kernel retains only surface/buffer object
management, capability enforcement, safe mappings, lifetime/synchronization
and presentation control. GPU/display device services run in userland where
possible with explicit MMIO/IRQ/DMA capabilities. The kernel keeps a minimal
panic/serial display path.

This refines migration step 6 in §6 ("Compositor/window services") and
supersedes older DIR-02/DIR-07 wording that placed the compositor in the HAL.
The current legacy kernel is not to be mass-moved in place; implement the
surface manager and services in the parallel NG tree, preserving the old boot
path until G0–G6 pass.
