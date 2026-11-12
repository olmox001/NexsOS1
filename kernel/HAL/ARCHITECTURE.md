# NX-HAL boundary reconstruction and migration plan

## Non-negotiable dependency rule

The target graph is one-way:

NX-CORE -> NX-HAL public protocol -> selected HAL provider

NX-HAL must not include NX-CORE private headers and must not call kernel
implementation symbols directly. Events travelling from hardware toward the
kernel cross a versioned inbound callback protocol. Requests travelling from
the kernel toward hardware cross the outbound primitive API. Architecture
selection is a HAL build concern only.

## Existing architecture reconstructed from source

The legacy architecture layer contains these provider classes:

1. boot/handoff and linker layout;
2. CPU bring-up, SMP wake, context switch and trap/syscall entry;
3. interrupt masking, barriers, idle/yield/reboot and hardware entropy;
4. page-table operations, address-space roots, TLB shootdown and cache
   maintenance;
5. platform discovery, CPU/memory inventory and PCI/VirtIO discovery glue;
6. timer/IRQ/UART architecture providers;
7. user-memory access/fault fixup;
8. device register access and architecture-specific I/O spaces.

The generic kernel still contains architecture selections in memory-layout/PTE
constants, boot normalization, scheduler trace decoding, FDT/provider gates and
several tests. Those are migration debt, not accepted final design.

## Reverse dependencies that must become inbound protocols

Object-level audit of the pre-migration tree showed HAL objects importing kernel
scheduler/syscall, IRQ, fault reporting, PMM/VMM allocation, PCI enumeration,
device registration, logging/panic, per-CPU state and boot globals. These calls
must be replaced, not renamed.

The inbound protocol is split by responsibility:

- trap_sink: syscall, IRQ, synchronous fault and scheduler-return events;
- memory_sink: page allocator/free and bootstrap mapping requests needed while
  page-table providers are HAL-owned;
- device_sink: discovered-device publication and generic PCI/device binding;
- diagnostic_sink: bounded early logging and fatal handoff;
- smp_sink: secondary-online notification and architecture-neutral CPU state.

Each sink is registered explicitly during bootstrap. Before registration only
the documented early-boot subset may run.

## Outbound primitive families

Kernel-facing HAL language covers every operation currently represented by
arch_* and arch_impl_*: CPU control, IRQ state, barriers, context operations,
VMM/TLB/cache, timer clock/comparator, entropy, fault metadata, uaccess,
platform inventory, device I/O, reboot and boot descriptor normalization.

No final generic source may include arch/*, inspect native trap-frame fields or
use ARCH_AMD64/ARCH_AARCH64.

## Build migration

Phase H0 (implemented): physical ownership plus a single HAL source manifest and
standalone libnxhal.a build.

Phase H1: kernel link consumes libnxhal.a as the only architecture object input.
Generic compilation receives only HAL public includes; native provider include
paths are removed.

Phase H2: replace direct HAL->kernel imports with the inbound sink protocols.
The standalone HAL build must then succeed with no -I../include dependency.

Phase H3: move architecture-selected constants and native trap layouts behind
HAL public types/accessors; remove every architecture preprocessor branch from
generic kernel source.

Phase H4: enforce the boundary in CI: forbidden include/symbol scans, standalone
HAL build, standalone kernel build against each HAL, then boot/runtime parity on
amd64 and aarch64.

A phase is complete only when both architectures compile and boot and the
boundary audit has zero unapproved reverse edges.

## NXEC capability-plane integration

The HAL integrates the complete requested NXEC preboot common/HAL/test
implementation as a capability/description plane. It complements, rather than
replaces, the existing NexsOS execution providers. HCD is descriptive input and
never grants authority. Raw hardware discovery, normalized
SUPPORTED/ENABLED/USABLE state, MMU configurations, firmware-memory ownership
and bounded planners are verified independently from operations that mutate
hardware state.

The temporary HAL/nxec checkout is not a production dependency and is removed
only after source/test equivalence is demonstrated.
