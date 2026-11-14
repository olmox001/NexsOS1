# Graphics port validation log

## 2026-07-11 baseline, AArch64

Command: `make run`

Result: successful build and boot in QEMU `virt` with VirtIO-MMIO.

- Kernel unit tests: 5 passed, 0 failed.
- VirtIO block, input and GPU discovered.
- Host scanout reported 1280x800.
- Four AArch64 CPUs reached online state.
- Init, notification service, dock, launcher and NXShell started.

Observed non-fatal build diagnostic:

```
build/aarch64/ksyms.S:4: Warning: zero assumed for missing expression
```

This warning is an existing baseline defect. It must be eliminated before a
zero-warning release baseline is declared.

## 2026-07-11 foundation, AMD64

Command: `make run ARCH=amd64`

Result: successful build and boot in QEMU x86_64 with VirtIO-PCI.

- Kernel unit tests: 5 passed, 0 failed.
- Modern VirtIO PCI GPU, input and block devices discovered.
- Host scanout reported 1280x800.
- Four AMD64 CPUs reached online state.
- Init, notification service, dock, launcher and NXShell started.

Existing linker diagnostics observed on this architecture:

```
kernel.elf: allocated section `.note.PVH' not in segment
kernel.elf: LOAD segment with RWX permissions
build/amd64/ksyms.S:4: Warning: zero assumed for missing expression
```

These diagnostics predate the graphics portability adapter and are a separate
hardening backlog; they are not suppressed by this work.

## 2026-07-11 surface/event increment

Commands: `make run`; `make run ARCH=amd64`

Result: both QEMU boots completed normally with the new in-kernel surface
contract test enabled.

- AArch64 VirtIO-MMIO: 6 passed, 0 failed.
- AMD64 VirtIO-PCI: 6 passed, 0 failed.
- `test_gfx_surface_contract` verifies clipped fill, source-over ARGB blend and
  ABGR-to-ARGB conversion with caller-owned surfaces.
- The userland portability adapter now translates focused keyboard, mouse and
  resize input into an ABI-independent event record for the SDL2/D3D9 layers.

## Required validation for each increment

1. `make run` — interact with desktop, window dragging/resizing, theme/style
   change and the affected graphics demo; obtain visual feedback.
2. `make run ARCH=amd64` — repeat the same checklist over VirtIO-PCI.
3. Record both terminal result and visual outcome here before expanding the
   porting surface.
