# DEV-1 — Device discovery and lifecycle source audit

**Date:** 2026-11-03
**Status:** source audit complete for the legacy discovery/binding paths; no runtime-device-manager implementation started.
**Scope:** reconcile the accepted NEXS-NG device graph design with the current AMD64/AArch64 HAL, PCI, USB and GPU code before changing the next subsystem.

## 1. Architectural decisions preserved

The target is the normalized, source-neutral device graph described in
`docs/architecture/NEXS-NG-DEVICE-LIFECYCLE-AND-HOTPLUG.md`:

- FDT, ACPI, PCI, USB and firmware are discovery sources, not authority grants.
- `devmgrd` owns graph, matching, policy, service lifecycle and event publication in userland.
- Kernel NG owns the resource/capability primitives: validated MMIO, IRQ, DMA/IOMMU, pinning, revocation and synchronization.
- Device identity is not a capability. Every device record carries source, confidence, generation and lifecycle state.
- Preserve the legacy driver-maturity decision: dynamic register/unregister, refcounts, remove callbacks, matching and hotplug events are required in the legacy driver programme; do not pretend the current static registry already implements them.
- Do not replace or simplify the existing project layout, and do not transplant the legacy global registry into NG.

This audit does not change those decisions and does not treat the legacy implementation as the NG architecture.

## 2. Current FDT discovery boundary

The BOOT-1 parser now validates a complete blob and collects memory regions,
reservation-map entries, static `/reserved-memory` ranges, the DTB allocation
and enabled CPU count. Its public API in
`kernel/include/kernel/fdt_parser.h` exposes only:

- `fdt_parser_validate()`
- `fdt_parser_collect_regions()`
- `fdt_parser_count_cpus()`

There is no generic node iterator or device-resource catalogue yet. In
particular, `compatible`, `status`, `reg` outside memory/reserved-memory,
`ranges`, `interrupts`, `interrupt-parent`, `phandle`, clocks, resets and
power domains are not exported as a validated device graph.

The AArch64 platform HAL therefore still contains QEMU-virt constants:
`VIRT_ECAM_BASE=0x4010000000`, `VIRT_ECAM_BUSES=16`, PCI MMIO windows
`0x10000000/0x2eff0000` and `0x8000000000/0x8000000000`; VirtIO-MMIO
discovery iterates `VIRTIO_COUNT` slots from `VIRTIO_MMIO_BASE` with
`VIRTIO_MMIO_STRIDE`, and assigns IRQs as `48 + slot`. The source comments
refer to the board DTB, but these values are not currently obtained through a
generic FDT device-node API.

**Consequence:** BOOT-1 closes the bounded memory/CPU/reservation parser gate;
it does not close platform-device discovery. The next FDT work must parse
nodes and translate bus resources without turning untrusted properties into
capabilities.

## 3. Legacy HAL registry and driver binding

The legacy registry in `kernel/core/hal_bus.c` is intentionally boot-only:

- fixed `MAX_HAL_DEVICES=64` flat array;
- append-only, no unregister/remove path and no generation/refcount;
- no lock, with comments requiring registration before SMP;
- overflow is logged and dropped, but `hal_register_device()` returns no
  status to the caller.

There are two parallel descriptor shapes. `kernel/include/kernel/hal_device.h`
defines `hal_device_t` plus `hal_resource`; the registry actually stores
`struct hal_device` from `kernel/include/kernel/hal.h`. The registry record
contains one `base` and one `irq`, not a typed resource collection, parent,
source, confidence, state or generation. Do not confuse the legacy register
access wrapper with the normalized NG device record.

Driver matching is also a boot-time legacy seam:

- `driver_register()` prepends to a static linked list;
- `driver_match_all()` probes unbound entries;
- the only current call site is `usb_init()`, after xHCI/EHCI/UHCI drivers
  register themselves;
- there is no general device-arrival notification or remove callback.

The comment in `driver.h` says registration may occur before or after scan,
but that is only safe if a caller explicitly invokes `driver_match_all()`
again. It does not provide automatic hotplug.

## 4. PCI findings

The two architectures currently use different PCI paths:

### AArch64

`kernel/arch/aarch64/hal.c` scans VirtIO-MMIO slots and then configures a
hardcoded ECAM window before calling `pci_scan_and_register()`. It supplies
hardcoded PCI MMIO allocation windows rather than deriving the ECAM node,
bus range, `ranges` translation and interrupt routing from the validated DTB.

### AMD64

`kernel/arch/amd64/hal.c` uses `pci_enumerate()` and its architecture callback.
That callback enables IO, memory decode and BusMaster before a driver has
claimed the device. The modern VirtIO path composes a 64-bit BAR4/BAR5 address,
but maps a fixed 64 KiB window rather than a discovered BAR extent.

### Shared PCI implementation

`kernel/drivers/pci/pci.c` has a second registration path used by AArch64.
It also enables IO|MEM|BusMaster (`cmd | 0x7`) for every discovered function
before driver ownership and DMA setup. Its `pci_get_bar_size()` probes only
one 32-bit BAR word; it does not size a paired 64-bit BAR. The registration
path maps a fixed 128 KiB extent. The MMIO bump allocator does not yet provide
complete overflow, overlap, bridge-window or concurrent-allocation handling.

**Required safety boundary:** discovery may read configuration space. BAR
sizing/assignment, decode enable, bus mastering and DMA capability creation
must be separate transitions. Bus mastering must not be enabled until the
selected driver owns the device and DMA mappings/IOMMU policy are ready.

## 5. USB and GPU lifetime findings

USB root-port enumeration is explicitly one-shot in
`kernel/drivers/usb/usb_core.c`; the source labels runtime rescan/hotplug as
Fase 2. `usb_hcd_ops` has enumeration, control and interrupt endpoint
operations, but no disconnect, endpoint-cancel, controller-remove or
quiesce/drain callback. The device table is a fixed array of 16 entries.
Current support must not be described as safe disconnect/reconnect while
transfers are in flight.

The GPU registry in `kernel/drivers/gpu/gpu_core.c` has a linked list and a
primary pointer. `gpu_unregister()` unlinks but does not wait for users or
in-flight commits; `gpu_get_primary()` returns a raw pointer without a
lifetime token. It cannot be used as the lifetime foundation for display
hot-unplug.

## 6. Implementation order derived from the accepted plan

1. **DEV-1 — FDT device catalogue:** add bounded generic node/property
   enumeration to the parser, preserving path, compatible list, status,
   phandle and raw resource tuples with explicit parent-bus context. Translate
   `reg` only through validated `ranges`; decode interrupts only after
   resolving the interrupt parent and its cell format. Export metadata only;
   do not create MMIO/IRQ/DMA capabilities in the parser.
2. Add host tests for valid QEMU-virt nodes, missing/disabled status,
   compatible string lists, nested buses, 32/64-bit cells, `ranges`
   translation, interrupt parents, phandle references, truncation, overflow,
   invalid tuple lengths and recursion/property limits.
3. Compare the parsed AArch64 ECAM, PCI MMIO windows, VirtIO-MMIO slots and
   GIC interrupt properties against the current constants. Keep constants as
   a documented compatibility fallback only until both paths are equivalent.
4. **DEV-2 — safe PCI resource lifecycle:** complete BAR sizing (including
   64-bit pairs), validate windows and overlaps, separate enumeration from
   decode/bus-master enable, and add explicit ownership/rollback.
5. **DIAG-1 — device lifecycle observability:** bounded read-only snapshots
   and event counters, including discovery source, generation, transition
   reason, failed probe and resource validation status.
6. USB disconnect/reconnect, GPU lifetime and full userland devmgr service
   remain separate gates; do not fold them into the parser change.

## 7. Verification boundary and next gate

BOOT-1 remains staged and its dual-architecture evidence is in
`docs/report/BOOT-1-FDT-HARDENING-2026-11-03.md` plus
`docs/report/logs/`. This audit did not modify runtime source and did not
claim DEV-1 implementation or hotplug support.

Before DEV-1 source edits, verify the exact parser token-walk representation,
property lookup semantics, cell-width validation and test harness once more
against the current staged code. Keep every parser test host-runnable under
ASan/UBSan, then rebuild and boot AMD64 and AArch64 after integration.
