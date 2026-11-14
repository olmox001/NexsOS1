# GPU / display / device safety census — Session 1

This is the live ownership and defect census for the GPU/display/device safety
phase. It records behavior observed from the current source and runtime. It is
not a claim that the target architecture is already implemented.

## 1. Current ownership map

### PCI / bus discovery

- `kernel/HAL/core/pci.c` provides the shared PCI config/BAR/enumeration
  implementation and publishes normalized devices.
- `kernel/HAL/arch/amd64/hal.c` still contains a separate AMD64 PCI scan and
  command-register enable path.
- AArch64 platform discovery currently includes QEMU-virt-specific ECAM/MMIO
  window assumptions; these are migration debt, not the target platform model.
- `kernel/core/hal_bus.c` stores the current boot-time device catalogue and
  performs driver matching.

### Driver manager

The current registry is boot-oriented and fixed-size. It has no complete
hot-remove/refcount/generation model. The Session 1 generic PCI safety block
adds only provisional driver claim and DMA authorization state; it does not
claim to implement the final lifecycle design.

### GPU

- `kernel/drivers/gpu/gpu_core.c` owns the current GPU registry and primary
  GPU pointer.
- `kernel/drivers/gpu/virtio_gpu.c` is the active VirtIO GPU provider.
- `kernel/resources/display.c` is the capability-checked presentation seam.
- `kernel/resources/buffer.c` already has a stronger reference/mapping
  lifetime model and is a useful internal design reference.
- `kernel/graphics/*` still owns legacy scene/window/compositor behavior and
  directly consumes the primary GPU pointer.

### Userland graphics

`nxcompositor` is currently a userland compositor over legacy kernel-owned
window/snapshot state. It is not yet the authoritative scene owner. That
migration is a later phase after the GPU/device substrate is safe.

## 2. Verified defects

### GPU lifetime

The GPU registry exposes a raw `primary_gpu` pointer. `gpu_get_primary()`
returns it without a reference or generation. Callers can retain it while
`gpu_unregister()` unlinks the device; there is no demonstrated quiesce,
in-flight drain or destroy protocol. This cannot be repaired locally while
`kernel/graphics` directly consumes the raw pointer.

### PCI DMA authority

The pre-Session-1 shared PCI path enabled command bits with `command | 7`
during enumeration, granting BusMaster/DMA before driver ownership.

The same unsafe policy still exists independently in
`kernel/HAL/arch/amd64/hal.c`. Therefore the first generic PCI block is only a
partial hardening step; AMD64 is not yet globally DMA-safe.

### Driver lifecycle

The device catalogue is fixed-size and boot-only. No complete remove callback,
reference drain, generation check or hotplug state machine exists.

### BAR / MMIO assumptions

xHCI and EHCI currently map fixed MMIO extents even though exact PCI BAR sizes
are available. These fixed map lengths must be replaced by validated resource
extents before claiming device-lifecycle safety.

### Platform assumptions

AArch64 currently embeds QEMU-virt platform constants in platform discovery.
The target model must derive resources from FDT/firmware/device descriptions and
support additional hardware without making QEMU the architecture contract.

### Input device reliability

AArch64 has a real intermittent input-delivery failure. During Session 1
verification, a complete `/sys/bin/nxwins` command reached the TTY while the
final Enter event did not. Sending one separate QMP Return immediately executed
the already-complete command. This proves an input-event loss/readiness problem
rather than a shell parser failure. It must receive a real end-to-end regression
test before being declared fixed.

The standard `nxadb` initial `SHELL-INTERACTION` probe also fails
intermittently on both AArch64 and AMD64 even when later commands execute
correctly. The probe/readiness logic and the guest input path must be separated
and tested rather than papered over with retries.

### Runner lifecycle

Stopping/killing the project QEMU wrapper can leave an orphan QEMU process that
retains the disk-image write lock. This caused a reproducible false boot failure
until the orphan process was explicitly terminated. The runner needs a
parent/child cleanup regression test.

### Build hardening

AMD64 linking reports that the system malloc object lacks
`.note.GNU-stack`, implying an executable stack. This is a hardening defect
and remains open.

## 3. Generic PCI safety block

Objective: enumeration must not grant DMA. DMA authority belongs to an actual
driver claim and must be revoked when that claim fails.

Implemented behavior:

- shared PCI discovery enables I/O/Memory decode while forcing BusMaster off;
- PCI Status write-one-to-clear bits are not echoed while updating Command;
- BusMaster enable/disable is a separate HAL operation;
- device manager records a provisional driver claim before DMA grant/probe;
- drivers explicitly declare `DEVICE_DRIVER_DMA`;
- DMA grant failure prevents the probe;
- failed probe revokes DMA before allowing fallback binding;
- failed DMA revoke quarantines the device under the failed claim rather than
  handing a still-DMA-capable device to another driver;
- xHCI/EHCI/UHCI declare DMA authority requirements.

Scope limitation: AMD64's independent PCI enumeration path still uses
`command | 7` and must be converted in the next bounded block. VirtIO PCI
also needs an explicit acquire/release lifecycle so DMA is not merely inherited
from that legacy AMD64 path.

## 4. Focused verification for the generic block

- PCI host unit test: PASS.
- PCI ASan/UBSan host test: PASS.
- PCI core freestanding cross-compile: x86_64 PASS, AArch64 PASS.
- driver DMA claim/revoke host test: PASS in AArch64 and AMD64 kernel
  configurations.
- scoped Power12 audit for the changed generic PCI/driver files: 0 findings.
- `git diff --check` for the safety files: PASS.

## 5. Clean integration/runtime evidence

### AArch64

- `make clean`: PASS.
- `make ARCH=aarch64 ... run`: clean build and graphical boot PASS.
- kernel tests: 35 passed / 0 failed.
- VirtIO block/GPU/input initialized.
- reported GPU mode: 1280x800.
- user services including nxcompositor/nxkeyboardd/nxmoused/nxttyd started.
- zero kernel faults.
- stable nxadb commands:
  - `pwd` -> `/home`;
  - `ls /sys/services` -> expected service set.
- graphical screenshot verified at 1280x800.
- initial nxadb shell-health probe remains intermittently failing because of
  the separate input/readiness defect described above.

Build log:
`.git/session1-recovery/logs/pci-generic-aarch64-make-run.log`

Runtime log:
`.git/session1-recovery/logs/pci-generic-aarch64-nxadb-test.log`

### AMD64

- `make clean`: PASS before the AMD64 build.
- `make ARCH=amd64 ... run`: clean build and graphical boot PASS.
- kernel tests: 34 passed / 0 failed.
- PCI/VirtIO block/GPU/input initialized.
- reported GPU mode: 1280x800.
- user services including nxcompositor/nxkeyboardd/nxmoused/nxttyd started.
- zero kernel faults.
- nxadb commands executed successfully in serial evidence:
  - `pwd` -> `/home`;
  - `ls /sys/services` -> expected service set.
- graphical screenshot verified at 1280x800.
- standard initial shell-health probe still reports FAIL and is tracked as a
  separate readiness/input test defect.

Build log:
`.git/session1-recovery/logs/pci-generic-amd64-make-run.log`

Runtime log:
`.git/session1-recovery/logs/pci-generic-amd64-nxadb-test.log`

## 6. Next bounded safety block

Convert the independent AMD64 PCI path to the canonical decode-without-DMA
policy and add explicit VirtIO PCI DMA acquire/release ownership. The block must
cover block, GPU and input failure cleanup, then repeat focused Power12 and the
dual-architecture clean graphical/runtime matrix before staging.
