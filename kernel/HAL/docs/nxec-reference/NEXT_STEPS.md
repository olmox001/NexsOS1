# nxec — Next Steps and Verification Gates

**Status date:** 2026-11-05  
**Ordering:** dependency-driven. Do not skip a gate because a later artifact is easier to demonstrate.

## Gate 0 — Restore a reproducible development environment

1. Allow the active Homebrew update/upgrade process to finish; do not run a second Homebrew installer concurrently.
2. Run make setup to initialize submodules, install/update missing dependencies, build GRUB and generate the toolchain manifest.
3. Run make toolchain and verify every required host, cross, QEMU, GDB and GRUB command.
4. Verify the submodule commit, mirror/upstream remotes, .gitmodules, .gitignore and patch-series policy.
5. Record exact versions and hashes in the performance/toolchain log.

Exit criteria: clean setup on the current host, complete manifest and successful repeat check.

## Gate 1 — Restore the baseline test evidence

1. Run preboot host tests, cross-compilation, freestanding linking, sanitizers and static analysis.
2. Run kernel host tests, AMD64/ARM64 cross-compilation, freestanding linking, sanitizers and static analysis.
3. Compare preboot and kernel HCD wire layouts against docs/HCD_SPEC.md using independent compile-time layout checks.
4. Verify that the kernel build has no include path or source dependency on preboot/.
5. Record failures as problem-catalog entries and do not relabel a failing test as optional.

Exit criteria: both projects pass independently; wire compatibility is tested without sharing implementation code.

## Gate 2 — Complete the preboot ISA HAL and firmware ownership

1. Implement and test the entry adapters against the resolved memory-map ownership/provenance for AMD64 BIOS/Multiboot2, AMD64 UEFI/Multiboot2 and ARM64 UEFI; see DEC-UEFI-ENTRY-OWNERSHIP-001. GRUB source inspection resolves the design question, but does not count as firmware/QEMU validation.
2. Specify each entry CPU state, transport, firmware services lifetime and HCD BOOT_CONTEXT values.
3. Implement ISA-specific discovery adapters and normalize CPU, topology, cache, MMU, interrupt, timer, memory and firmware information.
4. Wire the host-tested UEFI retry helper and memory-map normalizer into the appropriate concrete firmware adapter; preserve bounded allocation and prohibit unrelated Boot Services after a failed exit attempt.
5. Complete memory-map normalization and the physical extent planner's real firmware input path.
6. Build initial page-table plans, permission maps and explicit ownership ranges. HCD major 2 extends (without changing) the v1 container and records with versioned MMU_TRANSLATION_CONFIGS, MMU_MAPPING_SIZES and ARM64-only MMU_ARM64_CONTEXTS sections; see HCD_SPEC.md §29. Both independent validators preserve common constraints and validate the ARM64-specific cross-references. The host-tested raw-snapshot MMU producer must still be wired into concrete firmware entry paths. The extension describes supported configurations and mapping sizes, not the page-table plan itself. Define the virtual-address layout policy and its owner before generating plans; reject any active ISA state the versioned schema cannot represent rather than approximating it.
7. Test firmware input corruption, overlapping maps, memory exhaustion, unaccepted memory and all capacity limits.

Exit criteria: a complete HCD and memory plan can be generated on both architectures with auditable ownership and bounded failure paths.

**Current partial evidence:** the UEFI retry protocol helper, in-place descriptor-v1 normalizer, Multiboot2 BIOS memory-map converter and Multiboot2 EFI post-EBS map-tag extractor pass host/cross/sanitizer/static checks. GRUB 2.14 pinned source confirms the EBS owner for the AMD64 UEFI/Multiboot2 and ARM64 UEFI chainload paths. HCD v2 sections are defined additively in §29; builder/parser, independent validators and v1/v2 semantic tests pass preboot and kernel host suites. Host tests now cover AMD64 four/five-level producer paths and ARM64 TTBR contexts, PC-selected CURRENT, granule sizes and strict common flags. Cross-link, sanitizer and static checks for the latest producer changes are pending. No concrete firmware entry consumes the producer and no QEMU boot validation exists. Gate 2 is not complete.

## Gate 3 — Define and implement the kernel-image contract

1. Complete the NXB/container comparison and record the decision.
2. Freeze the signed manifest schema, component table, digest/signature algorithms and anti-rollback policy only after security review.
3. Select compression by measured decoder workspace, maximum output, throughput, latency and failure behavior on AMD64 and ARM64.
4. Implement bounded decompression into pre-reserved memory.
5. Implement ELF validation and a strict relocation whitelist; reject unsupported dynamic features.
6. Define BHD fields, ownership transfer, CPU state, page tables, stack, HCD, image ranges and preboot reclaim ranges.
7. Add malformed/truncated/overlapping image tests, signature failures and resource-exhaustion tests.

Exit criteria: the preboot loader can authenticate, decompress and load the kernel image without unbounded allocation or unvalidated executable bytes.

## Gate 4 — Kernel entry and initial MACHINE Core

1. Implement AMD64 and ARM64 kernel entry adapters independently from preboot HAL.
2. Independently validate HCD/BHD and verify all physical/virtual ranges before dereferencing them.
3. Establish kernel-owned page tables with W^X, guard pages, NX stacks and explicit MMIO attributes.
4. Adopt physical memory and prove preboot reclaim conditions before freeing any range.
5. Implement minimal exception, interrupt and per-CPU initialization.
6. Add a deterministic panic/recovery path that never attempts unsafe continuation after integrity failure.

Exit criteria: both architectures reach a minimal, isolated kernel Core under QEMU with complete serial logs and no reliance on preboot structures.

## Gate 5 — REG, registry and capability security

1. Freeze REG wire framing, versioning, request identity/generation, operation classes, completion and cancellation semantics.
2. Specify shared-memory ring ownership, atomics, cache coherency, DMA/IOMMU boundaries, backpressure and fairness.
3. Prove request bounds and reject malformed envelopes before indexing arrays or dereferencing capabilities.
4. Implement canonical registry plus sharded/local snapshots without turning the registry into an authority.
5. Implement capability generation, rights, scope, delegation, revocation linearization and in-flight commit checks.
6. Test races among cancellation, completion, revocation, CPU offline, endpoint destruction and memory reclamation.
7. Record measured maxima separately from analytically proven timing bounds.

Exit criteria: untrusted requests cannot escape capability-bounded resources; concurrency/lifetime invariants have repeatable tests and reviewable proofs.

## Gate 6 — ELF system modules and user-mode driver model

1. Define the system-module manifest, ABI, signatures, dependency graph and K-class declarations.
2. Implement module loading from the authenticated kernel image only.
3. Create a separate address space, bounded stack and capability set for each module.
4. Keep drivers and services in user-mode sandboxes; device access occurs through REG and explicit device capabilities.
5. Implement module fault isolation, restart policy, revocation and resource accounting.
6. Add module-specific build/test projects under kernel/modules/.

Exit criteria: a faulting driver/service module cannot corrupt the privileged Core or access another module's memory.

## Gate 7 — Separate userland image and VFS

1. Compare cpio newc, tar/pax and SquashFS; evaluate a custom format only if measurable requirements justify it.
2. Freeze a read-only base image and separate writable user-data volumes.
3. Implement VFS and filesystem services in user mode.
4. Implement ROOT, SYSTEM, USER and SHARED namespaces with per-user data/sdN mounts.
5. Verify symlink, hardlink, rename, mount crossing, dynamic linking and compatibility-layer isolation.
6. Add libc, runtime, dynamic linker, system services and user applications as independent packages.

Exit criteria: userland can boot as a second image and cannot write outside explicitly granted mounts.

## Gate 8 — QEMU matrix, hardware qualification and release integrity

1. Build a bootable GRUB image for AMD64 and ARM64.
2. Test BIOS/UEFI paths, SMP, topology, memory pressure, CPU feature masks, page sizes, interrupt controllers and virtualization feature availability.
3. Test QEMU TCG and hardware acceleration separately; never conflate them with physical-hardware evidence.
4. Add fault injection, malformed protocol fuzzing and memory/capability exhaustion.
5. Reproduce builds from a clean checkout using only the documented setup.
6. Audit GPL and third-party licenses, preserve corresponding source and record provenance.
7. Review all normative documents against code, tests, build graph and decisions before the first baseline commit.

Exit criteria: every claimed property has an evidence class, source revision, toolchain record and reproducible test result.
