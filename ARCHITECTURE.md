# Technical Analysis - OS1TEST-DEV

This document serves as a persistent record of the system architecture, function mappings, and technical debt to guide the refactoring process.

## 1. Hardware Abstraction Layer (HAL)

### CPU Control & IRQ
- `arch_local_irq_enable/disable`:
  - **AArch64**: `msr daifclr, #2` / `msr daifset, #2`.
  - **AMD64**: `sti` / `cli`.
- `arch_vmm_set_pgd`:
  - **AArch64**: `msr ttbr0_el1, x0` + `tlbi vmalle1is` + `dsb/isb`.
  - **AMD64**: `mov cr3, rax`.

### **VirtIO Subsystem (Unified HAL)**
*   **Abstraction Layer**: `virtio_handle_t` (defined in `kernel/include/drivers/virtio.h`) abstracts MMIO and PCI-based access.
*   **AArch64 Implementation**: Uses fixed MMIO addresses from the Device Tree/QEMU Virt platform.
*   **AMD64 Implementation**: Uses PCI discovery.
    *   **Legacy Support**: Detects devices with IDs `0x1000-0x103F`, uses BAR0 I/O ports.
    *   **Modern Support**: Detects devices with IDs `0x1040-0x107F` or via PCI Capabilities (Type 1 Common, Type 2 Notify).
    *   **Fix (2026-05-11)**: Corrected Modern register offsets (e.g., `queue_select` at `0x16`, `queue_size` at `0x18`) which resolved I/O timeouts.
*   **Drivers**:
    *   `virtio_blk.c`: Synchronous sector R/W using a shared polling loop with `arch_yield()`.
    *   `virtio_gpu.c`: Basic 2D command submission for framebuffer support.

### **Memory Management**
*   **PMM**: Physical Page Allocator (Buddy-like). Standardized to return physical addresses in the identity-mapped range.
*   **VMM**: 4-level page tables (AArch64/AMD64).
    *   **Identity Mapping**: First 1GB mapped 1:1 on both architectures to simplify early driver development.
    *   **PAGE_DEVICE**: Flag for uncacheable MMIO/PCI-BAR mapping.
  - AArch64 uses `MAIR_EL1` for memory attributes.

## 2. Timer System

### Current Implementation
- `HZ` is typically 100.
- **AArch64**: `kernel/drivers/timer/timer.c`
  - Uses ARM Generic Timer (CNTV).
  - Frequency read from `cntfrq_el0`.
  - programs `cntv_cval_el0` for the next tick.
- **AMD64**: `kernel/drivers/timer/pic_pit.c`
  - Uses legacy 8254 PIT.
  - Frequency is hardcoded to 1.193182 MHz.
  - programas Channel 0 via Port I/O.

### Technical Debt
- Timer frequency logic is fragmented.
- AMD64 uses a hardcoded fallback frequency in some places.

## 3. VirtIO Discovery

### Implementations
- **AArch64**: `kernel/arch/aarch64/virtio.c`
  - Hardcoded MMIO scan from `0x0a000000` (`VIRTIO_MMIO_BASE`).
  - IRQs are hardcoded starting from 48.
- **AMD64**: `kernel/arch/amd64/virtio.c`
  - PCI enumeration via `pci_enumerate()`.
  - Handles both Legacy (Port I/O) and Modern (MMIO) VirtIO-PCI devices.

### Technical Debt
- Lack of Device Tree (DTB) support on AArch64 for dynamic discovery.
- Inconsistent register access (MMIO vs Port I/O) is partially abstracted but needs cleaner integration into the unified `virtio_read/write_reg`.

## 4. Build System & Tooling
- `Makefile`: Manages `ARCH=aarch64` and `ARCH=amd64`.
- `tools/mkdisk.c`: Custom tool to create GPT + Ext4 images.

---
*Last Updated: 2026-05-11*
