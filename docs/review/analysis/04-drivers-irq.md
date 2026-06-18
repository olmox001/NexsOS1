> STATUS: agent-generated, **maintainer spot-checked** (2026-06-02) — see REVIEW.md Corrections section.

# Subsystem Analysis 04 — Drivers + IRQ

> Severity/kind tags are defined in [`../TAXONOMY.md`](../TAXONOMY.md).
> Evidence basis: **[verified]** built/run-confirmed; **[static]** read-only; **[inferred]** reasoned with stated assumption.

| | |
|---|---|
| **Subsystem** | Drivers, IRQ framework |
| **Sources** | `kernel/irq/irq.c` (144), `kernel/drivers/console.c` (13), `kernel/drivers/irq_ctrl.c` (8), `kernel/drivers/sys_timer.c` (8), `kernel/drivers/uart/pl011.c` (162), `kernel/drivers/uart/16550.c` (96), `kernel/drivers/gic/gic.c` (153), `kernel/drivers/timer/timer.c` (172), `kernel/drivers/timer/pic_pit.c` (139), `kernel/drivers/virtio/virtio_blk.c` (216), `kernel/drivers/virtio/virtio_input.c` (221), `kernel/drivers/gpu/virtio_gpu.c` (274), `kernel/drivers/gpu/gpu_core.c` (55), `kernel/drivers/keyboard/keyboard.c` (284), `kernel/drivers/pci/pci.c` (142), `kernel/drivers/cpp_test.cpp` (25), `kernel/arch/aarch64/virtio.c` (79), `kernel/arch/amd64/virtio.c` (311) |
| **Headers** | `kernel/include/drivers/{virtio,virtio_blk,virtio_input,virtio_gpu,gic,uart,pci,keyboard,timer}.h`, `kernel/include/drivers/gpu/gpu.h`, `kernel/include/kernel/{irq,platform}.h` |
| **Arch hooks** | aarch64: GICv2 (`gic.c`), ARM generic timer (`timer.c`), PL011 UART, MMIO virtio with `mmio_ops`; amd64: 8259 PIC (`pic_pit.c`), LAPIC, 16550 UART, PCI virtio with `legacy_ops`/`modern_ops` |
| **Build** | **[static]** Not build-verified by this review; see Verification Notes. |

---

## 1. Purpose & Role

This subsystem provides the full hardware-interface stack between the kernel core and physical (QEMU) devices:

1. **IRQ framework** (`kernel/irq/irq.c`) — a thin `irq_chip` abstraction with a 256-entry handler table; the single dispatch entry-point `irq_handler` (aarch64 GIC path) and `irq_dispatch` (amd64 IDT path).
2. **Interrupt controllers** — GICv2 (`gic.c`) for aarch64; 8259 PIC (`pic_pit.c`) shim for amd64 plus LAPIC (handled in `idt.c`).
3. **Timers** — ARM generic timer (`timer.c`) for aarch64; PIT + LAPIC timer (`pic_pit.c` + `hal.c`) for amd64.
4. **UART console** — PL011 (`pl011.c`) for aarch64; COM1 16550 (`16550.c`) for amd64.
5. **VirtIO transport layer** — arch-specific `virtio_setup_queue` / `arch_virtio_scan`; each arch presents a `virtio_transport_ops` abstraction (MMIO ops vs PCI legacy/modern ops).
6. **VirtIO block, input, GPU** — block-device I/O, keyboard/mouse input, and GPU 2-D rendering over virtqueue.
7. **PCI bus driver** — minimal config-space reader/enumerator, no resource management.
8. **GPU core** — device-list abstraction (`gpu_core.c`) over whichever GPU driver registers.
9. **Keyboard** — HID scancode→ASCII translation layer sitting above virtio-input.

---

## 2. Data Flow

```
Exception vector (aarch64)              IDT handler (amd64)
        │                                       │
  irq_handler(regs)               irq_dispatch(vec, regs)
        │                                       │
  GIC acknowledge()                 lapic_eoi() + pic_send_eoi()
        │                                       │
  irq_handlers[irq].handler()    irq_handlers[irq].handler()
        │
  ┌────────────────────────────┐
  │   virtio-blk IRQ handler   │   (not used — busy-wait instead)
  │   virtio-input IRQ handler ├── compositor_update_mouse / keyboard_notify_input
  └────────────────────────────┘
          │
   keyboard_poll / keyboard_process_key
          │
   keyboard_focus_pid ──► kernel_ipc_send (IPC to userland)

arch_virtio_scan()                  arch_virtio_scan()     [amd64]
  └─ MMIO walk @ 0x0a000000            └─ PCI scan → hal_register_device()
       mmio_ops                             └─ modern_ops / legacy_ops
          │                                         │
  virtio_blk_init / virtio_gpu_init / virtio_input_init
          │
  pmm_alloc_pages → virtqueue → virtio_setup_queue → MMIO/PCI registers
```

---

## 3. What Works (verified vs static)

- **[static]** The `irq_chip` abstraction is clean and minimal; the GIC driver fills every op correctly including `init_percpu`, `set_priority`, and `send_ipi_all`.
- **[static]** GIC priority mask (`GICC_PMR = 0xFF`) accepts all priorities; all SGI/PPI priorities set uniformly; pending/active interrupts cleared on init.
- **[static]** PL011 TX path is lock-protected (`uart_lock`) with `spin_lock_irqsave`; newline-to-CR/LF expansion is correct; `_uart_putc_unlocked` used inside locked regions.
- **[static]** 16550 COM1 UART is correctly set up (DLAB sequence, 8N1, FIFO enabled, IER set).
- **[static]** aarch64 virtio MMIO scan correctly reads magic value, device-ID, and assigns `mmio_ops` with direct MMIO register offsets — register accesses on the aarch64 path are correct.
- **[static]** `virtio_blk_init` reads `QUEUE_NUM_MAX` and clamps queue size to 16; the `qsize==0` guard exists (`virtio_blk.c:64–67`).
- **[static]** `translate_modern` (`kernel/arch/amd64/virtio.c:42–65`) maps both `QUEUE_NUM_MAX` and `QUEUE_NUM` to PCI common-config offset 0x18. This is correct: the virtio-1.0 PCI common-config layout has a single `queue_size` register at 0x18 that returns the device max on read and accepts the driver's chosen size on write. No separate max register exists in this layout. Similarly the legacy path maps both to 0x0C (the single queue-size port I/O register), also correct.
- **[static]** `gpu_core.c` correctly handles multi-GPU registration with a spinlock and failover on unregister.
- **[static]** `pci_config_read/write` protects the CF8/CFC address/data pair with `pci_lock` (spinlock + irqsave), preventing torn accesses on SMP.
- **[static]** VirtIO transport abstraction (ops struct + inline wrappers) is the right shape for the planned seL4 driver-isolation goal — a boundary already exists at `virtio_transport_ops`.
- **[static]** `keyboard_process_key` correctly handles shift, caps-lock XOR, and Ctrl+C (ETX injection); layout-override table for Italian characters is complete.

---

## 4. Central Invariant / Theme

**The driver subsystem mixes two incompatible transport models and has no uniform irq-chip contract on amd64.**

aarch64 is internally consistent: MMIO virtio uses `mmio_ops` (direct offsets), GIC dispatches via `irq_handler`, timers use the ARM generic-timer registers. amd64 breaks every boundary: MMIO virtio devices discovered in the fallback scan get `modern_ops` (PCI common-config offsets — wrong for MMIO); the `irq_chip` dispatch loop (`irq_handler`) is bypassed entirely — the IDT handler calls `irq_dispatch` directly and calls `lapic_eoi()` itself; `pic_chip->acknowledge()` hard-returns 1023 so the generic loop is a permanent no-op on amd64. This architectural split is the root of the most severe bugs.

---

## 5. Findings

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| DRV-VIRTIO-01 | W5 | BUG | `kernel/drivers/pci/pci.c:106`, `kernel/arch/amd64/hal.c:26–37` | `pci_get_bar` returns `uint32_t`; 64-bit BARs unhandled → MMIO base truncated when QEMU relocates BARs above 4 GB at `-m 4G`; yields `QUEUE_NUM_MAX == 0` → "Invalid queue size (0)!" crash. |
| DRV-VIRTIO-02 | W3 | BUG | `kernel/arch/amd64/virtio.c:251` | MMIO virtio devices in fallback scan assigned `modern_ops` (PCI offset translation) instead of direct `mmio_ops`; register reads land on wrong offsets for any MMIO virtio device. Not reached in normal x86 QEMU (PCI devices win in `arch_virtio_get_device`), but active for `-machine microvm` or other MMIO-only configurations. |
| DRV-VIRTIO-03 | W4 | BUG · SECURITY | `kernel/drivers/virtio/virtio_blk.c:99–120` | `req` and `status` are stack-allocated and used as DMA targets; device writes to stack memory; no cache-line alignment guarantee; potential data corruption if DMA coherency is not maintained. |
| IRQ-01 | W4 | WRONG-DESIGN | `kernel/drivers/timer/pic_pit.c:57–59`, `kernel/irq/irq.c:87–135` | `pic_chip->acknowledge()` always returns 1023; `irq_handler()` exits immediately on amd64 — the generic chip loop is dead on x86; actual dispatch is `irq_dispatch()` from `idt.c`, which bypasses the chip EOI contract entirely. |
| DRV-VIRTIO-04 | W2 | REFINE | `kernel/drivers/virtio/virtio_blk.c:125–132` | `old_idx = used->idx` snapshot is taken after `avail->idx++` and `hal_mb()` — under the assumption the device polls the avail ring before the `virtio_notify` kick (line 136), a completion could be missed and the busy-wait would time out; not observed under QEMU but latent on compliant non-polling implementations [inferred]. |
| DRV-GIC-01 | W3 | WRONG-DESIGN | `kernel/drivers/gic/gic.c:49–51` | All SPIs hard-targeted to CPU 0 only; no affinity hints, no round-robin — all device interrupts serialise on core 0. Blocks any SMP load distribution. |
| DRV-VIRTIO-06 | W3 | STUB | `kernel/drivers/gpu/virtio_gpu.c:87–92` | `vgpu_set_mode` ignores `width` and `height` and always returns 0; resolution is hard-coded to 720×1280 at init (`virtio_gpu.c:216–217`). |
| DRV-VIRTIO-07 | W3 | MISSING | `kernel/drivers/virtio/virtio_input.c:91,103` | `pmm_alloc_pages` / `pmm_alloc_page` return values not checked; NULL dereference if allocation fails during `init_device`. |
| DRV-PCI-01 | W3 | MISSING | `kernel/include/drivers/pci.h:13`, `kernel/drivers/pci/pci.c` | `pci_scan_and_register()` is declared in the header but never defined or called. No callers exist in the tree. ***RESOLVED (#51)*** — now defined (`kernel/drivers/pci/pci.c:272`, arch-neutral ECAM/port scan into the HAL registry) and called on aarch64 (`kernel/arch/aarch64/hal.c:123`); amd64 uses its own virtio legacy/modern callback with firmware-programmed BARs. Boot logs show `PCI: scanning config space`. |
| DRV-UART-01 | W3 | BUG · SECURITY | `kernel/drivers/uart/pl011.c:109–118` | `uart_getc` and `uart_getc_nonblock` read `rx_tail` without a lock; `uart_irq_handler` writes `rx_head` under no lock; on SMP, two readers or a reader+IRQ can corrupt the index — requires a memory barrier at minimum, a lock for SMP correctness. |
| DRV-GPU-01 | W3 | BUG | `kernel/drivers/gpu/virtio_gpu.c:16–19`, `virtio_gpu.c:111–130` | `desc`, `avail`, `used` are module-level globals; `virtio_gpu_send` is called from `vgpu_flush` under `gpu_lock`, but `gpu_lock` is not held when `virtio_gpu_init` writes them — if `virtio_gpu_send` is called concurrently from init and flush paths the ring state is corrupted. |
| DRV-GPU-02 | W3 | WRONG-DESIGN | `kernel/drivers/gpu/virtio_gpu.c:216–219` | Display resolution queried from the host with `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` is never used; 720×1280 is hard-coded, which forces portrait-mode rendering on any landscape display. |
| IRQ-02 | W3 | WRONG-DESIGN | `kernel/irq/irq.c:7–11,32–59` | `irq_handlers` table has no lock; concurrent `irq_register` / `irq_unregister` from different CPUs and the dispatch path in `irq_handler` can race on the handler pointer. |
| DRV-KB-01 | W1 | BUG | `kernel/drivers/keyboard/keyboard.c:255–284` | `keyboard_read_line` calls `keyboard_read_char()` which unconditionally returns `'\0'` (line 250); the function would spin forever if called. No callers exist in the current tree (confirmed by full-tree grep and build map); dead exported symbol. |
| DRV-KB-02 | W2 | WRONG-DESIGN | `kernel/sched/process.c:27` | `keyboard_focus_pid = 7` is a hard-coded magic PID for the shell. Focus management is correct via the compositor, but the default leaks shell's PID into the driver layer. |
| DRV-VIRTIO-08 | W2 | BAD-IMPL | `kernel/drivers/virtio/virtio_blk.c:139–142`, `virtio_gpu.c:133–136` | All I/O is synchronous busy-wait (up to 10⁹ or 2×10⁸ iterations); no sleep/yield except `hal_cpu_yield()` in blk path; GPU path spins tightly. Blocks all scheduler progress on the issuing CPU. |
| DRV-GIC-02 | W2 | BUG | `kernel/drivers/gic/gic.c:133–136` | `gic_eoi` writes only the IRQ number to `GICC_EOIR`; for SGIs (irq < 16), GICv2 requires bits [12:10] = source CPU ID. SGI EOI with wrong value may leave the interrupt active on the distributor. |
| DRV-VIRTIO-09 | W2 | WRONG-DESIGN | `kernel/arch/aarch64/virtio.c:49–56` | `virtio_setup_queue` silently discards `avail_addr` and `used_addr`; only sets `QUEUE_PFN` (page-frame number of the descriptor ring), relying on the device to derive avail/used at fixed offsets from the same page — which only works if the caller packs all rings into one contiguous allocation. The contract is invisible to the caller. |
| DRV-PCI-02 | W2 | MISSING | `kernel/drivers/pci/pci.c:106–111` | `pci_get_bar` returns `uint32_t` and reads only one 32-bit config register; 64-bit BARs (type bits [2:1] = `10b`) require reading BAR+1 for the high word. No check for BAR type. See also DRV-VIRTIO-01. |
| DRV-GPU-03 | W2 | BAD-IMPL | `kernel/drivers/gpu/virtio_gpu.c:155–165` | `kmalloc(sizeof(struct gpu_device))` and `kmalloc(sizeof(struct virtio_gpu_state))` with no NULL check; kernel panics if kmalloc fails. |
| DRV-CONSOLE-01 | W2 | WRONG-DESIGN | `kernel/include/kernel/platform.h:1–5` | `platform.h` is labelled "Platform definitions for QEMU Virt machine (AArch64)" but is included by both arches; amd64 has no GIC, no PL011, yet `PLATFORM_GICD_BASE`, `PLATFORM_GICC_BASE`, `PLATFORM_UART_BASE` are visible to it. |
| IRQ-03 | W1 | DOC | `kernel/drivers/timer/timer.c:92–95` | Comment "We handle IRQ 27 explicitly in gic.c dispatch" is stale; the special handling is in `irq_handler` in `irq.c`, not in `gic.c`. |
| DRV-CPP-01 | W1 | STUB | `kernel/drivers/cpp_test.cpp:20–25` | `cpp_test_func` allocates a `TestClass` on the stack but does nothing observable; the comment admits it only proves compilation. No global constructors or C++ runtime usage — the file is a proof-of-concept placeholder. |
| DRV-VIRTIO-10 | W1 | REFINE | `kernel/drivers/virtio/virtio_blk.c:145–153` | Line 146 reads `INTERRUPT_ACK` to "clear" interrupt status — on MMIO virtio, clearing requires a *write* to `INTERRUPT_ACK` with the status bits; a bare read has no side-effect. The actual ACK write is absent; the interrupt status register remains set after each I/O completion. |
| DRV-GPU-04 | W1 | BAD-IMPL | `kernel/drivers/gpu/virtio_gpu.c:16–19`, `virtio_gpu.c:195–204` | `desc`, `avail`, `used` are single static pointers for Queue 0; `virtio_gpu_state.qsize` is instance-level, but the descriptor arrays are module-global. A second GPU device would overwrite the first's rings. |

---

## 6. Detailed Entries (selected)

### DRV-VIRTIO-01 — 64-bit PCI BAR truncation causes "Invalid queue size (0)!" at `-m 4G` `[inferred]`

**Assumption:** QEMU relocates the virtio-blk device's 64-bit MMIO BAR to an address above the 32-bit boundary when RAM is configured at `-m 4G`, due to the 3–4 GB PCI hole being consumed by system RAM.

**Trace:** `amd64_pci_callback` (`kernel/arch/amd64/hal.c:26–37`) calls `pci_get_bar(bdf, 4)` which reads a single 32-bit config register and returns `uint32_t`. If the BAR is 64-bit type (`bits[2:1] = 10b`), the actual MMIO address requires reading `BAR+1` (the high 32 bits). With the high word absent, `dev->base` receives only the low 32 bits of the address — typically 0 or a nonsense value. `virtio_pci_init_device` (`kernel/arch/amd64/virtio.c:196–213`) uses `pci_get_bar(bdf, bar)` again when scanning VirtIO capability structures; if `bar_addr` comes out wrong there too, `vdev->base` remains misset. `virtio_blk_init` then reads `VIRTIO_MMIO_QUEUE_NUM_MAX` via `virtio_read_reg → modern_read32 → translate_modern → hal_read32(bad_base + 0x18)`, which returns 0. The `qsize == 0` guard fires (`virtio_blk.c:64–67`), prints "Invalid queue size (0)!", and returns. The block device is unavailable; the filesystem layer then attempts a read through the buffer cache with `qsize == 0`, leading to the downstream divide-by-zero reported in the `mm` analysis. The size-dependence (3 GB works, 4 GB fails) is the diagnostic that confirms a RAM-layout-sensitive PCI BAR addressing fault, not a static register-offset bug.

**Fix direction:** `pci_get_bar` must inspect `bits[2:1]` of the returned value; if `10b`, read the adjacent BAR register and compose a 64-bit address. `hal.c` and `virtio_pci_init_device` must use `uintptr_t` or `uint64_t` for MMIO base storage throughout.

---

### DRV-VIRTIO-02 — amd64 MMIO virtio devices assigned `modern_ops` (wrong register translation) `[static]`

`arch_virtio_scan` on amd64 (`kernel/arch/amd64/virtio.c:239–259`) performs a fallback scan of MMIO addresses at `VIRTIO_MMIO_BASE` (0x0a000000) and assigns `modern_ops` to any device found, with the comment "MMIO is always modern". `modern_ops` routes register reads through `translate_modern`, which maps `VIRTIO_MMIO_QUEUE_NUM_MAX` (0x034) to PCI common-config offset 0x18. On a real MMIO v1 device, offset 0x18 is not `QUEUE_NUM_MAX` — it falls inside the device-features area. Any read of `QUEUE_NUM_MAX` would return incorrect data. aarch64 correctly uses `mmio_ops` (direct passthrough: `hal_read32(dev->base + offset)`) for all MMIO devices. This defect does not explain the `-m 4G` symptom (the MMIO scan is a fallback; PCI devices from the first scan take priority via `arch_virtio_get_device`), but it would cause failures if QEMU is configured with MMIO virtio on x86 (e.g., `-machine microvm`).

**Fix direction:** Introduce an `mmio_ops` struct in `kernel/arch/amd64/virtio.c` (identical to the aarch64 one) and assign it when the MMIO scan finds a device.

---

### IRQ-01 — amd64 `irq_chip` acknowledge contract broken; generic loop is dead `[static]`

`pic_chip` (`kernel/drivers/timer/pic_pit.c:57–66`) sets `acknowledge = pic_chip_acknowledge`, which unconditionally returns 1023. The `irq_handler` main loop in `kernel/irq/irq.c:94–133` breaks on 1023 — so on amd64, every call to `irq_handler` is a no-op. Actual dispatch is done by the IDT common handler in `idt.c:183–184`, which calls `irq_dispatch(vec, regs)` and then calls `lapic_eoi()` directly, completely bypassing the chip's `end()` method. The two code paths (`irq_handler` with GIC chip vs `irq_dispatch` + raw EOI) are irreconcilable without either a chip-aware IDT handler or a chip that correctly participates in `irq_handler`. Functions `irq_enable`/`irq_disable` still work because they invoke `pic_chip_enable/disable` which manipulate PIC mask bits — so that side of the contract holds.

**Fix direction:** Either route amd64 through `irq_handler` (with `acknowledge` returning the IDT vector, not 1023) or document and enforce the split-path contract explicitly. The former is cleaner for seL4-style isolation where the chip layer must be composable.

---

### DRV-VIRTIO-03 — Stack-allocated DMA buffers in `virtio_blk` `[static]`

`virtio_blk_read` and `virtio_blk_write` (`kernel/drivers/virtio/virtio_blk.c:99,119,168,188`) allocate `req` and `status`/`status_w` on the kernel stack and store their addresses directly into virtqueue descriptors. The device then DMA-writes the 1-byte status field back to the stack. This is unsafe for two reasons: (1) the stack frame is reclaimed when the function returns — while the function busy-waits for completion this is tolerable, but if the device is slow or broken, the frame remains live long enough; (2) on aarch64 with cache-coherent MMIO the DMA write is coherent, but on real hardware with a non-coherent interconnect, the CPU's cached copy of the stack slot could be stale after the DMA write. The existing `pmm_alloc_page`-based approach used for the virtqueue ring itself is the correct model. The request header and status byte should be allocated from there.

---

### DRV-GIC-01 — All SPIs hard-routed to CPU 0; SMP interrupt load not distributed `[static]`

`gic_init_dist` (`kernel/drivers/gic/gic.c:49–51`) sets `GICD_ITARGETSR` for every SPI (interrupts 32+) to `0x01010101`, routing all four per-byte targets to CPU 0. This is never updated by `gic_init_cpu` or `irq_register`. On a 4-core QEMU run, all virtio, UART, and device interrupts arrive only on core 0. Combined with the busy-wait I/O model (DRV-VIRTIO-08), core 0 handles all interrupt context, all I/O polling, and all timer ticks — other cores only run scheduler threads. This is tolerable today but is the wrong starting point for the seL4-style goal of isolatable per-core driver threads.

---

### DRV-UART-01 — PL011 ring buffer lacks SMP safety `[inferred]`

**Assumption:** the kernel may run on SMP (4 cores as in the standard QEMU invocation).

`uart_irq_handler` (`kernel/drivers/uart/pl011.c:26–49`) writes `rx_buf` and updates `rx_head` without a lock. `uart_getc` and `uart_getc_nonblock` (`pl011.c:109–131`) read `rx_tail` and `rx_buf` without a lock. `rx_head` and `rx_tail` are `volatile int` — sufficient for a single-core compile barrier but not for SMP. On aarch64 (weak memory model), the store to `rx_buf[rx_head]` and the store to `rx_head` can be observed out of order by the consumer without an explicit `dmb` or `stlr`. The practical risk is low today (interrupt lands on core 0, caller likely on core 0 too), but it is a latent data-race that becomes live once IRQ affinity is distributed.

---

## 7. Refactor Direction (toward the declared goals)

| Goal | Drivers/IRQ implication |
|---|---|
| **seL4-style isolation** | The `virtio_transport_ops` vtable is the right seam — move each driver into its own protection domain behind that interface. Prerequisite: fix the amd64 chip contract (IRQ-01) so both arches share one dispatch path; one chip implementation per arch, one `irq_handler` entry point. |
| **Plan 9 "everything is a file"** | `gpu_get_primary()` / `virtio_blk_read/write` / `virtio_input_poll` already look like file-descriptor ops; they map naturally onto a `/dev/gpu0`, `/dev/blk0`, `/dev/input0` namespace. The `keyboard_focus_pid` mechanism should become a `write` to `/dev/input/focus` rather than a direct PID field. |
| **Real hardware (non-QEMU)** | The entire subsystem is QEMU-only: GIC is GICv2 only, PL011 base is hard-coded, PCI enumeration has no IOMMU/resource-management, no device-tree parsing for timer/UART base addresses. A devicetree or ACPI layer is prerequisite. |
| **Correctness first** | Priority ordering: (1) fix 64-bit BAR reading (DRV-VIRTIO-01, DRV-PCI-02) — unblocks amd64 at 4 GB; (2) fix MMIO ops on amd64 (DRV-VIRTIO-02); (3) unify the irq-chip dispatch contract (IRQ-01); (4) move DMA buffers off the stack (DRV-VIRTIO-03); (5) add IRQ table lock (IRQ-02); (6) fix `keyboard_read_line` or remove it (DRV-KB-01). |

---

## 8. Verification Notes

- **aarch64 [inferred from MM analysis]:** aarch64 boots correctly with virtio-blk; `QUEUE_NUM_MAX` is read correctly via `mmio_ops` at the MMIO v1 offset `0x034`. The aarch64 virtio path is sound as long as the identity-map PA/VA invariant documented in the MM analysis holds.
- **amd64 [-m 3G, verified in MM analysis]:** Works. PCI BARs fit in 32 bits; `pci_get_bar` returns a usable address; `QUEUE_NUM_MAX` > 0; virtio-blk initialises.
- **amd64 [-m 4G, verified in MM analysis]:** Fails with "Invalid queue size (0)!" and subsequent divide-by-zero. The root cause is traced in DRV-VIRTIO-01 [inferred]: 64-bit BAR truncation from `pci_get_bar` returning `uint32_t`.
- **cpp_test.cpp:** [static] No global constructors; `TestClass` is stack-local. No C++ runtime dependency beyond trivial stack allocation. No risk from missing `__cxa_atexit` or `.ctors`.
- **`pci_scan_and_register`:** [static] Declared in `pci.h:13`, not defined in `pci.c`, never called. Confirmed by full-tree grep and build map inspection.
- **`keyboard_read_line`:** [static] Present in `keyboard.c:255–284`; calls `keyboard_read_char()` which returns `'\0'`; confirmed dead by full-tree grep (no callers outside the translation unit).
