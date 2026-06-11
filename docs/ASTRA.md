# ASTRA — architectural guidelines for NEXS (Phase B and beyond)

> **ASTRA** (*Abstract Service Tree Runtime Architecture*): the kernel core is
> completely independent of the ISA and the platform; every hardware dependency
> is expressed as a **provider of primitives**, organized hierarchically.
> The kernel does not know hardware — it knows **services**.
>
> This document adapts the maintainer's ASTRA model to the actual NEXS codebase
> and defines the implementation method for Phase B (epics #92–#96) and a
> possible Phase C. It complements `docs/PROJECT_CHARTER.md` (the seL4/Plan 9
> target): the charter says *where* services end up, ASTRA says *how the layers
> talk* while they get there.

## 1. The layer model

```
                 User Applications
                         │
                     API / libc
                         │
──────────────── Kernel Core ─────────────────────
  Scheduler · Process · VM · VFS · IPC · Syscalls
                         │
────────────── Kernel Primitives ─────────────────
  IRQ · Timer · CPU/SMP · Bus · DMA · Console
                         │
──────────── Infrastructure Providers ────────────
  APIC/IOAPIC · GICv2 · PIT/HPET · ARM Generic
  Timer · ACPI · FDT · PCI · VirtIO-MMIO
                         │
──────────────────── ISA ─────────────────────────
  x86_64 · AArch64  (context switch, trap entry,
  paging, TLB — and nothing else)
```

Rules that follow from the model:

1. **Every layer exports primitives to the layer above** and consumes only the
   contracts of the layer below. No layer contains knowledge of *which*
   implementation sits underneath it.
2. **There are no "platforms", only providers.** A PC is {APIC, IOAPIC, PIT,
   ACPI, PCI}; QEMU virt is {GICv2, ARM generic timer, FDT, VirtIO-MMIO}.
   The kernel core never knows which combination it is running on.
3. **The ISA layer is minimal**: context switch, trap entry, page-table walks,
   TLB maintenance. Everything else that today lives under `kernel/arch/` is
   really a provider and must migrate behind a contract over time.
4. Contracts (the `*_ops`/`*_chip` structs) live in `kernel/include/kernel/`;
   provider implementations live with the driver, not with the arch.

## 2. Where NEXS stands today

Already provider-shaped (keep and extend — do not reinvent):

| Contract | Today | Providers |
|---|---|---|
| `struct irq_chip` (`kernel/irq/irq.c`, chip-owned EOI via `irq_chip_end`) | ✅ conforming | GICv2 (aarch64) · LAPIC/8259 (`pic_chip`, amd64) |
| Timer tick (`kernel/core/timer.c` callback) | ✅ conforming | ARM generic timer · LAPIC timer (PIT = calibration only, halted after) |
| `hal_bus` device registry (`kernel/core/hal_bus.c`) | ✅ embryonic bus contract | VirtIO-MMIO (aarch64) · VirtIO over PCI (amd64) |
| Console | ✅ conforming | PL011 · 16550 |

Violations the phases must fix (worst first):

| Violation | Where | Fixed by |
|---|---|---|
| Syscalls and the ELF loader call `ext4_*` directly — no FS contract at all (`vfs.c` is only path normalization) | `kernel/fs/` | **B1** (#64/#56) |
| `virt_to_phys` is identity; no PA/VA model, no W^X — the "VM primitive" doesn't exist as a contract | `kernel/mm/`, `vmm.h` | **B2** (#92) |
| No capability layer: any process can kill PIDs, steal focus (syscall 232), overwrite registry keys — providers cannot be wired safely without it | ABI | **B3** (#93) |
| Boot-protocol parsing and the 1 GB fallback are monolithic platform code instead of ACPI/Multiboot *providers* | `kernel/arch/amd64/platform/platform.c` (frozen file — do not touch until B4 replaces it behind a contract) | **B4** (#94) |
| Compositor/fonts/registry live in the kernel core and reach into sched (focus boost) — a service living below its layer | `kernel/graphics/` | **B5** (#95) |

## 3. How each Phase B microphase applies ASTRA

- **B1 — VFS/ext4**: the first real ASTRA seam. Introduce `struct fs_ops`
  (open/read/write/list/stat) + a mount table keyed by partition; ext4 becomes
  *a provider registered behind it* (extent-tree support added inside the
  provider). Acceptance includes: **zero `ext4_*` calls outside `kernel/fs/`**.
  This creates the provider chain `virtio-blk → block → fs provider → VFS`.
- **B2 — address space (#92)**: define the VM primitive (PA↔VA conversion,
  map/unmap/protect with W^X) as a kernel contract; the per-arch page-table
  code shrinks to the ISA layer (paging + TLB only). Prereq for ASLR/KASLR.
- **B3 — ABI + capabilities (#93)**: capabilities are ASTRA's *wiring*
  mechanism. A consumer asks for `CAP_IRQ | CAP_DMA | CAP_MMIO`-style handles;
  it never learns who provides them. Start with per-process caps on the
  existing syscalls (kill/focus/registry), keeping the handle model compatible
  with Phase C drivers.
- **B4 — amd64 parity (#94)**: ACPI/MADT becomes an *infrastructure provider*
  (like FDT already is on aarch64), feeding CPU count, memory map and IRQ
  routing through the same contracts — `platform.c` is absorbed, not patched.
- **B5 — services/HAL (#95)**: the main ASTRA landing zone. Reorganize the
  tree so that primitives (IRQ/Timer/Bus/DMA/Console) and providers
  (APIC/GIC/timers/PCI/VirtIO-MMIO/ACPI/FDT) are explicit; decouple
  compositor↔sched (SCHED-01) so graphics can later leave the kernel.
- **B6 — SMP sweep (#96)**: per-CPU bring-up and IPIs behind a CPU/SMP
  primitive; async block I/O (DRV-VIRTIO-08) becomes the block provider's
  internal concern.

## 4. Phase C (proposed, after B): userspace driver services

With B1–B5 done, the kernel keeps only ISA + infrastructure providers + core,
and exports the four service primitives ASTRA requires:

```
map_mmio()        # device window into a service's address space (needs B2+B3)
wait_irq()        # blocking IRQ delivery to userspace (needs B3 caps)
dma_alloc()       # pinned DMA-safe buffers (needs B2 PA/VA + IOMMU-less bounce)
send_ipc()/recv() # already exists; formalized by B3
```

Functional (L2) drivers then migrate to supervised ELF services —
`net.elf`, `gpu.elf`, `audio.elf`, eventually `blk.elf` — gaining fault
isolation (a crashed driver is respawned by init, which already does
deterministic supervision since `f37d137`) and portability (the same
`virtio_net.elf` runs on both arches because it consumes primitives, not
APIC/GIC). Migration order: start with a *non-boot-critical* driver (net or
audio when they exist, or input); `blk.elf` goes **last** because the rootfs
depends on it (needs an in-kernel fallback or an initramfs first).

## 5. Practical rules for every commit from now on

1. New hardware support = a **new provider implementing an existing contract**.
   If the contract is missing, define it in `kernel/include/kernel/` first.
2. **No new code in `kernel/arch/<arch>/platform/`**; arch directories may only
   grow ISA-layer code (context switch, traps, paging, TLB).
3. The kernel core (`sched/ mm/ fs/ ipc/`) may include only `kernel/include/`
   contracts — never `arch/` or driver headers. (Today's offenders are
   catalogued in §2 and burn down with their phase.)
4. A provider may depend on primitives of the layer below, never on a sibling
   provider's internals (e.g. virtio-blk must not know whether its transport
   is MMIO or PCI — that is the bus provider's job).
5. Cross-layer shortcuts taken for expedience get a `NOTE(ASTRA-VIOLATION):`
   comment + a GitHub issue, so they are debt, not precedent.
