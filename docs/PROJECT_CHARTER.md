# OS1 / NEXS — Project Charter

> Purpose, scope, and target architecture, derived from a first-principles code
> review (see [`review/`](review/)). This is the north-star document: the review
> says *where we are*, this says *where we are going and why*. Aspirational items
> are marked **[goal]**; verified current capabilities are marked **[now]**.
>
> Status date: 2026-06-12 (originally 2026-06-02). The README has since been rewritten to
> the verified state; this document remains the north star.

---

## 1. What OS1/NEXS is

OS1 (internal codename **NEXS**) is a **from-scratch, dual-architecture
(AArch64 + x86-64) operating system** that boots on QEMU `virt`/`q35`, drives
**VirtIO** GPU/input/block devices, and runs a **graphical, multi-window
environment with a TTY shell** on top of a preemptive, SMP-capable kernel.

It is simultaneously two things, and the tension between them defines the roadmap:

- **[now]** A *working graphical OS asset*: it really boots, detects CPUs and RAM,
  enables the MMU, mounts an Ext4 root, composites windows, and runs userland ELF
  programs (shell, a notification server, a font manager, demos, a DOOM port).
- **[goal]** A *principled HybridKernel MicroKernel ispired* in the lineage of **seL4** (capability-based
  isolation) and **Plan 9** (everything is a file, thin portable core). Today most
  "services" (compositor, fonts, VFS/Ext4, registry) live **inside the kernel**;
  the project's purpose is to move them **out** into sandboxed userland services
  behind a coherent ABI.

## 2. Verified current capabilities **[now]**

Confirmed by building and running this revision (headless QEMU, serial capture):

| Capability | aarch64 | amd64 |
|---|---|---|
| Builds clean (`-Werror -Wall -Wextra -Wpedantic -Wshadow`) | ✅ | ✅ |
| Boots to interactive **TTY shell** in a composited window | ✅ | ✅ |
| **Higher-half kernel**, documented PA/VA contract, **W^X** | ✅ | ✅ |
| Dynamic RAM discovery (boot-protocol memory map), MMU + remap | ✅ | ✅ (PVH/MB; PCI-hole accounting defect, #94) |
| SMP (multi-core bring-up, work-stealing scheduler) | ✅ (4/4) | ✅ (4/4; CPUID-based count, #30) |
| VirtIO GPU / input / block, **VFS + Ext4 (extents)**, GPT+MBR | ✅ | ✅ |
| Userland: ELF loader, IPC, windows, registry, fonts, DOOM | ✅ | ✅ |
| Coherent ABI (single numbering, negative errno) + first capability checks | ✅ | ✅ |
| **Fault isolation** (user crash never kills the kernel; symbolized backtraces) | ✅ | ✅ |

Remaining amd64 gaps are **understood and root-caused** (epic #94). **The aarch64
path is the reference of "correct".**

## 3. Purpose & guiding principles

1. **Isolation by capability (seL4-inspired).** Authority is conferred by
   unforgeable handles, not ambient identity. A process can only touch what it has
   been given a capability to. *The first permission layer (kill/focus/window/
   file-write/registry checks) landed with epic #93; unforgeable handles and
   sandboxing are still the single most important architectural change.*
2. **Everything is a file (Plan 9-inspired).** Resources — devices, services,
   the registry, IPC channels, windows — are named in a per-process **namespace**
   and accessed through a uniform `open/read/write/close`-style interface. *Today
   the registry is a flat key/value store and "fds" are overloaded integers.*
3. **Thin, efficient portability layer.** The arch/HAL boundary should be a small
   set of direct primitives (à la Plan 9's `port`/`pc`), not a heavyweight device
   abstraction invoked per register access. *Today the HAL synthesises a device
   struct on every MMIO read.*
4. **Services, not kernel features.** The compositor, font manager, VFS, network
   stack, and drivers (where feasible) are **userland servers** in their own
   address spaces, supervised and restartable. *Today they are in-kernel.*
5. **A coherent, versioned ABI.** One syscall numbering scheme, negative-errno
   semantics, a real handle table, stable struct layouts. *Numbering and errno landed
   (single `syscall_nums.h` compiled into both sides); the handle/fd table is the
   open item (ABI-03).*
6. **Honesty over marketing.** Documentation states verified vs. aspirational.
   (The legacy README's "Production-Ready" / "Military Grade" / MIT-license claims
   are inaccurate and will be corrected — the project is **GPLv2**.)

## 4. Target architecture **[goal]**

```
                 ┌──────────────────────────────────────────────────┐
  userland       │  shell   compositor   fontsrv   vfs/fs   netsrv   │  ← sandboxed
  (capabilities) │  apps    window-mgr   driversrv registry-as-ns    │    services
                 └───────▲───────────────────────────────────────────┘
                         │  IPC = read/write on capability-named channels (Plan 9 9P-like)
                 ┌───────┴───────────────────────────────────────────┐
  HybridKernel MicroKernel ispired    │  caps · scheduling · address spaces · IPC · IRQ    │  ← minimal TCB
                 └───────▲───────────────────────────────────────────┘
                 ┌───────┴───────────────────────────────────────────┐
  thin HAL       │  per-arch: MMU ops, traps, timer, MMIO/port, SMP   │
                 └───────────────────────────────────────────────────┘
       boot: a vetted, GPLv2-compatible loader (e.g. Limine/U-Boot) → kernel
```

## 5. Strategic gap (review → roadmap)

The review's findings cluster into the foundations this target needs, in
dependency order:

1. ~~**Define & document the PA/VA model + enforce W^X**~~ — **done 2026-06** (epic #92:
   higher-half kernel on both arches, single `KERNEL_VIRT_BASE` flip point, W^X, full
   teardown, TLB shootdown).
2. **A coherent, capability-checked ABI** (syscall table, errno, handle table) —
   **in progress** (epic #93: single numbering, negative errno and the first capability
   layer landed; fd/handle table, authenticated IPC and sandboxing remain).
3. **Complete the allocators** (buddy PMM + growable slab kmalloc; finish userland
   malloc) so long-lived services don't exhaust fixed pools. kmalloc now grows;
   multi-region PMM (holes!) and reclaim are open.
4. **Stabilise boot on both arches** via a real loader (GPLv2-compatible) — fixes
   the amd64 4GB/`make run` gap and prepares real hardware.
5. **Slim the HAL** to direct primitives; flesh out drivers/device-tree.
6. **Extract services to userland sandboxes** (VFS → compositor → fontsrv →
   shell), one at a time, behind the new ABI/IPC.
7. **Plan 9 namespace registry**; then **networking** and **real hardware**.

## 6. Scope & non-goals (for now)

- **In scope:** QEMU `virt`/`q35` as the reference platform; AArch64 as the
  correctness reference; a small, auditable trusted computing base.
- **Deferred:** broad real-hardware matrices, a POSIX-completeness guarantee,
  self-hosting toolchain, and the bundled DOOM port (kept as an integration demo,
  not maintained as first-party code).
- **Non-goal:** preserving the current in-kernel-service architecture; it is a
  scaffold to be dismantled, not an invariant.

## 7. Licensing

The project is **GNU GPL v2** (`LICENSE.md`). Any third-party code we integrate
(boot loader, libraries, drivers) **must be GPLv2-compatible** — e.g. Limine
(BSD-2) or U-Boot (GPLv2) for boot; **not** GRUB (GPLv3) as a *linked* component.
The legacy README's MIT statement is incorrect and will be fixed.
