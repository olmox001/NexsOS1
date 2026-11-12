# nxec — System Architecture v2

**ID:** ARCH-NX-002  
**Version:** 0.2.0-DESIGN — 2026-11-05  
**Status:** DESIGN; this document does not imply implementation or verification.

## 1. Objective

nxec consists of independent projects: preboot, kernel image, privileged kernel runtime, packaged ELF system modules, separate userland image and integration tools. Every subsystem has its own build, ABI, tests, manifest, version and explicit dependencies. The root project orchestrates and verifies these projects; it does not merge them into a monolith.

The privileged kernel Core combines the ISA/platform HAL, lightweight hypervisor control, microkernel primitives, registry, scheduler, memory management, capability enforcement, IPC and ELF loader/runtime. The term hypervisor describes the control and isolation architecture. VMX/SVM/EL2 are conditional hardware backends and are never assumed available. Without hardware virtualization, the kernel uses the supported MMU and privilege mechanisms and must not claim hardware virtualization.

The kernel image is broader than the privileged Core: it packages independently built ELF system modules under kernel/modules/. Device drivers and system services are user-mode sandboxed components. They are included in the kernel image for distribution, boot ordering, versioning and lifecycle management—not because they execute in MACHINE/ring zero. The separate userland/ image contains the wider user environment and applications.

## 2. Boot stages and ownership

1. Firmware/bootloader transfers control using its transport-specific contract.
2. The preboot ISA HAL normalizes the initial CPU state and gathers firmware and memory information.
3. Preboot common code validates input, produces the HCD, reserves bootstrap resources and prepares temporary allocator and mappings.
4. The image loader verifies the manifest and authenticity policy, decompresses the kernel image and validates ELF segments and permitted relocations.
5. The kernel entry receives an immutable Boot Handoff Descriptor (BHD), independently validates it and takes ownership.
6. The kernel image manager validates and registers the packaged system modules, then starts them in isolated user-mode sandboxes.
7. The kernel verifies and mounts the separate userland image through the registry and starts the initial SYSTEM services and user sandboxes.

The preboot-to-kernel transition transfers ownership. Before the jump, the handoff must explicitly describe CPU state, stack, page tables, mappings, HCD/BHD, image ranges, reservations, firmware lifetime, console, entropy provenance, modules and reclaimable intervals.

## 3. Physical and virtual address spaces

Preboot builds a canonical physical map from UEFI, E820/Multiboot, ACPI and/or DTB sources. An unlisted region is not automatically free RAM. Firmware, preboot, stack, HCD/BHD, kernel ELF, decompression workspace, page tables, packaged modules, userland image, framebuffer/MMIO and handoff buffers are reserved explicitly.

The initial virtual map covers only the code/data needed for the transition, HCD/BHD, kernel segments with W^X permissions, stack, page tables, essential MMIO and supported firmware-runtime mappings. The kernel reconstructs its own page tables and does not inherit undeclared mappings. Temporary mappings remain until adoption is verified, then are removed.

## 4. Preboot ISA HAL

Preboot has an ISA HAL distinct from the kernel HAL. The two implementations may follow the same documented hardware facts and binary protocols, but do not share privileged implementation code or C structures.

Interfaces include ISA and CPU-state identification, register discovery, bootstrap MMU preparation, cache maintenance and memory barriers, interrupt/exception state, timers, entropy provenance and kernel transition.

Entry adapters are distinct for AMD64 BIOS/Multiboot2, AMD64 UEFI x64 and ARM64 UEFI/AArch64. Each adapter declares the actual initial CPU state; the entry paths are not assumed equivalent.

## 5. Allocation and NASA Rule 3

NASA Rule 3 remains binding. The design distinguishes static reservations and preallocated pools, assignment of already reserved frames, activation of enumerated hardware capabilities, firmware allocation before the initialization boundary and actual dynamic allocation after that boundary. These categories are not interchangeable and must have explicit ownership, bounds and state transitions. Dynamic allocation after initialization is prohibited in the Rule 3 scope.

## 6. Kernel image and ELF modules

The kernel image contains the privileged kernel ELF, its authenticated manifest and the system ELF modules assigned to kernel/modules/. Each module is an independent artifact with a versioned ABI, ISA/K-class compatibility, requested capabilities, resource budgets, dependencies, signature and revocation policy.

A module's location inside the kernel image does not determine its privilege. Drivers and services execute in user-mode sandboxes unless a separately justified MACHINE-domain component is explicitly specified and approved.

REG is a communication protocol, not an executable-code interpreter. The module loader accepts executable artifacts only through the authenticated, validated and audited ELF-loading path.

## 7. REG and registry

REG is the common protocol for requests, events, notifications, capability transfer and shared buffers. The intended primary transport uses bounded shared-memory queues, endpoints and per-CPU/per-resource sharding. The canonical registry is logically centralized; hot paths use versioned local snapshots and shards.

The registry describes resources but does not grant rights. The Capability Engine is the authority. Every protected access revalidates generation, rights, scope and revocation state at the defined access/commit boundary.

## 8. Security domains

MACHINE, SYSTEM, ROOT, USER and SHARED are logical principals/domains, not synonyms for ISA privilege levels.

- MACHINE: privileged kernel Core and direct hardware control.
- SYSTEM: trusted system services running in user mode with narrowly scoped system capabilities.
- ROOT: administrative user-mode sandbox; not equivalent to MACHINE.
- USER: per-identity user-mode sandbox.
- SHARED: sandbox for explicitly shared resources.
- DRIVER: a module class that normally runs in user mode with device and service capabilities.

On AMD64, CPL, VMX root/non-root and ring terminology are not interchangeable. On ARM64, exception levels and security state are used; ARM is not described literally as x86 ring 0. The exact ISA mapping is specified separately.

## 9. VFS and userland

The complete VFS is a separate userland service. The kernel retains mount authority, namespace and capability enforcement; filesystem logic, path lookup, journaling and ordinary device drivers run in isolated user-mode modules.

The userland image is separate from the kernel image and is verified/mounted after kernel initialization. Each user has a private namespace and data mount, for example data/sdN; writes require the capability for that mount. ROOT, SYSTEM, USER and SHARED namespaces remain distinct. Symlink and mount crossing follow explicit policy.

## 10. Recovery and resident preboot

The proposed default is to reclaim preboot code after verified kernel adoption and preserve an immutable audit record containing versions, hashes and validation results. An error-prevention daemon runs as a SYSTEM user-mode service with diagnostic capabilities. Retaining preboot code does not grant that daemon preboot privileges. Any resident MACHINE monitor must be a separate, minimal kernel component with a restricted interface.

## 11. Evidence states

- DESIGN: specified.
- IMPLEMENTED: source code exists.
- TESTED: reproducible tests pass.
- VERIFIED: evidence supports the specific claimed property.
- QUALIFIED: the declared platform/configuration matrix is complete.

Boot image, REG wire protocol, security domains, userland image, build graph and GRUB licensing remain separate gates. No boot is VERIFIED before AMD64 and ARM64 firmware/QEMU paths are exercised.
