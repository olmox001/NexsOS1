# NEXS-NG Hardware, Isolation and Determinism Specification

Status: research-backed architecture decisions; implementation is not yet started.  
Companion contracts:
- ABI freeze: docs/architecture/ABI-COMPATIBILITY-MATRIX.md
- Clean-room scope: docs/architecture/NEXS-NG-CLEANROOM.md
- Build baseline: docs/architecture/BUILD-BASELINE-2026-11.md

## 1. Architectural boundary: microkernel first, hypervisor optional

A microkernel and a hypervisor solve related but different problems. NEXS-NG
will be a native microkernel first. Hardware virtualization is an optional
execution facility, not a boot prerequisite and not the mechanism used for
ordinary driver isolation.

The trusted core contains only:
- exception/trap entry and return;
- thread/process control blocks and context switching;
- scheduler, per-CPU run queues and IPI coordination;
- address spaces, page tables, TLB shootdown and safe user-memory access;
- capability table, object references and bounded IPC;
- interrupt routing and minimal per-CPU clocksource/deadline timer;
- boot-time physical-frame allocator and essential MMIO mapping primitives.

Everything else is a service or a replaceable provider.

### Optional virtualization providers

| ISA | Hardware mode | Main mechanisms | Native fallback |
|---|---|---|---|
| Intel x86-64 | VMX root operation | VMXON/VMCS, EPT, VM exits, virtual APIC | Run NEXS directly without guest execution |
| AMD x86-64 | SVM host | EFER.SVME, VMCB, NPT, intercepts | Run NEXS directly without guest execution |
| AArch64 | EL2 | HCR_EL2, VTTBR_EL2, stage-2 tables, virtual timer/interrupt routing | Run at EL1 if firmware did not enter EL2 or EL2 is absent |

Feature detection must include architectural capability registers and firmware
control state. A CPU flag alone is not proof that virtualization can be enabled:
firmware may lock the relevant controls, or boot firmware may enter AArch64 at
EL1. In those cases NEXS must still boot normally; only guest execution is
unavailable.

Driver sandboxing uses address spaces, capabilities and IOMMU isolation. It does
not require VMX/SVM/EL2.

References:
- Intel SDM, including VMX and system programming:
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
- AMD64 Architecture Programmer's Manual, Volume 2:
  https://docs.amd.com/api/khub/documents/sD1_QL~h4Afq2_tvzxqqSQ/content
- Arm A-profile architecture reference:
  https://developer.arm.com/documentation/ddi0487/latest/

## 2. Hardware discovery: ACPI + Device Tree, normalized into one graph

Device Tree and ACPI are complementary firmware interfaces, not competing
universal replacements.

### 2.1 Device Tree / FDT

Use Devicetree Specification v0.4 as the FDT input contract.

The DTB is a bounded, pointerless binary blob containing a header, reservation
map, structure block and strings block. Firmware or the bootloader passes it to
the kernel at boot. Every offset, length, alignment, string terminator, token
and reservation range must be validated before access.

Important properties and bindings include:
- compatible, reg, ranges, interrupts, interrupt-parent;
- dma-ranges, iommus, memory-region and reserved-memory;
- clocks, resets, power-domains, regulators and cooling maps;
- cpus, cpu-map, cache properties and enable-method;
- chosen, initrd start/end and boot arguments.

The kernel performs only the minimal early parse needed to discover usable RAM,
reserved regions, boot CPU and interrupt/timer bootstrap. It then publishes an
immutable, validated snapshot to the userland Device Manager. Full matching,
resource policy, optional bindings and device lifecycle belong to devmgr.

Use the libfdt source files whose SPDX terms allow the BSD-2-Clause option.
The upstream source is dual-licensed; audit each imported file and do not
accidentally include GPL-only U-Boot wrappers or local modifications. Pin the
source revision and preserve the selected license notices.

### 2.2 ACPI

On PC-class x86-64 systems, ACPI is a primary discovery and power-management
interface. ACPI is also used on some AArch64 server platforms.

The first implementation must validate checksums, lengths, revisions, physical
addresses and table overlap before parsing. The table inventory includes:
- RSDP/XSDT and FADT for table discovery and fixed power/sleep interfaces;
- MADT for APIC/x2APIC or GIC interrupt topology and processor interfaces;
- MCFG for PCI Express ECAM configuration space;
- SRAT and SLIT for NUMA CPU/memory affinity and distance;
- PPTT for processor/cache topology;
- GTDT for architected timer descriptions;
- DMAR for Intel VT-d; IVRS for AMD-Vi;
- IORT for Arm I/O topology and SMMU relationships;
- DSDT/SSDT AML for device methods, thermal zones and power resources.

The kernel parses only the tables needed for memory, CPU, interrupt, timer and
IOMMU bootstrap. AML interpretation is isolated in a userland ACPI service.
ACPICA's upstream license changed in 2026; current upstream source offers
BSD-3-Clause or GPL-2.0-only. Select the BSD-3-Clause option, pin a release or
commit, and verify SPDX headers at that exact revision. Do not use the deprecated
unix2 source archive. The service receives bounded access to firmware tables
and only explicitly granted I/O/MMIO operations.

### 2.3 Normalized Device Graph

devmgr publishes a hardware graph independent of the firmware source:

- node ID, parent bus and compatible/class identifiers;
- register ranges and resource ownership;
- IRQ lines/MSI vectors and interrupt-controller route;
- DMA mask, IOMMU unit/group/domain and reserved regions;
- clocks, reset controls, power domains and regulator providers;
- CPU/package/core/thread topology and cache sharing;
- discovered CPU ISA features, kept separate from firmware-provided device
  properties;
- diagnostic state, provenance and confidence of each discovered property.

A Device Tree or ACPI property is input data, not an instruction to trust or
execute. Unknown or malformed properties are retained as diagnostic metadata
or rejected; they never silently become MMIO permissions.

References:
- Devicetree Specification v0.4:
  https://devicetree.org/specifications/
- DTB binary format:
  https://devicetree-org.github.io/devicetree-specification/
- ACPI Specification 6.6:
  https://uefi.org/specs/ACPI/6.6/
- ACPICA licensing:
  https://www.intel.com/content/www/us/en/developer/articles/license/acpica-licensing.html

## 3. ISA baseline and optional CPU extensions

Device Tree is not the authority for instruction-set support. ISA capabilities
are read from each logical CPU and recorded in the normalized CPU feature
catalogue. Firmware tables describe topology and resources; CPUID and
architectural ID registers describe executable CPU features.

### 3.1 x86-64 feature detection

The baseline must not assume AVX. Detection and enablement are separate steps.

- CPUID leaf 1: SSE/AVX, XSAVE, OSXSAVE and related features.
- CR4.OSXSAVE must be enabled before XGETBV/XSAVE state management.
- CPUID leaf 0xD enumerates XSAVE-managed state components and sizes.
- AVX requires the CPU feature plus OS-enabled XCR0 x87/SSE/AVX state.
- AVX-512 requires the relevant CPUID feature and XCR0 opmask/ZMM state
  components; never dispatch AVX-512 from a CPUID bit alone.
- Later extensions (including AVX10 and vendor-specific features) are
  enumerated dynamically; do not hardcode a maximum ISA generation.

The scheduler must preserve all enabled extended state across context switches.
The state size is queried, aligned and bounded at boot. Kernel interrupt and
scheduler paths stay on the baseline ISA unless an explicit FPU/vector-state
protocol is active.

### 3.2 AArch64 feature detection

Read the architectural feature registers, including:
- ID_AA64PFR0_EL1 for FP, AdvSIMD and SVE;
- ID_AA64ISAR0/1/2/3_EL1 for atomics, crypto, dot product, pointer
  authentication and newer instruction features;
- ID_AA64ZFR0_EL1 for SVE extensions;
- ID_AA64SMFR0_EL1 for SME;
- ID_AA64MMFR* for translation and memory-system features.

FP/AdvSIMD, SVE and SME are not interchangeable. SVE/SME require explicit
per-thread state management, vector-length policy and context-switch support.
Use NEON/AdvSIMD as the normal vector baseline where the feature register says
it is available; SVE/SME variants are optional.

### 3.3 Heterogeneous CPU systems

A feature may exist on one core and not another. NEXS therefore maintains:
1. per-CPU feature records;
2. a system-safe feature intersection for migratable tasks;
3. optional affinity-bound execution classes for tasks that request a stronger
   feature set and are pinned to compatible CPUs.

A task must never migrate to a CPU that cannot restore or execute its required
architectural state. Hotplug and power-state transitions revalidate the
feature class before the CPU rejoins a run queue.

### 3.4 Algorithm variants

Every accelerated algorithm has:
- a baseline implementation using only the documented ISA baseline;
- optional separately compiled variants (SSE/AVX2/AVX-512, NEON, SVE, SME,
  crypto, atomics, etc.);
- a feature predicate and required OS-state predicate;
- a correctness test comparing output against the baseline;
- a benchmark and maximum-work bound for each variant;
- a fallback when the feature is absent or the state cannot be enabled.

The build must not compile the whole kernel with the build host's ISA. Each
variant is compiled explicitly and selected at runtime from the target CPU's
verified feature set.

References:
- Intel SDM:
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
- Intel AVX-512 OS-state requirements:
  https://cdrdv2-public.intel.com/671436/253665-sdm-vol-1.pdf
- Arm CPU feature register catalogue:
  https://docs.kernel.org/arch/arm64/cpu-feature-registers.html
- Arm GNU Toolchain feature support:
  https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain

## 4. Paging, cache and address-space design

### x86-64
- 4 KiB base pages; 2 MiB and 1 GiB huge pages when CPUID and alignment permit.
- Select 4-level or 5-level paging only after checking LA57 support and boot
  environment compatibility.
- Use PCID when supported; use per-address-space ASIDs/tags on architectures
  that provide them.
- Use per-CPU TLB shootdown queues with sequence numbers and acknowledgements.
- Never assume physical RAM is contiguous or that virtual equals physical.

### AArch64
- VMSAv8-64 supports 4 KiB, 16 KiB and 64 KiB translation granules; actual
  support is implementation-defined and reported by ID_AA64MMFR0_EL1.
- TCR_EL1 has independent TG0 and TG1 granule fields. For ABI v1 compatibility,
  use a 4 KiB user granule where supported; the kernel mapping may select a
  different supported granule after measuring the trade-offs.
- If 4 KiB user translation is unavailable, the loader must use an explicit
  compatibility mapping strategy and pass all W^X/segment-overlap tests; do
  not silently round protections in a way that creates writable-executable
  pages.
- Use ASIDs and broadcast TLB invalidation where supported.

### Common rules
- Discover cache topology and line size per CPU; align per-CPU structures and
  ring descriptors to avoid false sharing.
- Use per-CPU allocation caches and NUMA-local page pools where topology exists.
- Separate user virtual, kernel virtual, physical and device DMA address types.
- Enforce W^X, user/kernel privilege separation, guard pages and zero-on-free
  policy for sensitive objects.
- Keep the legacy user virtual address ABI from the compatibility matrix.

References:
- Arm memory management and granules:
  https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/LearnTheArchitecture-MemoryManagement-101811_0100_00_en.pdf
- Arm Linux page-size trade-offs:
  https://developer.arm.com/community/arm-community-blogs/b/operating-systems-blog/posts/linux-kernel-configuration-for-arm-servers

## 5. SMP, timers and cycle accounting

### SMP

- x86 CPU discovery and interrupt routing use ACPI MADT and APIC/x2APIC;
  secondary CPU startup uses the platform's supported INIT/SIPI path.
- AArch64 CPU topology comes from ACPI MADT/SRAT or FDT cpu nodes; secondary
  CPU startup uses PSCI CPU_ON when available, otherwise a validated platform
  enable-method.
- Every CPU has a private stack, scheduler state, interrupt nesting state,
  timer state and allocator cache.
- Lock hierarchy, ownership and maximum hold sections are documented. No
  unbounded global spinlock is allowed on a hot path.
- TLB shootdowns, reschedule IPIs and device interrupts have separate bounded
  queues and counters.

### Timer/clocksource

- AArch64 uses the architected Generic Timer where available. CNTFRQ_EL0 is
  a firmware-programmed declaration of the system-counter frequency, not a
  hardware self-measurement. Verify it is nonzero and consistent across CPUs;
  calibrate/cross-check it against another trusted source where available.
  CNTVCT_EL0/CNTPCT_EL0 are time counters, not CPU core cycle counters.
  Per-CPU compare registers generate deadlines. A DT clock-frequency override
  is a last-resort workaround for known broken firmware, not the normal source.
- x86 prefers a verified invariant TSC as a clocksource, with APIC TSC-deadline
  mode when available. Check CPUID.80000007H:EDX[8]; use CPUID.15H/16H frequency
  information only when the enumerated fields are valid. HPET/ACPI PM timer or
  another verified platform clocksource is the fallback.
- Calibration checks counter stability and synchronization across CPUs and
  power-state transitions. Unknown or inconsistent sources are rejected or
  downgraded.
- The scheduler uses absolute deadlines and a bounded timer wheel/heap design;
  no random timer intervals and no busy-wait delay loops in normal operation.

### Distinguish time from cycles

Do not label CNTVCT ticks or invariant TSC ticks as actual core cycles.

- Monotonic time: architected timer/TSC clocksource with a known frequency.
- Core cycles: PMU/performance-monitor counter when the platform exposes a
  usable, correctly configured event.
- Wall time: RTC/firmware time service, separate from monotonic time.
- Voltage/frequency telemetry: only from a verified ACPI/CPPC/PMU/PMIC provider.

Every sample records counter type, CPU ID, frequency source, calibration,
migration and power-state context.

### What can be bounded

For every kernel operation define:
- maximum input size and number of pages;
- maximum loop iterations and retry count;
- maximum capability transfers and message bytes;
- maximum queue depth and allocation attempts;
- maximum lock acquisition attempts/hold budget;
- maximum number of CPUs participating in an IPI/TLB operation;
- timeout, cancellation and failure return.

For hardware operations whose latency is not architecturally bounded (MMIO,
firmware calls, storage/network completion, cache/DRAM contention), use an
explicit deadline, cancellation path and safe fallback. Do not claim a universal
maximum number of CPU cycles across arbitrary hardware.

WCET records are per exact CPU/platform profile, compiler, build flags,
memory/cache configuration and power policy. Use static control-flow/loop
bounds where possible, plus measurement and margin on the target machine.
A measurement maximum is evidence for that profile, not a proof for all CPUs.

References:
- Arm Generic Timer guide:
  https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Generic%20Timer.pdf
- ACPI 6.6 timer, power and processor tables:
  https://uefi.org/specs/ACPI/6.6/

## 6. DMA and driver isolation

### Address model

A driver must distinguish:
- CPU virtual address;
- CPU physical address;
- device-visible DMA/IOVA address.

A DMA address is not assumed to equal a CPU physical or virtual address. The
DMA API model uses map/unmap operations and IOMMU translation where available.
On platforms without an IOMMU, a bounce-buffer path is required for devices
whose DMA cannot safely be restricted.

### IOMMU providers

- Intel VT-d: ACPI DMAR, DRHD units, device scopes and reserved regions.
- AMD-Vi: ACPI IVRS, IOMMU units and device relationships.
- Arm SMMU: ACPI IORT or Device Tree iommus bindings.

A driver process receives a dedicated IOMMU domain and only the IOVA ranges
needed for its buffers. The kernel pins pages, maps/unmaps IOVA, invalidates
IOTLBs and enforces ownership. If no safe isolation exists, the driver is
marked trusted or denied direct DMA; the kernel must not pretend it is sandboxed.

### Driver capability bundle

A driver service receives explicit capabilities for:
- MMIO register ranges and access widths;
- interrupt vectors/lines and EOI/affinity controls;
- PCI configuration and BAR resources;
- DMA buffer pools and IOMMU domain;
- device reset, clock, regulator and power-domain operations;
- shared-memory rings and event channels.

The driver cannot enumerate or access unrelated devices by ambient authority.
Driver crash/restart must not corrupt kernel state or other device domains.

### Compatibility layers

One small, documented Device Driver Interface (DDI) is the native contract.
Separate adapters may implement:
- BSD-style bus_space, bus_dma, Newbus attach/detach and bus_setup_intr;
- a deliberately limited Linux driver compatibility layer for common device,
  PCI, MMIO, DMA, IRQ, workqueue, completion, timer and wait-queue patterns.

Do not attempt to embed a whole Linux kernel or claim that arbitrary Linux
drivers will run unchanged. Linux driver code often depends on kernel-wide
services, locking, memory, workqueues, networking and internal APIs. Each
driver is ported/compiled individually against the compatibility subset and
runs as an isolated process. Import only code with a reviewed compatible
license and a clear dependency map.

References:
- Linux DMA mapping model:
  https://docs.kernel.org/core-api/dma-api-howto.html
- x86 IOMMU and ACPI DMAR/IVRS:
  https://www.kernel.org/doc/html/latest/arch/x86/iommu.html
- AMD IOMMU architecture:
  https://docs.amd.com/api/khub/documents/GD6kOXjzWsek8QUbn_qMvg/content
- Arm SMMU architecture:
  https://support.arm.com/documentation/ihi0062/dc/
- FreeBSD Architecture Handbook: Newbus and bus_dma:
  https://docs.freebsd.org/en/books/arch-handbook/book/

## 7. Capabilities, masked handles and hardware hardening

### Software capability contract

For the current x86-64/AArch64 target set, the portable security boundary is a
software capability system enforced by the MMU and kernel:

- Userland receives opaque integer handles, never kernel object pointers.
- Internally each handle resolves through a slot index, generation and rights
  metadata; stale generations fail closed.
- Capability derivation only attenuates rights. Transfer is explicit and
  receiver-authorized.
- Object references are atomic or protected by per-object/per-table
  synchronization; object destruction waits for in-flight references.
- Namespaces are per-process and capabilities are the authority to resolve
  or operate on a name.
- Internal pointers may be encoded/masked as defense-in-depth, but pointer
  obfuscation is not the security boundary. Bounds, generation and rights
  checks are.

### Optional hardware hardening

Enable only after feature detection and correct context/state management:
- x86 SMEP/SMAP, PKU, CET shadow stack/IBT, NX, PCID and IOMMU.
- AArch64 PAN, BTI, Pointer Authentication (PAC), Memory Tagging Extension
  (MTE), stage-2 translation and SMMU.
- Cache and memory tagging features are diagnostics/hardening, not substitutes
  for object capability checks.

True hardware capability pointers are a separate CHERI architecture extension.
They are not present on ordinary x86-64 or commodity AArch64 processors.
Arm Morello is a CHERI prototype; CHERI-RISC-V has FPGA and emerging platform
implementations. NEXS must not depend on CHERI to boot or provide its capability
model. A future CHERI/Morello/RISC-V port can replace the software capability
backend with hardware-enforced capabilities.

References:
- CHERI capability model and available instantiations:
  https://ctsrd-cheri.github.io/cheri-specification/
  https://ctsrd-cheri.github.io/cheri-faq/questions/general.html
- Arm PAC/BTI/MTE:
  https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Providing%20protection%20for%20complex%20software.pdf
- Intel SDM protection features:
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

## 8. Power, voltage and thermal control

Power management is a service policy backed by platform-specific providers.

- x86: ACPI FADT, _PSS/_PPC or CPPC, thermal zones, idle states and platform
  power resources.
- AArch64: PSCI CPU_ON/CPU_OFF/CPU_SUSPEND, Generic Timer wakeup, FDT clocks,
  regulators and power-domains, or ACPI equivalents on server platforms.
- PMIC, voltage rail and thermal sensor access is device-specific and must be
  granted through a driver capability.
- There is no universal safe voltage-control register/API across x86-64 and
  AArch64. If firmware or a device provider does not expose a validated control,
  powerd reports it as unavailable and leaves firmware policy in place.

The kernel retains only the minimal CPU idle/suspend/interrupt wake primitives.
Powerd owns policy, telemetry, thermal limits and user-facing configuration.

## 9. Zero-copy buffer model

Zero-copy is a controlled shared-memory protocol, not the passing of raw
pointers between mutually untrusted processes.

- Create a shared-memory object with explicit owner, size, cache attributes
  and access rights.
- Pin backing pages while a device or service owns a mapping.
- Pass a capability plus bounded offset/length descriptors.
- Use fixed-size per-CPU descriptor arrays/rings, aligned to cache lines.
- Define producer/consumer ownership, memory barriers and sequence numbers.
- For DMA, map the buffer into the device's IOMMU domain and unmap after
  completion; do explicit cache maintenance for non-coherent devices.
- A timeout or process death revokes mappings, drains/cancels in-flight work,
  then releases page references. Never recycle a buffer while a device can
  still DMA into it.

Small control messages stay inline in bounded IPC payloads. Large file, graphics,
audio and network transfers use shared-memory rings.

## 10. Implementation order

1. Freeze the v1 ABI and add cross-compiler layout tests.
2. Define normalized CPU/device/interrupt/timer/DMA contracts.
3. Boot a minimal x86-64 and AArch64 microkernel with native fallback and no
   virtualization requirement.
4. Implement address spaces, W^X, per-CPU scheduler, timer, IPC and capability
   tables with bounded allocation.
5. Implement optional VMX/SVM/EL2 provider behind the hypervisor interface.
6. Start the boot supervisor and migrate VFS/registry/compositor/font/input
   behind compatibility RPC, keeping legacy syscalls unchanged.
7. Add devmgr, IOMMU domains and the first sandboxed non-boot-critical driver.
8. Add BSD and Linux driver adapters one device family at a time.
9. Add per-CPU vector feature catalogue and baseline/optimized algorithm
   variants after context-state handling is tested.
10. Run ABI conformance, fault injection, SMP stress, DMA isolation, WCET
    profile and dual-architecture boot tests before considering legacy kernel
    removal.

No legacy kernel deletion is authorized by this document. The verified archive
and original source remain until every ABI and hardware gate passes.
