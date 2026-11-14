# Session 1 canonical architecture direction

This document records the current owner-approved direction after reconciling the
Session 1 directive with the existing ASTRA/NEXS-NG design history. It defines
ownership targets; it does not claim the current tree already satisfies them.

## 1. Kernel

The kernel owns security and hardware mechanisms, not system policy.

Kernel responsibilities include:

- scheduling, memory protection and generic process/thread mechanisms;
- object/capability enforcement and revocation;
- generic resource ownership and lifetime;
- validated surface/buffer objects and controlled presentation seams;
- hardware discovery primitives and kernel-side platform/resource mediation
  that cannot yet safely live in userland;
- IRQ, DMA/IOMMU, MMIO/PIO, pinning, synchronization and bounded wait
  mechanisms;
- minimal independent panic/emergency display output.

The target kernel does not own:

- compositor scene/window policy;
- themes, decorations, shadows, buttons or window effects;
- application pixel buffers;
- POSIX policy;
- VFS policy or filesystem implementation in the final architecture;
- hard-coded system registry sets;
- application privilege classes as an informal substitute for capabilities.

Legacy syscall, VFS, graphics and registry-policy code is migration debt and is
removed only when a tested object/service replacement exists.

## 2. HAL

The HAL abstracts ISA mechanisms and architecture-specific primitives. Shared
kernel code must not know whether it runs on AArch64 or AMD64.

The HAL is not the product device manager and must not encode QEMU as the only
platform. It must not own system UART policy. Serial/UART is a debug or device
transport exposed through the appropriate device/diagnostic mechanism.

Platform discovery is normalized from firmware/hardware descriptions (FDT,
ACPI, PCI, USB and future buses). Hardware descriptions do not grant authority;
validated resource capabilities do.

## 3. Device and driver model

The current flat boot-time registries are transitional. The target device model
separates:

- discovery and normalized device/resource description;
- resource validation and capability creation;
- driver matching/binding;
- driver lifecycle and generation;
- IRQ/DMA/MMIO ownership;
- hotplug detach, quiescence, cancellation and revocation.

No device receives BusMaster/DMA authority before a driver owns a validated DMA
contract. Device removal cannot free mappings/buffers until IRQ and DMA work is
quiesced and references are drained.

Ordinary device drivers migrate to isolated userland driver services through a
future DriverKit and driver sandbox. Kernel/platform drivers are retained only
where required by the security/boot boundary during the transition.

## 4. Graphics and display

Three states are distinct and must never be collapsed:

1. physical display/scanout device mode and supported hardware modes;
2. compositor/rendering workspace resolution and global scale policy;
3. each application's logical surface/window size.

Hardware modes are discovered from the actual device/provider. User-selected
rendering resolution remains a separate configurable state.

Applications own their backing buffers through SDK/resource primitives.
`nxcompositor` is the replaceable authoritative userland scene server and owns:

- window registry and scene metadata;
- z-order, focus and visibility;
- logical geometry and resize policy;
- composition, clipping, scaling and damage;
- themes, chrome, shadows, buttons and effects;
- cursor composition and presentation policy.

The kernel retains only capability-checked buffer/surface/display mechanisms and
the emergency display seam. `kernel/graphics` is removed only after every live
mechanism has a verified destination.

## 5. Userland library layers

The SDK is split conceptually into explicit ownership layers before directories
are rearranged:

### Userland primitive/sandbox layer

Consumes kernel object/capability primitives and exposes safe process-local
operations. Application sandbox and driver sandbox are distinct models.

### System libc

Consumes the safe primitive layer. It does not know portability adapters or
service extensions. POSIX-facing behavior belongs above the native kernel
object boundary rather than inside the kernel.

### Portability

Adapters and compatibility runtimes for external software. External source
trees are separated from NexsOS1 integration code. Planned compatibility
families include Linux, Windows and Android.

### Extensions

Reusable higher-level libraries built on libc or DriverKit as appropriate.

### UserKit

Public client APIs for supervised system services. Service-private code remains
inside the service project.

### DriverKit

Future public driver contracts plus driver-only capability-gated fast paths.
Each ordinary driver is intended to become a separate userland project. The
design must study Linux driver interfaces for reuse/compatibility without making
Linux source a build dependency.

## 6. IPC, registry and zero-copy

IPC is used where protection-domain messaging is required; it is not a default
substitute for shared buffers or direct capability-checked resources. MMIO is a
device-memory capability, not an IPC protocol.

The long-term object/registry model uses capability-authorized object requests,
bounded variable-size packets, shared/zero-copy buffers where ownership and
lifetime are provable, and explicit synchronization. Performance claims require
measurement.

Registry catalogue/policy sets are selected by the system integration project,
not hard-coded into kernel policy.

## 7. VFS, storage and system images

The target VFS/filesystem stack moves to userland with its drivers. The design
uses Linux VFS concepts together with Android/iOS/Linux-style sandbox and
multi-image ideas, while retaining native NexsOS object/capability semantics.

System images must support normal user mode and installer/demo mode. Current
`NexsOS1` and `demo1` boot manifests are evidence for the design, not
permission to duplicate policy.

NXOLMO is an approved project reference/source candidate for ext4, registry,
9P/VFS and reusable checked algorithms, subject to per-component integration and
tests.

## 8. Performance rule

No architecture shortcut is accepted only because it is simple. Conversely,
complexity is not accepted without evidence. Critical paths require measured
latency/work/cycle data, bounded queues, clear ownership and concurrency tests.
The goal is a general-purpose OS without unnecessary IPC, copies or clock-cycle
waste while preserving capability isolation and analyzability.
