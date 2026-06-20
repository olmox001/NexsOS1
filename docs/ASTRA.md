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
| ~~No capability layer: any process can kill PIDs, steal focus (syscall 232), overwrite registry keys~~ — **RESOLVED**: real object manager + unforgeable per-process handles with attenuable rights now back kill/IPC/registry/windows (see §7) | ABI | **B3 ✅** (§7) |
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

---

# 7. Implementation status (updated 2026-06-20)

§1–§6 describe the target; this section records what is **actually implemented**
in the tree today, so the structural work ahead starts from fact. Everything
below is verified building `-Werror` on both arches (aarch64 + amd64) and booting
with 0 panics; capability regression `captest` 9/9, `capkill` 5/5.

## 7.1 Capability layer — the real object manager (§6.1/6.2/6.5) ✅ DONE

The B3 "seed" has been generalized into a real, unforgeable-handle capability
layer — no longer ambient `proc_has_cap()` identity for the object surface.

- **Model**: a kernel resource is a refcounted `struct kobject`; a process names
  it only through a **handle** — a private integer index into its per-process
  handle table. A handle with no installed slot is `-EBADF` (unforgeable).
  Rights are a **separable, attenuable** subset (`dup`/`grant` can only shrink),
  so privilege escalation is impossible by construction (seL4 semantics).
- **Acquisition is ambient-gated, use is capability-based**: getting a write FILE
  cap needs `CAP_FS_WRITE` + the `/sys,/bin` ACL; a PROCESS cap needs kill
  authority; a cross-process WINDOW control cap needs window-manager authority.
  Once held, the handle's rights are the only authority that matters — a granted
  handle delegates that authority (Mach/seL4 grant).
- **Object types**: `OBJ_TYPE_FILE` (VFS read/write/seek), `OBJ_TYPE_PROCESS`
  (wait, capability IPC send, kill via `OBJ_CTL_KILL`), `OBJ_TYPE_REGKEY`
  (registry get/set — §6.6, first-writer-wins, `registry_enum`),
  `OBJ_TYPE_WINDOW` (§7.3).
- **ABI**: syscalls `235..243` (`SYS_HANDLE_CREATE/_DUP/_CLOSE`, `SYS_CAP_QUERY/
  _GRANT`, `SYS_OBJECT_READ/_WRITE/_WAIT/_CTL`) + `SYS_WINDOW_ENUM 202`.
  Userland: `OS1low_handle_create/_duplicate/_close`, `OS1low_cap_query/_grant`,
  `OS1_object_read/_write/_wait/_ctl`. Namespaces `OS1_NS_FS/PROC/REG/WIN`.
- **Files**: `include/api/object.h` (shared ABI), `kernel/core/object.c`,
  `kernel/include/kernel/object.h`. Handle tables are lazily allocated and freed
  at process teardown (`process_handles_destroy`). Tests: `/bin/{captest,capipc,
  capreg,capkill}`.

## 7.2 Per-path capability presets — VFS stratification (§6.3) ✅ DONE

A binary's **location in the VFS sets its privilege preset** (`level_for_path`
in `dispatch_spawn`): `/sys/bin/*` spawns at **ROOT** (system authority,
per-service refinement is a follow-up), everything else (notably `/bin/*`) at
**USER**. This is a ceiling + default, **not** an escalation: the monotonic
creator-clamp in `process_create_caps` still forbids a child being more
privileged than its creator, so a USER shell launching a `/sys/bin` binary does
NOT gain root. `/sys/bin` is write-protected (object.c denies non-machine writes
under `/sys`,`/bin`) → the binaries backing the preset are immutable. Follow-up:
per-service cap refinement and VFS-level read-only enforcement.

## 7.3 Window objects + native window server (§6.7) ✅ DONE (base)

Windows are first-class capability objects. The compositor stays the pure
*mechanism*; window-management *policy* lives in a userland service.

- `OBJ_TYPE_WINDOW` + `OS1_NS_WIN`: a window handle with `OS1_object_read` →
  `struct window_info`, and `OS1_object_ctl` verbs `OBJ_CTL_MINIMIZE/_RESTORE/
  _FOCUS/_CLOSE`. Acquisition: your own window is free; cross-process WRITE/
  DESTROY needs ROOT/machine (window-manager authority); FOCUS needs only READ,
  matching the compositor's unprivileged click-to-focus.
- `SYS_WINDOW_ENUM` → `struct window_info[]` (id/pid/geometry/flags/title).
- Compositor: a `minimized` window state + a titlebar **background button**;
  `compositor_minimize/restore/focus/window_info`.
- **/sys/bin/nxui** — the **dock** = the Window Server (SRL) as a userland ROOT
  service, supervised by init: enumerates windows, draws a tile per app, click
  focuses/restores/backgrounds via `OS1_window_*`. The compositor itself is left
  minimal.
- Files: `kernel/graphics/compositor.c`, `kernel/include/kernel/graphics.h`,
  `user/sys/bin/nxui.c`, libos1 `OS1_window_enum/_minimize/_restore/_focus/_close`.

## 7.4 Stratified SRL services (§6.4) ✅ pattern established

Every system CLI/control is built as **a reusable helper layer + a thin
frontend**, usable by both user and system apps. The helper adds **no ambient
checks**: it only wraps syscalls the kernel already gates per caller, so the
service is **secure-by-caller** automatically (a USER app and a ROOT service get
exactly their own rights). Established examples: `nxres` (display/style/theme),
`nxproc` (process management — `nxproc.h` helper + `nxproc` CLI; the shell `ps`
and `top` consume the same helper, `top` de-spinbombed to render only on change).
Planned, same pattern: **nxinfo** (system info) and **nxperms** (user-level
authorizations).

## 7.5 Unified input event ABI (§6.2 input objects / DIR-03) ✅ base

Window apps read input through one API: `input_poll_event(input_event_t*)` —
keyboard (`IPC_TYPE_INPUT`), mouse (`IPC_TYPE_MOUSE`, evdev `MOUSE_BTN_*` codes,
logical window coords), and window/desktop resize (`IPC_TYPE_RESIZE`). The
compositor delivers events to the focused window. `MOUSE_BTN_*` are centralised
in `include/api/input.h`. **Open**: per-window mouse delivery beyond the focused
window and a desktop-resize broadcast are tracked follow-ups (DIR-03/DIR-07).

## 7.6 What remains structural (next)

The **call-surface refactor** (DIR-01/#164) is the next structural work:
unify/standardise **all** existing syscalls/verbs onto the OS1_/OS1low_ +
capability model (not just renaming) — read the §6.1 ABI tables first so the
final names/shape match the documented surface and the work is not done twice.
Then the explicit **SRL/HAL source-tree split** (§6.4, B5) and Phase C device
primitives (§4).
