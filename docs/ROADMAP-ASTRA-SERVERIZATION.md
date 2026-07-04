# NEXS — ASTRA Serverization Roadmap

> **Purpose.** A single, phase-ordered technical plan that takes NEXS from its
> current hybrid kernel to the ASTRA end-state: **every kernel service is a
> transparent, capability-addressed process** over a uniform object/registry
> namespace, with a plug-and-play modular driver model. It complements — does
> not replace — `docs/ASTRA.md` (the target model), the `docs/direction/DIR-*`
> series (per-axis direction), `docs/PROCESS-KILL-MODEL.md`,
> `docs/FUTURE_DRIVER_EXPANSION_PLAN.md` and `docs/PIANO-DRIVER-MATURITY.md`.
>
> **Governing principle (the ASTRA synthesis).** Plan 9 (*everything is a
> name in a namespace*) + seL4 (*capabilities are the only authority; rights
> are attenuable and unforgeable*) + Fuchsia/NT (*every object reached through
> a uniform handle interface; services are userspace processes*). Every
> function becomes **transparent** (its authority and target are explicit) and
> **tied to a process** (a supervised, restartable server), and every call is
> **standardized** under the naming scheme in Phase 4.
>
> **Non-negotiable methodology (applies to every phase).**
> 1. **State = code.** Verify against the tree, not commit messages or docs.
> 2. **No hole left open.** Every phase closes the bad/obsolete implementations
>    it touches; migrations retire the old path in the same phase (shim
>    elimination), they do not accrete a second parallel path.
> 3. **Capabilities / object model / registry are never an afterthought.**
>    Every new surface is expressed in the object+capability model from day one.
> 4. **Two-arch parity through the HAL.** No `#ifdef ARCH_*` in core; per-arch
>    logic lives behind an `arch_*`/HAL contract (DIR-06).
> 5. **Every change builds `-Werror` on both arches and boots headless with 0
>    PANIC before it is considered done; graphical `make run` on both arches +
>    maintainer confirmation before any push** (`nexs-commit-rule`).

---

## Phase 0 — Where we are (2026-07-04, verified)

Landed and in-tree:

- **Capability/object layer** (ASTRA §7.1): refcounted `kobject`, per-process
  unforgeable handle table, attenuable rights; `OBJ_TYPE_FILE/PROCESS/REGKEY/
  WINDOW/CONSOLE`; fd table folded into the handle table (`kernel/fd.h` gone).
- **Registry as a `/reg` namespace tree** + `/proc` typed objects (§7.6); the
  three registry entry points share one authority seam (`registry_write_allowed`
  / `registry_caller_owner`, S-ALIGN F5); the three VFS write-class entry points
  share `vfs_write_allowed` (F6); one scanout accessor over `gpu_ops` (F7).
- **Boot model** made explicit: K1/K2/K3 phases, K3 gate (PID1 only after
  K1+K2), kernel-alone on K3 failure, boot-phase stamped in panics (F9). Shared
  SMP bring-up primitive `smp_bringup_secondary` (F8, first slice of B6).
- **nxexec** = real execution service ELF (launcher spawn path, #193), with the
  windowed-vs-terminal debounce.
- **Cooperative kernel-thread reschedule primitive** `arch_cpu_yield` (HAL) +
  `kthread_create`/`kthread_block` — validated for kernel↔kernel switches; the
  foundation for the input server thread and for `OS1low_wait_irq` (Phase C).
- **Input decoupled from the hardware IRQ** (bottom-half): the virtio/PS2 input
  IRQs only enqueue into a ring; dispatch runs in `input_drain()` from the CPU0
  timer tick — the input IRQs no longer take `compositor_lock` or mutate the
  window-list (#68/#194 for those paths). **Active and stable on both arches.**

Known-open, precisely isolated:

- `arch_cpu_yield`'s cooperative switch **to a freshly-woken USER task** stalls
  CPU0 (the input server *thread* is therefore staged, not launched; the tick
  bottom-half is the active decoupling). Root-caused to the hand-rolled
  epilogue's user-return path, NOT the dispatch (Phase 1.A).

---

## Phase 1 — Stable preemption & the kthread substrate

**Goal.** A hardened, uniform context-switch/block/wake substrate that every
later server thread stands on. Nothing else serverizes until this is bulletproof.

### 1.A — Harden `arch_cpu_yield` and unify with the trap epilogue

Route the cooperative yield through the **proven trap epilogue** instead of a
hand-rolled one, then fix the review findings. Concretely:

1. **Extract a common `restore_context` routine** on each arch and reach it by
   `jmp` from BOTH the IRQ/exception path (`common_isr_entry` / `irq_stub`) and
   `arch_cpu_yield`. The `pt_regs` layout then lives in exactly one place
   (removes the duplication the review flagged). This alone likely fixes the
   user-task-return stall, because the yield reuses the exact, validated restore.
2. **amd64 correctness (review):**
   - `cld` on entry to `common_isr_entry`/`arch_cpu_yield` — never inherit a
     set DF into `rep`-using C (memcpy/memmove). Linux always does this.
   - Assert/guarantee **16-byte `%rsp` alignment** before every `call` into C
     (SysV ABI; SSE faults otherwise). Add a build-time `.if` or a runtime
     `test $0xF,%rsp` debug guard.
   - `HAS_ERR` macro is defined but unused — remove or wire it.
   - `swapgs`/GS: the `testb $3,24(%rsp)` before/after is correct only without
     nested entries. Add **NMI-safe GS handling** (a paranoid-entry that reads
     `MSR_GS_BASE` sign or uses `swapgs`-if-needed) before enabling NMI use.
   - Document `cli` in `arch_cpu_yield`: it is safe only when called with IRQs
     already masked (`kthread_block` guarantees this) — assert it in debug.
3. **FPU/SIMD state.** Today only GP regs are saved on amd64. This is safe ONLY
   while the kernel provably does not clobber user XMM (CPU-AMD64-01/#167 was
   reverted). Decide and implement one model **for the whole context-switch
   surface** (IRQ, syscall, yield): either (a) kernel is `-mno-sse`/no autovec
   and never touches XMM in kernel C, enforced by a build flag, or (b) lazy-FPU
   (CR0.TS + #NM handler) or eager save in the common `restore_context`. aarch64
   already saves q0–q31 uniformly; amd64 must reach parity. **Close #167 here.**
4. **IF on resume.** The synthetic frame uses `push $0x202` (IF=1). Correct now;
   when kthreads later use `spin_lock_irqsave` across a block, ensure the
   saved/restored flags are the *task's* flags, not a hardcoded constant, once
   kthreads run with IRQs conditionally masked.

**Acceptance:** the input server *thread* (below) runs; a soak of block/wake
with real user tasks being woken (shell typing under load) is panic-free on
both arches.

### 1.B — Activate the input server thread

Move `input_drain` into `input_thread_entry` (already written); call
`input_server_start()` from `init_scheduler`; `input_report` enqueues + wakes.
Retire the tick-drain call. This removes input dispatch from IRQ context
**entirely** (the DIR-02/DIR-03 goal). Keep the ring; the tick-drain code path
is deleted (no parallel path).

### 1.C — Scheduler/IPC correctness (blocking-substrate bugs)

These are load-bearing for every server thread and must land in Phase 1:

- **`PROC_CREATED` orphan leak.** If a process is killed while `PROC_CREATED`
  (mid-spawn on another CPU), `process_terminate` sets `kill_pending` and defers
  to the creator's `process_finalize_spawn`/`process_abort_spawn`. **If the
  creator dies/panics before finalizing, that pool slot is leaked forever**
  (not in any runqueue/wait-queue, no finalizer). Fix: when a process is
  terminated, sweep for `PROC_CREATED` children it owns and finalize/release
  them (extend `__reparent_children` to cover half-built spawns), OR a bounded
  watchdog on `PROC_CREATED` age. Tag `SCHED-CREATED-LEAK-01`.
- **`kernel_ipc_send` unbounded queue = kernel-heap DoS (SCHED-05 extended).**
  Each send `kmalloc`s an `ipc_node` *before* any per-target quota check, so a
  sender with `CAP_IPC_ANY` (or to its own parent, always allowed) can exhaust
  the **global kernel heap**, not just the receiver. Fix (capability-consistent
  flow control): a per-process `pending_msgs` counter with a cap (e.g. 256);
  reject with `-EAGAIN` at the sender above the cap. This is also the
  Registry-IPC flow-control gap. Tag `IPC-FLOW-01`.
- **SCHED-05 AB-BA** (`sched_lock → msg_lock → cpu->sched_lock`): re-audit once
  server threads increase `kernel_ipc_send` concurrency; fold into Phase 2's
  lock work if a real inversion is proven.

### 1.D — Secondary (do not skip)

- `wait()` non-blocking (SCHED-06): give it a real blocking form via the same
  wait-queue/`kthread_block` substrate (a process waiting on a child blocks
  instead of spinning).
- `MAX_PROCESSES`/`MAX_WINDOWS` hard caps → resource-derived quotas (#122).
- Per-CPU idle already lean (F8); document the kthread lifecycle in
  `docs/PROCESS-KILL-MODEL.md` so server threads inherit the kill/reap rules.

---

## Phase 2 — Spinlock & CPU optimization (kill "every core blocks the others", #194)

**Goal.** Remove the global-lock serialization that makes NEXS effectively
single-cored under load and widens the race windows behind the UTM crashes.

Instrument first (measure, don't guess): per-lock contention counters
(acquire-spin cycles) exposed via `/proc` or `OS1_sys_stats`.

Targets, worst-first:

1. **`compositor_lock`** — held for the whole composite (blit + `virtio_gpu_send`)
   in syscall context; input now off it (Phase 0). Split: render from a
   **snapshot** (copy the window list + damage under a short lock, composite
   lock-free), so the long GPU submit never holds the lock. Prereq for the
   compositor becoming a server (Phase 3).
2. **`sched_lock` (global)** — creation/kill/wait/reap all funnel through it.
   Move to per-runqueue ownership where possible; the reap/pool operations that
   genuinely need a global view get a dedicated finer lock or RCU-style
   deferral. Preserve the SCHED-UAF invariants (documented in `process.c`).
3. **`object_lock` (global, all handle tables)** — split to **per-process**
   handle-table locks; the global lock only guards the object free-list.
4. **registry rwlock** — already reader/writer; move to per-subtree locking as
   the namespace grows.

**Cleanup mandate:** each lock split deletes the coarse lock's now-dead callers
and any "we take X then Y" comments that no longer hold. Correlate with the
frame-corruption crashes (#189/#190/#191): shorter critical sections shrink the
race windows.

---

## Phase 3 — Serverize the kernel components (compositor first, then drivers/services)

**Goal.** Each subsystem becomes a **supervised server thread** (kernel-side
first, then a userspace ELF in Phase C) reached only through its object +
capability, over IPC. This is the ASTRA §6.4 SRL landing and the "every service
is a transparent process" directive.

Order (least-boot-critical first, per ASTRA §4 migration rule):

1. **Compositor / Window Server** (after Phase 2's snapshot split). It already
   owns *policy* in `nxui`; move the *mechanism* onto a server thread that owns
   the window list and consumes an IPC/command queue (`OS1_window_*` verbs →
   messages). Kernel drawing calls become messages to this server. Retire direct
   cross-subsystem calls into the compositor.
2. **Notification, registry, audio (when it exists), clipboard** — the SRL set
   `libos1` consumes (§6.4). Registry-as-service is evaluated in #162.
3. **Input server** — already a kernel thread (Phase 1.B); promote to the same
   server contract; later a userspace `input.elf`.
4. **Block/GPU/net drivers** — Phase C, driver-as-ELF (see Phase 6).

Each server: a `struct` describing its command verbs, a per-server capability
(Phase 5), a supervised lifecycle (respawn via `nxrespawn`/init), and a
**source-tree home under `kernel/srl/`** — establishing the explicit SRL/HAL
split ASTRA §6.4/B5 still lacks. **Move, don't copy**; delete the old in-core
location.

**Cross-cut:** as each subsystem serverizes, its ad-hoc global state and any
non-object entry points are deleted — this is the "constant closure of bad
implementations" mandate. No subsystem may keep both a direct-call path and a
message path.

---

## Phase 4 — Nomenclature unification (the ASTRA call-surface refactor, DIR-01/#164)

**Goal.** Every call is standardized and its layer is legible from its name.
Refine the existing `OS1_`/`OS1low_` split into a full scheme and *apply it*
(not just rename — unify each verb onto the object+capability model):

| Prefix | Layer | Who provides it | Examples |
|---|---|---|---|
| `OS1low_` | stable low-level ABI (near-immutable, direct kernel entry) | kernel | `OS1low_handle_create`, `OS1low_ipc_send`, `OS1low_vm_map`, `OS1low_wait_irq` |
| `OS1_` | high-level API (versionable) | libos1 / SRL servers | `OS1_window_create`, `OS1_registry_get`, `OS1_notify_post` |
| `OS1driver_` | driver↔kernel service contract | HAL/driver core | `OS1driver_map_mmio`, `OS1driver_dma_alloc`, `OS1driver_irq_claim`, `OS1driver_register` |
| `OS1libkernel_` | in-kernel library surface (allocators, strings, lists, registry helpers) reachable uniformly | kernel lib | `OS1libkernel_kmalloc`, `OS1libkernel_registry_get` |
| `OS1arch_` | the HAL/ISA contract (the only per-arch seam) | arch layer | `OS1arch_cpu_yield`, `OS1arch_tlb_flush`, `OS1arch_timer_count`, `OS1arch_switch_context` |

Rules:
- Rename **behind the seam** (`user/sys/lib/lib.c` keeps the bare-name shims
  during a transition, then they are deleted — shim elimination, one family per
  pass, build+boot 2 arch each).
- `OS1arch_` replaces today's mixed `arch_*`/`arch_impl_*`/`hal_*` — one HAL
  vocabulary. `arch_cpu_yield` → `OS1arch_cpu_yield`, etc.
- Every renamed verb is simultaneously **routed through the object/capability
  model** if it was still ambient (e.g. `OS1_fs_write`'s ambient path pending
  `O_CREAT`, `user/sys/lib/lib.c` `NOTE(M4.5-FS-WRITE)`; `OBJ_TYPE_PORT` for IPC;
  `OS1low_vm_map/_unmap/_protect` which do not exist yet).
- Each pass **deletes** the old names once no caller remains (no dual surface).

---

## Phase 5 — Capability extension & unification (incl. kernel level)

**Goal.** One capability model, applied uniformly from userland down into the
kernel's own inter-service calls.

- **Family-per-verb caps** (DIR-04 §2): replace the 5 coarse bits
  (`CAP_SPAWN/FS_WRITE/IPC_ANY/WINDOW/REG_WRITE`) with `proc_*/fs_*/window_*/
  input_*/net_*/...` families (`CAP_PROC_KILL` distinct from spawn;
  `CAP_WINDOW_CREATE` vs `CAP_DISPLAY_MODE` — closes U-3: today `CAP_WINDOW` is
  in every level incl. GUEST so it restricts nothing). Unify the parallel
  `OS1_RIGHT_*` object rights with the `CAP_*` process caps into one lattice.
- **Per-service capability refinement** (§7.2, still flat ROOT preset): each
  `/sys/bin/*` service gets a **reduced mask** (a `services` category), not the
  blanket ROOT — the compositor server needs no `CAP_SPAWN`, nxntfy no
  `CAP_FS_WRITE`, etc. Drive from a per-service manifest in the registry.
- **Kernel-internal capabilities.** As subsystems become servers (Phase 3),
  their *inter-service* calls carry capabilities too (a driver server holds a
  `CAP_MMIO`/`CAP_IRQ`/`CAP_DMA` handle to exactly its device window — seL4
  wiring). No ambient kernel authority between servers.
- **`OS1driver_`/hardware caps** (§6.1): `OS1low_map_mmio`, `OS1low_wait_irq`,
  `OS1low_dma_alloc` become capability-gated (a driver asks for
  `CAP_MMIO|CAP_IRQ|CAP_DMA` handles and never learns who provides them).
- Extend `nxperm` from introspection to the full model (login/su/UAC) — its own
  future phase, but the mask vocabulary lands here.

---

## Phase 6 — Plug-and-play & modular driver model (Phase C, ASTRA §4)

**Goal.** Hardware support = a **new provider implementing an existing
contract**, discovered dynamically, run as an isolated supervised ELF — the
`docs/FUTURE_DRIVER_EXPANSION_PLAN.md` end-state.

- **Discovery is data, not code:** ACPI/MADT (amd64, closes B4/#94 — no provider
  exists today) and FDT (aarch64) feed the same `hal_bus` device registry;
  PCI/VirtIO/MMIO enumeration binds drivers to devices by ID at runtime. No
  board/CPU model names in code (the plan's core rule).
- **`OS1low_map_mmio` / `OS1low_wait_irq` / `OS1low_dma_alloc`** (Phase 5 caps)
  are the four primitives a driver ELF needs; `wait_irq` is the blocking IRQ
  delivery built on Phase 1's `kthread_block`/`arch_cpu_yield` substrate — *the
  same primitive*, now surfaced to userspace.
- **Migration order:** a non-boot-critical driver first (input/net/audio),
  `blk.elf` last (rootfs depends on it → needs an in-kernel fallback or
  initramfs). Each migrated driver is **deleted from the kernel** once the ELF
  runs (no in-kernel + ELF duplication).
- **libos1 skeleton (#163)** exporting `OS1_`/`OS1low_` is the prerequisite for
  Phase D (musl) — Phase D does not start without it.

---

## Cross-cutting track — Stability, crashes, and obsolete-code closure

Runs in parallel; **the maintainer feeds crashes laterally** (input-interaction,
sporadic, not stress-reproducible — do not burn time re-reproducing):

- **Pitfall C** (`PROCESS-KILL-MODEL.md §4`): rapid-terminate SMP double-free —
  the input decoupling (Phase 0) removed the shell-accumulation trigger; the
  residual (rapid window-close) is Phase 2 lock work + the `__dequeue_task`
  UAF audit.
- **UTM frame-corruption crashes** (#189/#190/#191): correlated to lock
  contention (Phase 2) + IRQ-context mutation (Phase 1/3). Boot-phase stamping
  (F9) makes future transcripts diagnostic.
- **Standing obsolete-code closure** (do continuously, never batch at the end):
  stray files (`compositor.c.old/.c.new`, `registry.h.new`), dead helpers,
  stale comments, the `SYS_FLUSH`/dual-path remnants, `init.cfg` never read
  (S-13), `nxfilem` rework (in progress). Every phase deletes what it obsoletes.
- **DIR-05 remaining**: DWARF `file:line` backtrace, kernel recovery-mode
  (quiesce a subsystem instead of `panic()`) — natural once subsystems are
  restartable servers (Phase 3).
- **Security backlog** (#151–#161): kmalloc UAF/krealloc, UTF-8 OOB, vsnprintf,
  backtrace overflow, atoi overflow — fold into the relevant phase's files.

---

## Phase ordering (dependency graph, summary)

```
P1 (preemption/kthread substrate + sched/IPC bugs)
   └─> P1.B input thread   └─> P2 (lock optimization)
                                   └─> P3 (serverize: compositor -> SRL -> drivers)
                                          ├─> P4 (nomenclature, applied per subsystem as it serverizes)
                                          ├─> P5 (capabilities, per service as it serverizes)
                                          └─> P6 (plug-and-play driver ELFs; needs P5 hw-caps + P1 wait_irq)
Cross-cutting stability/cleanup runs through all phases.
Phase D (musl) / E (userland rebuild) follow P6 (per ASTRA §4a/4b).
```

**Definition of done for the whole roadmap:** the kernel keeps only ISA +
infrastructure providers + core + the four `OS1low_` hardware primitives; every
other subsystem is a supervised, capability-addressed server; every call is
named per Phase 4 and gated per Phase 5; a new device is a discovered provider,
not a code change. No subsystem retains a pre-ASTRA parallel path.
