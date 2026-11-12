# NXOLMO integration into NX-HAL

## Purpose

NXOLMO is a reference and source of stronger contracts, tests and algorithms. It
is not copied wholesale into NX-HAL. Every source is classified against the
existing NexsOS ownership boundary and replaces an inferior path only after its
callers and tests are migrated.

## Ownership map

| NXOLMO area | NexsOS destination | Integration rule |
| --- | --- | --- |
| hardware/resource | `kernel/HAL/include/hal`, `kernel/HAL/core` | Canonical architecture-neutral physical resource, range and DMA validation. |
| authority/capability | `kernel/core/object.c` and shared object ABI | Kernel remains the sole issuer/enforcer. HAL descriptors never grant authority. |
| hardware grant | kernel authority -> HAL internal grant | A grant is derived by trusted kernel policy and revalidated at each side-effect boundary; never accepted from userland as a bearer credential. |
| HCD / discovery | existing NX-HAL common/capability plane | Descriptive only. HCD/DTB/ACPI/PCI discovery cannot grant rights. |
| virtual memory | NX-CORE policy + HAL page-table provider | Adapt bounds/protection semantics; do not duplicate VMM policy inside providers. |
| runtime/operation accounting | NX-HAL public/material operations | Stable IDs, deterministic work bounds, checked overflow and truthful UNKNOWN telemetry. |
| registry/NXREG | userland service plus minimal kernel authority index | Do not move the canonical registry model into HAL. |
| VFS/ext4/9P | userland services | Not HAL policy. Reuse algorithms/tests only at their owning service boundary. |
| block/device services | supervised userland drivers plus HAL transport grants | Kernel/HAL exposes capabilities/resources; service owns device policy/protocol state. |

## Hardware merge source set

Primary NXOLMO inputs:

- `include/nxolmo/hardware/capability.h`
- `src/hardware/capability.c`
- `tests/conformance/hardware/abi_layout.c`
- `tests/fuzz/hardware/fuzz_capability_standalone.c`
- `docs/primary/architecture/HARDWARE_CAPABILITY_H0.md`
- `docs/primary/assurance/POWER_OF_TEN.md`
- `docs/primary/OPERATION_BUDGETS.md`

Independent Linux comparison points:

- `include/linux/ioport.h`
- `kernel/resource.c`
- `include/linux/dma-mapping.h`
- `drivers/pci/pci.c`
- `arch/x86/mm/ioremap.c`
- `arch/arm64/mm/ioremap.c`

Linux is a behavioral/reference implementation, not a source-tree dependency.

## Power12 contract for NX-HAL

The JPL Power-of-Ten baseline plus NXOLMO Rules 11 and 12 apply to every newly
migrated HAL path and are progressively enforced over the pre-existing HAL.

1. no recursion or goto in trusted HAL C;
2. every loop has a format-, hardware-, resource- or quantum-derived bound;
3. no general heap allocation in runtime HAL paths after initialization;
4. production C functions are at most 50 physical source lines;
5. assertions do not validate untrusted inputs or authority;
6. declarations use the narrowest useful scope;
7. public boundaries validate parameters and every fallible result;
8. preprocessor use is limited to includes/simple compile-time definitions;
9. no untrusted pointer/wire-pointer authority; use offsets, spans and opaque identities;
10. strict warnings and static analysis are mandatory;
11. public/material operations use stable operation IDs and deterministic work
    bounds, report checked accounting and never fabricate hardware cycles;
12. finite limits are normative, hardware-derived, security-policy-derived,
    resource-budget-derived or scheduling quanta. Larger logical work continues
    via chunking/cursors/scatter-gather rather than private ceilings.

## First replacement sequence

1. Introduce the architecture-neutral resource/grant ABI and Power12 accounting.
2. Port NXOLMO resource/range/DMA validation and its ABI/mutation tests.
3. Extend the device publication protocol with resource identities instead of
   inferring transport from address magnitude.
4. Route MMIO/port/IRQ/DMA operations through authorized resource operations.
5. Migrate VirtIO/PCI providers and remove `kernel/hal_device.h` after the last caller.
6. Bind kernel object handles to resource grants; expose only attenuated handles
   to user-mode driver services.
7. Move device-class policy to userland services while retaining HAL execution
   primitives and kernel authority enforcement.
