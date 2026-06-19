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

--------------------hal-------------------
All abstractions of the ISA implementation are managed and pass from the HAL layer in Aarch to uniformly abstract the kernel for both architectures
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
3. **The ISA layer is minimal,Abstract logic in the HAL layer**: context switch, trap entry, page-table walks,
   TLB maintenance. Everything else that today lives under `kernel/arch/` is
   really a provider and must migrate behind a contract over time, All architectural implementations must be abstracted in the HAL.
4. Contracts (the `*_ops`/`*_chip` structs) live in `kernel/include/kernel/`;
   provider implementations live with the driver, not with the arch.
5. **The HAL is the seam, and divergence is a bug.** Core code calls only HAL
   primitives and never the ISA layer directly; if the same logic must observe
   *which* arch it runs on, that logic belongs behind a HAL contract. The
   three-tier timer is the reference implementation: `kernel_timer_tick` +
   `timer_percpu_tick`/`timer_percpu_arm` drive both arches identically over
   `arch_timer_get_count/_get_freq/_set_compare`, so the only per-arch code left
   is the trap entry. When one root cause produces *different symptoms per arch*
   (see `docs/report/TIMER-UAF-01-CERTIFIED-FIX.md`), treat it as residual ISA
   leakage to migrate behind the HAL — the conformance backlog is tracked in
   `docs/direction/DIR-06-hal-conformance.md`.

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
  *Progress (GFX-DYN-01, DIR-07):* the virtio-gpu driver is one provider over
  the HAL transport on both MMIO and PCI, exposing a `gpu_ops` capability
  contract (display info / set mode) the compositor consumes — no resolution
  magic numbers in the core; the terminal emulator is extracted to `term.c`;
  compositor↔sched stays inverted.
  *Extended by the 2026-06-19 directive (§6.4):* **formalize the SRL** —
  separate explicitly in the source tree the subsystems that belong to the
  Service Runtime Layer (GUI, audio, registry, notifications) from the HAL;
  these become the migration candidates for Phase C supervised services.
  Also: **configs-as-files** (expose kernel/system configuration as files where
  feasible, §6.3) and **capability-mediated VFS access** (consistent with B3).
- **B6 — SMP sweep (#96)**: per-CPU bring-up and IPIs behind a CPU/SMP
  primitive; async block I/O (DRV-VIRTIO-08) becomes the block provider's
  internal concern.

## 4. Phase C (after B): userspace driver services + ABI formalization

With B1–B5 done, the kernel keeps only ISA + infrastructure providers + core,
and exports the four service primitives ASTRA requires — now under the stable
**OS1low_** ABI naming (§6.1):

```
OS1low_map_mmio()              # device window into a service's address space (B2+B3)
OS1low_wait_irq()             # blocking IRQ delivery to userspace (B3 caps)
OS1low_dma_alloc()            # pinned DMA-safe buffers (B2 PA/VA + IOMMU-less bounce)
OS1low_ipc_send()/OS1low_ipc_recv()  # exists; formalized by B3
```

Functional (L2) drivers then migrate to supervised ELF services —
`net.elf`, `gpu.elf`, `audio.elf`, eventually `blk.elf` — gaining fault
isolation (a crashed driver is respawned by init, which already does
deterministic supervision since `f37d137`) and portability (the same
`virtio_net.elf` runs on both arches because it consumes primitives, not
APIC/GIC). Migration order: start with a *non-boot-critical* driver (net or
audio when they exist, or input); `blk.elf` goes **last** because the rootfs
depends on it (needs an in-kernel fallback or an initramfs first).

**Extended by the 2026-06-19 directive:**
- **Formalize the OS1low_ ABI** — the four primitives above become the
  documented, stable low-level ABI surface (§6.1).
- **libos1 skeleton** (#163) — a stub library exporting the `OS1_` and
  `OS1low_` symbols, linked against the kernel ABI. This is what musl links
  against in Phase D; **Phase D does not start without it.**
- **Object manager** (§6.2) — every kernel object (file, window, GPU context,
  audio stream, …) reachable through a uniform handle + capability, with the
  shared `OS1_object_read/write/wait` interface.
- **SRL service migration** (§6.4) — compositor, registry, notification, audio
  become supervised ELFs over the formalized ABI (same migration order; the
  VFS-as-service evaluation is tracked in #162).

## 4a. Phase D (new): musl integration + libc port

Prerequisite: Phase C complete, libos1 skeleton stable. **musl** is added as a
git submodule under `user/sys/library` and adapted to call `libos1 → OS1low_`
instead of Linux syscalls — **it is not reimplemented**. The POSIX→OS1
translation layer (the mapping table in §6.8) lives in `user/sys/library` and
never touches the kernel. The **base-nexs** port (`user/bin/base-nexs`) is the
canonical libc verification harness; its docs join the project context at the
same rigor as the rest of `docs/`. Tracked in #165.

## 4b. Phase E (new): userland optimization

After Phase D, `user/` is rebuilt and optimized on top of the completed
libraries (libos1 + musl personality). Until Phase E, userland is in a
**voluntary stall** and does not block the earlier phases. Tracked in #166.

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

---

# 6. Extended directive (2026-06-19): OS1 ABI & service model

> This section extends §1–§5 (which remain in force) with the ABI/service model
> set by the maintainer. It reframes epic #120 (onion userland) and adds Phase D
> (musl) and Phase E (userland optimization). Reference models are named per
> theme; NEXS adopts the *semantics*, not the implementations.
> Naming convention (global rename, #164): **`OS1low_`** = low-level ABI
> (stable, minimal, near-immutable; direct kernel entry); **`OS1_`** = high-level
> API (versionable, served by libos1 and the SRL services). This refines DIR-01
> (#137), which piloted the uppercase `OS1_` prefix.

## 6.1 OS1 ABI model: two-level separation

The kernel's native syscall interface is **not** POSIX. POSIX is one of several
*personalities* built on top. The full stack:

```
                       Applications
                            │
           Linux/POSIX    Win32      Android
                │            │            │
             musl         ntdll       Bionic
                │            │            │
          POSIX API     Win32 API   Android API
                  \          │          /
                     ── libos1 ──
                            │
         ┌──────────────────┴─────────────────┐
         │    High-level OS1 API  (OS1_)       │
         │  GUI · Audio · GPU · Net            │
         │  Registry · Storage · Service       │
         └──────────────────┬─────────────────┘
                            │
         ┌──────────────────┴─────────────────┐
         │          Kernel Core               │
         │  Scheduler · VM · VFS · IPC        │
         │  Object Manager · Capabilities     │
         └──────────────────┬─────────────────┘
                            │
              Low-level OS1 ABI  (OS1low_)
                            │
         ┌──────────────────┴─────────────────┐
         │            ASTRA HAL               │
         │  IRQ · Timer · DMA · SMP · Bus     │
         └──────────────────┬─────────────────┘
                            │
              Infrastructure Providers
         APIC · GIC · ACPI · FDT · PCI · VirtIO
                            │
                 Minimal ISA layer
             x86_64               AArch64
```

**`OS1low_` primitives** (stable ABI; the four already named in §4 for Phase C
adopt this prefix and become the documented surface):

| Domain | Primitives |
|---|---|
| Object manager | `OS1low_handle_create`, `OS1low_handle_duplicate`, `OS1low_handle_close` |
| VM | `OS1low_vm_create/_map/_unmap/_protect/_share/_pin` |
| Thread | `OS1low_thread_create`, `OS1low_thread_sleep` |
| Process | `OS1low_process_spawn/_exec/_clone/_handle` |
| IPC | `OS1low_ipc_send`, `OS1low_ipc_recv` |
| Capability | `OS1low_cap_grant`, `OS1low_cap_query` |
| Hardware access | `OS1low_map_mmio`, `OS1low_wait_irq`, `OS1low_dma_alloc` |

**`OS1_` primitives** (high-level, served by libos1/SRL):

| Domain | Primitives |
|---|---|
| GUI | `OS1_window_create/_resize`, `OS1_surface_create`, `OS1_display_enumerate` |
| GPU | `OS1_gpu_context_create`, `OS1_gpu_submit` |
| Audio | `OS1_audio_stream_open` |
| Clipboard | `OS1_clipboard_get/_set` |
| Notification | `OS1_notify_post` |
| Registry | `OS1_registry_get/_set` |
| Service | `OS1_service_connect` |
| Object I/O | `OS1_object_read/_write/_wait` |

## 6.2 Unified object I/O model

Everything is an object: File, Socket, Pipe, Window, Framebuffer, GPU context,
Audio stream, Input device — all sharing `OS1_object_read` / `OS1_object_write`
/ `OS1_object_wait`. Reference models: Darwin, Windows NT, Fuchsia. This removes
type specialization at the kernel boundary. The B3 per-process fd table (#90)
is the seed; it generalizes into the object manager (§6.1) in Phase C.

## 6.3 VFS: two distinct models for two distinct concerns

1. **Internal OS1 syscalls / namespace & memory management** — Plan 9 as the
   structural/philosophical model: everything is a namespace, shared memory is a
   natural mechanism, no rigid kernel/user conceptual split. W^X, COW, ASLR,
   lazy allocation and shared mappings already live in the VM layer (B2). This is
   the *operating model*, not a p9 port.
2. **Partitioning & file/directory distribution** — Unix SystemV/Linux standard:
   partition table, mount table, config files, dynamic resolver, capability-based
   access control (consistent with B3).
3. **Configurations as files** — where feasible during B5/Phase C, kernel and
   system configuration is exposed as configurable files (boot params, device
   config, service config), consistent with the Plan 9 philosophy.
4. **VFS as a user service (future)** — long-term, the VFS becomes a supervised
   SRL service (#162). For now it stays in the kernel.

## 6.4 Service Runtime Layer (SRL)

The kernel exports two distinct layers to userspace; do not conflate them:

```
Kernel
├── HAL  (invisible to applications)
│     IRQ · Timer · DMA · SMP · Bus · MMIO
│
└── SRL  (Service Runtime Layer — foundation of libos1)
      GUI · Registry · Audio · GPU
      Service Manager · Notifications
      Clipboard · Security
```

The HAL is already defined (§1). The SRL is the set of system services libos1
consumes; SRL services are supervised ELF processes (Phase C, §4), exposed via
IPC/capability. The SRL/HAL split must be explicit in the source tree after B5.

## 6.5 Process / IPC / sandbox / capability — seL4 + Mach + NT

Reference models: seL4 (capability semantics), Mach (IPC structure), NT (object
manager). Partly implemented in B3; formalization target = Phase C.
- **From seL4:** capabilities are the only reference to kernel objects (no
  PID-by-number access without a capability); handles are unforgeable; rights are
  separable and delegable. The 4-level privilege/capability sandbox (B3,
  `24fab00`) is the foundation — **not to be touched, to be formalized.**
- **From Mach (semantics, not implementation):** ports are first-class objects
  (a port is itself a capability); sync RPC + async messaging, out-of-line memory
  transfer. Pure-Mach context-switch cost is avoided — the semantics ride on the
  existing B3 IPC layer.
- **From NT:** per-process handle table (B3, `51c3179`); object manager as the
  central dispatch for all kernel objects.

Process structure (implemented in B2+B3, to be formally documented):
`Process = { VM space (B2), Handle table (B3), Capability space (B3), Threads }`.

## 6.6 Registry

A tree of nodes where: every node is representable as a file (§6.3 namespace);
every operation is a message (§6.5 IPC); every node has an associated capability
(no operation without a valid handle). First-writer-wins ownership (B3) is the
baseline; sub-capability delegation is the future direction. This unifies
configuration, system state and security policy in one inspectable, auditable
namespace.

## 6.7 Graphics & display

Native stack — **no kernel dependency on X11/Wayland**:

```
Application → libos1 → Window Server (SRL) → Compositor → GPU service (SRL)
```

X11, Wayland, Win32 GDI, Android SurfaceFlinger are translation layers above
libos1, not kernel dependencies. Reference models: Darwin, Fuchsia. The
compositor↔sched decoupling (SCHED-01, in B5 scope) is the prerequisite.

## 6.8 Compatibility personalities via libos1

libos1 is the platform ABI. Personalities translate their native API to
libos1 → OS1low_; none access the kernel directly.

```
musl           → POSIX layer   → libos1 → OS1low_ → Kernel
ntdll/kernel32 → Win32 layer   → libos1 → OS1low_ → Kernel
Bionic         → Android layer → libos1 → OS1low_ → Kernel
```

| Native call | OS1 primitive |
|---|---|
| `open(path, flags)` | `OS1low_handle_create(ns, path, rights, type)` |
| `fork()` | `OS1low_process_clone()` |
| `exec()` | `OS1low_process_exec()` |
| `mmap()` | `OS1low_vm_map()` |
| `pthread_create()` | `OS1low_thread_create()` |
| `sendmsg()` / `recvmsg()` | `OS1low_ipc_send()` / `OS1low_ipc_recv()` |
| `CreateFileW()` | `OS1low_handle_create()` |
| `CreateProcessW()` | `OS1low_process_spawn()` |
| `WaitForSingleObject()` | `OS1low_object_wait()` |
| Binder (Android) | OS1 IPC (`OS1low_ipc_*`) |
| SurfaceFlinger (Android) | OS1 compositor (SRL) |
| AudioFlinger (Android) | OS1 audio (SRL) |
