# PLAN 2026-07-23 — Hardening, formal diagnostics, portable SDK, Rust core

Maintainer directive, 2026-07-23.  Seven programmes (A–E from the first
directive, **F–G added 2026-07-23 second directive**), executed in order, each
one task at a time: **study the surface → write the plan → apply it → test on
both architectures → refine → next**.  Both arches build and boot at every task
boundary; the maintainer drives `make run` interactively, this plan's own gate
is the headless boot plus the build.

**COMMIT POLICY (maintainer, 2026-07-23):** from the second directive onward,
NO commit without the maintainer's explicit authorization.  Work is staged and
verified; the maintainer authorises the commit.

Programmes F and G are the "make the kernel actually usable" directive:
on-disk PERSISTENCE with a real partition model + a first-boot INSTALLER
(Programme F), on top of REAL storage & device drivers (Programme G).  They
EXTEND existing in-tree plans rather than replacing them — `docs/MICROSCOPE-
RELEASE-STORAGE.md` (block contract done, tmpfs/xfs/memory-drivers open),
`docs/PIANO-DRIVER-MATURITY.md` (Fase 2 = runtime hotplug/plug-and-play, USB
stack partly landed), `docs/FUTURE_DRIVER_EXPANSION_PLAN.md` (NVMe/AHCI as block
providers, partition-table parser).  Method stays ASTRA: every device/format is
a provider behind a contract; `arch/` holds only ISA/boot glue.

This file is the task list.  It is updated IN PLACE as tasks complete, and any
defect or open point found while doing something else is added to §H rather than
fixed silently — unless it is a one-line fix, which is applied on the spot and
recorded in the same table.

Naming: defects use the project's existing internal ID convention
(`SUBSYS-NN`, e.g. `ABI-04`, `SCHED-05`, `UACC-AMD64-02`), NOT CVE numbers.
These are not published vulnerabilities and giving them CVE identifiers would
make them look like something they are not.  The catalog in §A is the
equivalent artefact: one row per defect, with severity, status and owning task.

---

## §0 — State  (updated 2026-07-24, HEAD `a636e27`)

What is actually done, for someone opening this file cold.  Corrected IN PLACE;
the per-programme sections carry the detail.  **This file is the only state
artefact** — no per-session handoff document, because two of them diverge and
the divergence is the defect this section exists to prevent.

**Confirmed work order (maintainer, 2026-07-24).**  The ALIGNMENT of programme
R closes BEFORE programme F resumes:

```
R1b → R2 → C1 → R3 → R4 → R5 → R6 → F
```

C1 is the UART/printk channel ONLY, not the rest of programme C, and it is
inserted before R3 deliberately: R3 moves 17 verbs through the compositor, and
entering it with `printk` still sharing the graphical shell's path loses the
diagnostic output exactly when the graphical shell is the thing that breaks.

**Landed since the 2026-07-23 handoff** (`dcec177` → `a636e27`):

| commit | what |
|---|---|
| `602cb2f` | nxui: one record per window — UI-DOCK-PID-01 (§A) |
| `8ebe1c1` | V0.0.5.4 |
| `a636e27` | five defects: `fgetc`/`fputc`/`fputs` took an `if (fd >= 0)` shortcut past the stream buffers (a `FILE` now holds a handle for its lifetime, so the shortcut started bypassing them); `sys_ipc_recv` rewound the PC through `current_process->context`, which is NOT the live trap frame; the reaper drained `msg_queue` without `msg_lock` (double `list_del` + double `kfree` → "Invalid magic"); amd64 now classifies a user-mode fault with no current task as scheduler-state corruption, as aarch64 already did; the amd64 link had one RWX `PT_LOAD` fusing pre-paging code with the boot page tables, and `.note.PVH` was `SHF_ALLOC` in no `PT_LOAD` at all.  Adds `user/bin/libctest.c`, 18 cases. |

**HAL-0b is HALF done.**  `a636e27` closed the IMAGE half (the amd64 link is now
split per permission).  The RUNTIME half is untouched: amd64 still identity-maps
all usable RAM in the low half besides the higher-half direct map — RAM mapped
twice, kernel alias in user space — and `AMMU-01` still leaves kernel RAM W+X
with no NX.  **So the charter's W^X claim is not upheld on amd64 today.**  That
is stated plainly because it is a DECLARED property that is not true, which is a
different and worse thing than a missing feature.  Closes inside HAL-0, before
E2 (Rust) depends on it.

**Verification gates.**  Both arches build AND boot, plus `captest` 64/64 AND
`libctest` 18/18.  `libctest` joins the gate from `a636e27` — its 18 cases are
named after the app each one protects, so a regression names its own victim.
Harness: `tools/nxrun.sh -a <arch> -n <runs> -c captest -c libctest -e captest=64
-e libctest=18`, in the tree since `922cc71`, where its gate was also made real
(it used to print the test counts without comparing them, so a run reporting
`captest 60/64` exited 0).

**Do NOT shorten the harness waits using the boot times.**  Measured 2026-07-24,
3 runs each: `"Entering supervisor loop"` appears at 0.60 s on amd64 and 0.67 s
on aarch64.  That line is the KERNEL reaching its loop — it is **not** the system
ready to accept input; the graphical services and the shell come afterwards, and
in practice ~24 s (amd64) and ~40 s (aarch64) are needed before `qmp_type.py`
produces reliable results.  Cutting the waits to those 0.6 s would reintroduce a
flakiness that reads as a guest fault when it is a tool artefact.  The number
worth having is time-to-first-shell-response, which is not what was measured
here.

Boot counts are THREE different numbers and conflating them is how a race gets
declared fixed while it is still there:

| purpose | runs | why |
|---|---|---|
| routine per-phase smoke test | 5 | cheap, catches a hard break |
| DETECT the intermittent SMP race | 20 per arch | at p=0.25, `0.75^20` ≈ 0.3% chance of missing it.  **5/5 passes 24% of the time with the bug fully present**, so 5 proves nothing about a race |
| DECLARE the race resolved | 59 clean | 95% confidence the failure rate is below 5% |

**MEASURED 2026-07-24 on HEAD `922cc71`: 20/20 amd64 and 20/20 aarch64 clean —
the intermittent SMP boot race did NOT reproduce.**
- Detector matches the documented signature: the race is recorded as an
  *intermittent kernel PANIC* in the K3-userland window
  (`PLAN-2026-07-17-STRATIFICATION.md:744`), and the harness greps
  `PANIC|PAGE FAULT|PROTECTION FAULT` plus heap corruption.  Each run was given
  a 20 s window after `"Entering supervisor loop"` — without a command the fault
  check fires instantly and a panic two seconds later is missed.  All 40 serial
  logs contain the full `ls` output, so every boot reached a userland shell that
  ANSWERED; "clean" is not "did not explode".
- `0.75^40` = `1.0e-05`, so **the "~1 in 4" characterisation is refuted.**  Worth
  recording where that figure came from: `PLAN-2026-07-17-STRATIFICATION.md:744`
  notes "3/4 headless boots clean" — **one failure in a sample of four**, on
  2026-07-17.  It was never a measured rate, which is why it survived a year of
  repetition unchallenged.
- **This is NOT "resolved".**  Zero failures in 40 boots bounds the rate at
  **7.2%** with 95% confidence (`1 - 0.05^(1/40)`); the under-5% claim needs 59
  clean, which is exactly where that number comes from.  Nothing was fixed — the
  honest statement is "not reproduced in 40 boots, rate below 7.2%".
- The `884b7f3` comparison was NOT run: the maintainer conditioned those 40
  extra boots on the race actually manifesting, and it did not.

**Retired syscall numbers are never reused.**  251/252/254 (R1) and, when R1b
lands, 259/260.  A stale binary must get `-ENOSYS` — a clean failure — instead
of landing on another verb's slot with its arguments misread.  `263`
(`SYS_STAT`) is assigned; new numbers continue upward from there.

---

## Programme A — Security & correctness audit  (IN PROGRESS)

Surface, in the order the maintainer named it.  Every file gets a verdict, and
every EXISTING open marker gets CONFIRMED / OBSOLETE / RECLASSIFIED — a stale
"known issue" is worse than an unknown one, because it consumes the attention
that would find the real one.

| task | surface | state |
|---|---|---|
| A1 | `kernel/core/syscall_dispatch.c` + both arch entry paths (`arch/aarch64/cpu/syscall.c`, `arch/amd64/cpu/syscall.c`), user stubs | done |
| A2 | `kernel/core/object.c` — handle table, capability acquisition, object ctl | done |
| A3 | `kernel/lib/registry.c` — key parsing, ACL, virtual routing | done |
| A4 | capabilities: `include/abi/caps.h`, level presets, `level_for_path`, creator clamp | done |
| A5 | `kernel/sched/process.c` — spawn, kill, subtree, stop/cont, wait, reap | done |
| A6 | windows: `compositor_*` capability surface + `OS1_NS_WIN` acquisition | done |
| A7 | `user/sys/bin/nxinit.c` + the supervised-service model | done |
| A8 | userland abstraction: `user/sys/lib/lib.c`, `include/api/*` | done |
| A9 | Verdict pass over all 108 pre-existing markers | done |

### A-catalog — findings
Severity: **S1** exploitable/crash from unprivileged userland · **S2** privilege
or isolation defect requiring some privilege · **S3** correctness/robustness ·
**S4** hygiene.

| id | sev | surface | finding | status |
|---|---|---|---|---|
| ABI-06 | S2 | process.c | `process_kill_subtree()` killed a MACHINE root's whole subtree while sparing the root | **FIXED** b7e5668 |
| GFX-COMP-RESERVE-02 | S1 | compositor.c | `compositor_lock` → `sched_lock` AB-BA + UAF on the returned `struct process *` | **FIXED** b7e5668 |
| ABI-08 | S1 | syscall_dispatch.c | `SYS_WAIT` arg1 written without the one-arg stub zeroing the register | **FIXED** b7e5668 |
| **GFX-WIN-WRITE-01** | **S1** | syscall_dispatch.c | `SYS_WINDOW_WRITE` gated only by CAP_WINDOW (the WEAKEST cap — guests hold it), no owner/ctty check: least-privileged process could write text into ANY window (id from the ungated SYS_WINDOW_ENUM). UI-spoofing against shell/dock/notify. | **FIXED** (this batch) — owner/ctty/machine check, mirrors DRAW/BLIT/SET_FLAGS |
| **PORTCAP-01** | **S2** | object.c | `sys_port_send_caps` rolled back installed handles only on install-loop failure; on `-EPIPE`/`-EAGAIN` (queue full) the transferred handles stayed in the receiver. `-EAGAIN` is retryable, so a peer keeping a service's port queue full leaks a fresh handle set into the service's table per retry → capability-table exhaustion DoS. | **FIXED** (this batch) — unwind on any `ret < 0` |
| **SPAWN-LVL-01** | **S3** | syscall_dispatch.c | `level_for_path()` prefix-matches the RAW path but the VFS resolves `..`, so `/sys/bin/../../home/x` takes the ROOT preset. Not an escalation (creator clamp drags it back to USER), but "safe only because a distant second check covers it". | **FIXED** (this batch) — reject `..` components in a spawn path |
| **CAP-POLICY-01** | **S2 (design)** | process.c `level_ceiling[]` | `PLVL_USER = CAP_ALL`. Every ordinary process holds **every** capability (SPAWN, FS_WRITE, IPC_ANY, WINDOW, REG_WRITE). The capability MECHANISM is enforced everywhere, but the POLICY is "everyone gets everything except guest", so least-privilege is not realised — this is the maintainer's "problem with capabilities". Also the root of USR-SEC-01 (any process may overwrite `srv.notify_pid`). | OPEN → B2 (needs per-app/per-service caps, ASTRA §7.11 Q5 — cannot be narrowed unilaterally without breaking every app) |
| PROC-REF-01 | S2 | object.c, process.c | a `struct process *` outlives the lock that validated it (`sys_cap_grant`, `sys_port_send_caps` `rcv`, `dispatch_spawn`'s `src` into `process_redirect_child_fd_from`) | **PARTIAL — 2 of 3 sites closed.**  The `dispatch_spawn` site is still OPEN (the fix was reverted in `bd3073d`; see B1/B1b and the 25-line account at `syscall_dispatch.c:266-291`) |
| **UI-DOCK-PID-01** | **S3** | nxui.c | The dock kept one window's fields in four PARALLEL arrays (`ids`/`flg`/`ttl`/`pids`), and the launcher-hoist loop that pins nxlauncher to tile 0 shifted only three of them — `pids[]` was never moved. Every tile before the launcher's original index therefore drew the icon of its NEIGHBOUR: at boot the compositor lists `[nxshell, nxlauncher]`, the hoist shifts the shell to index 1, and tile 1 resolves `sys.proc.<launcher_pid>.name` → the shell wore the launcher's icon. Only the icon was wrong because the title and the click target came from the arrays that *were* shifted. Reopening a shell put it after the launcher, needing no hoist — hence "close it and the icon is right". | **FIXED** — the four arrays collapsed into one `struct tile_win win[]`, so a reorder moves the whole record and no field can be left at its old index |

### A — verdicts on surface (no defect = why)
- **A1 dispatcher / arch entry / stubs**: user-pointer copy discipline is
  UNIFORM — every syscall taking a user pointer either copies through
  `arch_copy_*` or hands to a `sys_*` that does (mechanically verified, table
  above).  `argc`/`nredir`/`nfds` all bounded before any `n * sizeof` (no
  size-overflow).  Both arch entry paths mechanically checked stub-arity vs
  dispatcher-arg-use (done in the prior session); only `_sys_spawn`/`_sys_wait`
  needed the scratch-register zeroing, both done.  **GFX-WIN-WRITE-01** was the
  one real hole. Verdict: clean after this batch.
- **A2 object.c**: handle indices bounded at every entry (`< 0 || >= NPROC_HANDLES`);
  acquisition ACLs present per namespace; ctl owner/privilege checks present.
  **PORTCAP-01** the real hole; **PROC-REF-01** the lifetime one.
- **A3 registry.c**: writes gated by CAP_REG_WRITE; virtual keys routed before
  the ACL with their own per-process rule; strict copy for keys/values (no
  silent truncation).  `reg_proc_split` pid parse is unbounded (`*10+`) but the
  pid is only used as a table lookup that fails for a wrapped value → S4, noted
  §H.  Verdict: sound.
- **A4 capabilities**: the mechanism is correct and enforced; the POLICY is the
  finding (**CAP-POLICY-01**).  Creator clamp is monotone and correct.
- **A5 process.c**: spawn/kill/wait/reap reviewed with the prior session's
  fixes; **PROC-REF-01** the remaining lifetime gap.
- **A6 windows**: `OS1_NS_WIN` acquisition ACL correct (WRITE/DESTROY to
  another pid's window is machine/root only); dispatcher owner checks now
  complete after GFX-WIN-WRITE-01.
- **A7 nxinit**: supervised-service model reviewed in the prior session
  (backoff, port-claim retry).  USR-SEC-01 is a symptom of CAP-POLICY-01.
- **A8 userland abstraction**: declared-vs-implemented libc audit is EMPTY
  (prior session); POSIX layer is a mapping over `OS1_*`, no parallel impl.
- **A9 markers**: 108 project-code defect IDs catalogued; the audit CONFIRMED
  the security-relevant ones above and reclassified USR-SEC-01 as a symptom of
  CAP-POLICY-01.  The MM-*/ARCH-*/AMMU-*/BOOT-* families are correctness/hardening
  notes outside this audit's named surface — swept into Programme B backlog, not
  lost.

---

## Programme B — Resolve the catalog, one micro-phase per defect

Each defect gets its own micro-phase: study → fix → build both arches →
headless boot both arches → stage.  Ordered by severity, then by whether the
fix unblocks another.  Populated from §A; B1 is already known.

| task | defect | state |
|---|---|---|
| B1 | PROC-REF-01 | **PARTIAL — 2 of 3 sites.**  The approach was to hold `sched_lock` across lookup+use (it pins the pool) instead of adding a process-wide refcount; `sched_lock` is exposed in sched.h.  **Closed:** `sys_cap_grant` (also closes OBJ-GRANT-REAP) and `sys_port_send_caps` `rcv` — both are ALLOCATION-FREE under the lock **on purpose**: they refuse a target that has no handle table rather than creating one.  **Reverted (`bd3073d`), still open:** `dispatch_spawn`'s `src`.  `process_redirect_child_fd_from` ALLOCATES — `handles_ensure()` kmallocs the child's table and `kobj_free()` releases the displaced console handle — and allocating under `sched_lock` establishes the `sched_lock → pmm_lock/kmalloc_lock` order that `process.c` states in writing nothing else establishes.  With IRQs off, an idle core spinning inside the allocator while this CPU holds `sched_lock` is a hard hang: that is the amd64 K3-userland panic (corrupted RSP, execution off into the stack).  A rare UAF was accepted over a reproducible deadlock in the path every `system()` call takes.  The full account is `syscall_dispatch.c:266-291` — **read it before touching this site.**  Remaining work → B1b. |
| B1b | PROC-REF-01 residual — `dispatch_spawn`'s `src` | planned, and it is NOT "add a refcount".  **First, why the other two sites are already safe, because it is the asymmetry that matters and not an oversight:** `sys_cap_grant` and `sys_port_send_caps` are allocation-free under `sched_lock` **on purpose** — they REFUSE a target that has no handle table rather than creating one, which is legitimate there because a real target always has one.  `dispatch_spawn` could not be closed the same way: the child legitimately MAY need its table built, so refusing would break the operation rather than harden it.  That is the whole difference between the third site and the other two.  The way out is to move the ALLOCATION instead of the lock: (1) hoist `handles_ensure(p)` OUT of the critical section — the child is `p`, created by this same dispatch and racing with nobody, and `object.c:1710` notes its table is normally already built by `install_stdio`; (2) under `sched_lock`, look up `src` and REFUSE a source with no handle table instead of creating one — exactly what makes the other two sites safe (`syscall_dispatch.c:283-286`); (3) the dup runs under `object_lock` nested inside `sched_lock`, the documented `sched → object → kmalloc` order; (4) `to_free` is returned to the caller and released after BOTH locks drop.  Requires splitting `process_redirect_child_fd_from` into an allocation-free core returning `to_free` plus a wrapper that frees it — the `to_free` pattern `object.c` already uses in that same function.  No new mechanism, no termination path touched. |
| B2 | CAP-POLICY-01 — reframed after the maintainer's correction: this is NOT per-app bitmasks, it is the **4-LEVEL abstraction** (machine/root/user/guest) that must stratify.  The mask half AND the per-namespace ACL half together.  Split into B2.0–B2.4 below. | in progress |

### B2 — the level model, corrected understanding
The maintainer corrected two approximations: (1) apps reason in the 4 LEVELS
(machine/root/user/guest), mapped over caps, not raw bits — the POSIX
abstraction sits on that; (2) that mapping exists at KERNEL level
(`struct process.level`, `proc_is_privileged`/`proc_is_machine`,
`caps_for_level` (was `level_ceiling`), `level_for_path`, `registry_caller_owner`),
mirrored in userland by `nxperm.h`.  The stated target semantics:
- **machine** — full authority (it IS the check).
- **root** — full, EXCEPT writing `/sys/bin` and `/system` (VFS ACL, not mask).
- **user** — restructured: home moves `/home` → `/mnt/usr1/home` (per-user
  partition prep), per-service manifest presets LATER.
- **guest** — windows only (the one level whose MASK is genuinely narrow).
- **The POSIX abstraction must work over our VFS.**

| task | item | state |
|---|---|---|
| B2.0 | Loss analysis: is the mask/ACL regressed vs the reference or a release? | **DONE** — nothing lost.  `level_ceiling[PLVL_USER]` = CAP_ALL since the model's first commit `24fab00`; VFS write-ACL byte-identical across `884b7f3` and every release tag; `/system` and `/mnt/usr` never existed.  The stratification is a FORWARD upgrade, not a repair. |
| USR-SEC-01 | system-owned registry key writable by anyone (owner-0 hole) | **DONE** `bf92c76` — deny is now "a non-system caller writes only a key it owns", matching registry_del which was already correct. |
| B2.1 | separate the level→mask policy from the scheduler into caps | **DONE** `d93b6fc` — `caps_for_level()` in `include/abi/caps.h`, kernel + nxperm derive from it, drift bug removed. |
| B2.2 | `/system` machine-only in the VFS write-ACL (root refused, like /sys/bin) | **DONE** `5b8b77d` — path guarded before it is populated (harmless; nothing writes it). |
| B2.3 | user home `/home` → `/mnt/usr1/home` | **BLOCKED on a disk-layout decision** — see below.  84 hardcoded `/home` references across userland (icons, shell cd, nxexec `~`, history, image paths); rootfs is a single ext4 partition with no `/mnt`; the VFS supports multiple mounts but roots a single one; no symlink support.  This is the maintainer's "per-user partition = future block".  Options to decide: (a) separate mounted partition at `/mnt/usr1` vs (b) a directory in the same rootfs; and how the 84 hardcodes migrate (consult `HOME` everywhere vs keep `/home` as a compat alias). |
| B2.4 | per-service capability manifest/preset (ASTRA §7.11 Q5) | pending — after B2.3.  Mechanism: a per-path/per-app cap mask layered over `caps_for_level`, default-permissive, tightened one service at a time. |

---

## Programme N — Contract gates: make C fail on what it cannot check  (LANDED, ongoing)

Opened 2026-07-24 on the maintainer's directive: *everything we routinely
forget must block compilation*.  Not a new programme in the ASTRA sense — it is
the enforcement layer under all of them, and every other programme inherits it.

**The problem it addresses.**  Every defect this kernel has lost days to
belonged to a class C is blind to: a return value nobody looked at, a struct
whose layout assembly depends on, a lock held across an allocation, a page
freed while a register spilled onto it was still live.  Each was written into a
comment after it was found, and each was violated again — because a comment
cannot fail a build.

### N1 — the vocabulary  (`kernel/include/kernel/nx_contract.h`)

Two enforcers reading ONE set of annotations, so a second list cannot drift
from the first:

| macro | mechanism | catches |
|---|---|---|
| `NX_MUST_USE` | `warn_unused_result` | an `-errno` return nobody acted on |
| `NX_NONNULL(...)` | `nonnull` | a NULL where the contract forbids one |
| `NX_ASSERT_OFFSET` / `NX_ASSERT_SIZE` | `_Static_assert` | a layout assembly or the other arch depends on |
| `NX_DISCARD(expr, why)` | typed sink | a deliberate drop, WITH a written reason |
| `NX_NO_ALLOC` / `NX_ALLOCATES` / `NX_HOLDS` | checker-read | the interprocedural rules |

**Always on.**  There is deliberately no switch.  An earlier version was gated
behind `NX_STRICT` and the result was that nobody saw a single diagnostic: a
gate you have to remember to enable is a gate that is off.  The Makefile
carries a note stating the three legitimate responses (handle it, surface it
with the CONSEQUENCE named, or `NX_DISCARD` with a reason) and the three that
are not (delete the annotation, `-Wno-unused-result`, or a `(void)` cast —
which does not suppress the attribute under GCC and only makes the omission
look intentional).

`NX_DISCARD` exists because a must-use gate without an escape hatch does not
get satisfied, it gets DELETED by the first person who meets a legitimately
ignorable result.  It is deliberately more typing than handling the error.

### N2 — layouts pinned

There was **not one** `_Static_assert` in the tree.  `struct pt_regs` is built
BY HAND in assembly and read as a C struct by the dispatcher — two statements
of one layout with nothing connecting them.  Now pinned on both arches;
verified by negative control (inserting a field fails the build naming the
moved offsets, removing it returns to zero).  aarch64 is the worse case:
anything before `qregs[]` shifts 512 bytes of NEON state addressed by constant
in the save/restore stubs.

### N3 — the interprocedural rules  (`make lockcheck`)

RULE A (nothing allocates under a `sched_lock`) and RULE B (nothing goes back
to the PMM while the running context still depends on it), both TRANSITIVE —
the call that broke rule A contained no allocator name at all.  27 hits; needs
a triage pass before it can gate `all`.

### N4 — what the gate found (first pass, both arches build + boot clean)

| site | defect |
|---|---|
| `arch/{amd64,aarch64}/mm/mmu.c` | `arch_vmm_map_device` looped over a BAR ignoring every failure and returned 0 — **the same defect on BOTH arches**, which is exactly the divergence class this gate exists for |
| `arch/amd64/mm/mmu.c` | the MMIO window and the low-2MB SMP-trampoline identity map dropped failures; a partial map surfaced later as a driver fault or as secondary CPUs that never came online |
| `arch/aarch64/cpu/cpu.c` | the one range carrying GIC + PL011 + all 32 VirtIO-MMIO slots dropped its failure: no interrupt controller, no console, no devices, reported as silence |
| `mm/vmm.c` | `vmm_unmap_page` ignored the unmap result while its own comment promises "the caller may safely recycle the backing frame" — a still-mapped page handed back to the PMM |
| `sched/elf.c` | a short segment read left a page holding whatever the frame contained, and the process executed it |
| `core/syscall_dispatch.c`, `core/object.c` | `arch_copy_to_user` dropped in `SYS_WAIT` and `OBJ_CTL` — a bad user pointer reported a successful wait whose status never arrived.  Both carried a `(void)` cast that suppressed nothing |
| `sched/process.c` | `sys_getprocs` returned a count after a failed copy; `kill_subtree` could not tell a survivor from a process never in the kill set |
| `drivers/timer/pic_pit.c` | **the halt-IPI handler had never been registered**: `pic_init()` runs from two composition points, the second gets `-EBUSY`, and with the result dropped nobody knew.  A kernel fault on one core left the others running |
| `graphics/compositor.c` | resize and pointer notifications dropped — the app kept drawing at its old size, or stopped receiving input, with nothing in the log |
| `fs/procfs.c`, `lib/registry.c` | a failed mount left `/proc` or `/reg` simply absent while consumers reported "no such file" |
| `mm/buffer.c` | writeback cleared the DIRTY bit after a FAILED write, asserting the block matched the disk and never retrying it |
| `include/kernel/test.h` | `KASSERT` was a bare `if`: `if (c) KASSERT(x); else y;` bound the else to the macro's own if |

**Open, deliberately not decided here:** `pic_init()` is driven from BOTH
`arch/amd64/hal.c` and `arch/amd64/platform/platform.c`.  ASTRA wants exactly
one composition root.  Suppressing the second call outright was tried and
BROKE THE BOOT — the second call does work the first does not — so which
caller should own the wiring is a maintainer decision, recorded rather than
guessed.

### N5 — remaining

`NX_MUST_USE` beyond the kernel headers (userland `include/api/*`), the timer
rule (`timer_del` must precede the free of the object a timer points at),
`#pragma GCC poison` for arch/transport leakage into generic code, the
`lockcheck` triage, and real `ktest` coverage — today its 13 cases live inside
the framework's own files and test string/math/gfx contracts, not one kernel
invariant.

---

## Programme C — Formal diagnostics

Goal: make any function in the kernel accurately and readably diagnosable, and
make unlikely logical/semantic errors *catchable* rather than merely possible.

- **C1 — `printk` audit.** `kernel/lib/printk.c` (LIB-PRINTK-01,
  LIB-VSNPRINTF-02 open here): levels, ratelimiting, per-subsystem filtering,
  and whether the UART path is safe from IRQ and from a spinlock-held context.
- **C2 — the UART channel is separate from the graphical shell.** Establish it
  formally: a diagnostic channel that survives a dead compositor, with its own
  contract, not "printk happens to reach the serial port".
- **C3 — two error systems and the POSIX link.** kernel `pr_err`/panic/fault
  path vs userland `OS1_report_error` → notify severities vs POSIX `errno`.
  Today these meet ad hoc.  One documented mapping, one seam each direction.
- **C4 — assertion & invariant layer.** A `KASSERT`-class facility with the
  context the fault path already knows how to symbolise, so an invariant break
  reports WHERE and WHAT, not just a fault address.
- **C5 — structured subsystem tracing** gated per subsystem, off by default,
  so a diagnosis is a flag rather than a rebuild with temporary `pr_info`s
  (the plan records that pattern being used repeatedly).

## Programme D — Portable SDK, dynamic linking, kernel/userland separation

The end state: userland binaries do not statically carry the libc, the kernel's
API and the userland's API are separate artefacts, and apps are built against
an SDK rather than against the kernel tree.

- **D1 — finish Phase 10a first.** `lib.c` still `#include`s three kernel `.c`
  sources (USR-LIB-01).  Dynamic linking cannot decouple what is compiled in.
- **D2 — ELF dynamic loading in the kernel loader**: `PT_DYNAMIC`, `PT_INTERP`,
  relocations, a runtime loader process.
- **D3 — `libos1.so`**: the libc as a shared object, one copy in memory.
- **D4 — the SDK tree**: headers + link stubs + a sysroot, versioned, with
  nothing from `kernel/` reachable.  The layering gate (already in tree)
  becomes the SDK's admission test.
- **D5 — port the userland to the SDK**, app by app, keeping static linking
  available until the last one moves.

## Programme E — Rust in the kernel core

- **E1 — toolchain**: replace/augment the GCC cross toolchains; pick the Rust
  target triples (`aarch64-unknown-none-softfloat`, `x86_64-unknown-none`),
  decide on `core`/`alloc` and build integration with the existing Makefile.
- **E2 — kernel Rust semantics through the HAL.** Before any module is
  rewritten: panic handler, allocator shim onto `kmalloc`, the spinlock and
  IRQ-state primitives, `unsafe` boundaries for MMIO, and the calling
  convention on both arches.  This is HAL-0's successor and depends on it —
  two architectures that implement the same primitive differently cannot be
  given one Rust abstraction.
- **E3 — a first module**, chosen for being self-contained and verifiable
  rather than for being important.
- **E4 — the delicate modules**, one at a time, each with the C version kept
  until the Rust one passes the same tests.

---

## Programme F — On-disk persistence, partition model, first-boot installer

> Maintainer directive 2026-07-23 (second).  This is the programme that makes
> the system **actually usable**: persistent on real disk, installed once, then
> booting as a named user rather than as root.

### F0 — VERIFIED CURRENT STATE (real files, no assumptions)
Everything below was read out of the tree, not inferred:

| fact | evidence |
|---|---|
| The block layer is already a CONTRACT with one active backend | `kernel/drivers/block/block.c` — `block_register()`, `block_read/write()` route to `static const struct block_dev *active` |
| virtio-blk registers, then **ramdisk OVERRIDES it if a boot module exists** | `kernel/main.c:236-240` — `virtio_blk_init(); ramdisk_init();` |
| ramdisk writes are **volatile by construction** | `kernel/drivers/block/ramdisk.c` — `memcpy(disk, buf, len); /* RAM-backed: writes are volatile */` |
| ext4 writes reach the backend, so **persistence already works on the virtio-blk path** | `kernel/fs/ext4.c:90,252` → `block_write()`; `make run` attaches `disk.img` as virtio-blk |
| The ISO/release path is the RAM one ("loaded next to the kernel") | `Makefile:840` `module2 /boot/disk.img diskimg`; module reserved as `MEM_REGION_RESERVED` before PMM (`kernel/main.c:199-211`) |
| `mkdisk` builds a **single** GPT partition, hand-rolled single-group ext4 | `tools/mkdisk.c` — `MIN_PARTITION_BLOCKS (432 MiB)`, ~1014-inode cap, `plan_partition_blocks()` |
| The VFS supports MULTIPLE mounts but roots exactly one | `kernel/fs/vfs.c:54-57` mount table, `vfs_mount_at`/`vfs_umount`; "Single root mount" |
| No symlink / bind-mount support | no `VFS_TYPE_LINK`, no bind in `vfs.c` |
| RAM discovery is already an arch-HAL contract | `arch_platform_get_mem_regions()` — aarch64 FDT + manual probe, amd64 MB2 MMAP |
| Storage drivers present: **virtio-blk and ramdisk only** | `kernel/drivers/block/`, `kernel/drivers/virtio/virtio_blk.c`; **no AHCI/NVMe/SATA/SCSI anywhere** |
| 84 hardcoded `/home` references in project userland | measured, `grep '"/home'` |

**Conclusion:** the block CONTRACT and ext4 write-back already exist (MICROSCOPE
R1 landed).  What is missing is the PARTITION MODEL, the RAM-copy + authorised
write-back persistence policy, the installer, and real disk drivers.

### F-target — the architecture the maintainer specified
```
DISK (after install)
 ├── P1 KERNEL   immutable on disk; never writable at runtime
 ├── P2 ROOT "/" writable by ROOT only
 ├── P3 MACHINE  machine-only paths (the 4th partition the maintainer asked for,
 │               so "/" can be root-writable without exposing machine state)
 └── P4 USR1     /mnt/usr1 — per-user; more users = more partitions

BOOT
 kernel loaded from P1 → detects RAM → assigns ~20% of RAM to "/"
 → system image mounted there → nxinit from /sys/bin (unchanged)
 → kernel + root are COPIES IN RAM; the disk copies stay authoritative

PERSISTENCE (tied to the syscalls)
 userland write → VFS → path is authorised & disk-backed?
   → operation completes in the RAM view
   → the changed file/section is handed to the DISK DRIVER, which persists it
```
Setup modes: **full** (a disk exists → choose disk, choose the partition sizes)
and **RAM-only** (test setup: home initialised in RAM from detected free space).

### F-phases

**Phase state:** F1 **DONE** `986858c` · F2 **DONE** `7418dca` · F3 **DONE**
`7418dca` · F4–F10 planned, and they resume only after programme R closes (§0).
What F2/F3 actually landed: six NEXS role GUIDs minted in `tools/mkdisk.c:97-103`
(`NEXS_META`, `NEXS_KEYSTORE`, `KERNEL_A`, `KERNEL_B`, `ROOT`, `MACHINE`, `USR`
— note `KERNEL_A`/`KERNEL_B` are the D14 A/B slots and `USR` is P4+, plural by
design per D15), mount by ROLE instead of by index (retires GPT-02), and one
SHA-256 implementation shared by `tools/mkdisk.c` and `kernel/lib/sha256.c` —
one algorithm, not two transcriptions of a spec, so a mismatch is a real
mismatch.  **The partition table is therefore not a blank sheet:** later phases
consume these roles, they do not re-mint them.

- **F1 — DESIGN DOC FIRST (`docs/DESIGN-PERSISTENCE-INSTALL.md`).**  R6 rule
  ("doc dedicato prima del codice") applies: this changes the boot contract, the
  disk format and the write path at once.  The doc fixes: partition table
  layout + GUIDs, which trees live on which partition, the RAM-copy semantics
  (what is copied, when, what stays disk-authoritative), the write-back
  contract (who decides a write is persistable, at what granularity — file vs
  section — and the failure/ordering semantics), and the install/first-boot
  state machine.  **Nothing in F2+ starts before this is agreed.**
- **F2 — `mkdisk` multi-partition.**  Generalise the hand-rolled GPT+ext4 writer
  from one partition to N (it currently writes exactly one; the single-group
  ext4 and the ~1014-inode cap are per-partition limits that must be sized per
  role).  Emit P1..P4.  Keep a single-partition mode so today's `make run` stays
  byte-identical until F5 flips it.
- **F3 — kernel: mount the partition set.**  Use the existing multi-mount table
  (`vfs_mount_at`) to mount ROOT, MACHINE and USR1; extend the GPT probe to
  recognise the roles.  Extend `vfs_write_allowed()` so the tree ACL and the
  PARTITION agree (a machine-only path must also be on the machine partition —
  today the ACL is path-string-only, B2.2).
- **F4 — DECISIONS TAKEN (maintainer, 2026-07-24), to be folded into
  `DESIGN-PERSISTENCE-INSTALL.md` as numbered decisions when F4 starts — recorded
  here now so they are not lost, and NOT minted into the agreed design doc
  unilaterally:**
  - **The journal format is NEXS-proprietary, not JBD2.**  The ext4 in tree is
    hand-rolled; a partial JBD2 is worse than none because it promises Linux an
    interoperability that does not hold, and interoperability is not a declared
    goal of this project.
  - **The journal lives in P0 (`NEXS_META`), not in a partition of its own.**
    P0 already holds the Merkle roots, and D6' commits the data change and the
    hash change in ONE transaction: putting them in two partitions means two
    writes to distant regions that must be atomic with respect to each other —
    i.e. re-creating the exact problem the journal exists to solve.
  - **Data journaling for the integrity-protected partitions (P1/P2/P3);
    metadata-only for P4+ (per-user).**  The 2× write cost is paid where
    integrity is bought.  Reason it cannot be avoided on P2: if the data is not
    journaled, a crash leaves the hash and the data divergent with no way to
    know which is right, and a Merkle tree that "repairs" itself by recomputing
    the hash FROM the data verifies nothing at all — it certifies whatever is on
    the disk, including what an attacker put there.  That would forfeit the
    property F4c exists for.
  - **Boot-time Merkle verification stays ON** (maintainer: "non mi interessa
    quanti secondi di boot perdiamo").  The policy is decided and a measurement
    does not re-open it.  A build flag may disable it with two constraints:
    **default ON**, and a release image must not be able to boot with
    verification off.  A flag that can be left off by accident is a security
    defect, not a development convenience.
  - **Its cost is NOT MEASURED, and that is a statement about the code, not a
    task nobody did.**  `kernel/lib/sha256.c` arrived with F2/F3 (`7418dca`) but
    has **zero callers** — verified: `grep -rn sha256 kernel/ --include=*.c`
    outside the file itself returns nothing, and there is no `merkle`/`digest`
    caller either.  Nothing hashes anything at boot today, so there is no
    verification to time; the number would have to come from a throwaway kernel
    patch measuring code that does not exist yet.  **Maintainer's decision
    (2026-07-24): defer the number to F4c**, where the verification is actually
    implemented.  What IS known and measured: P2 is 110592 blocks × 4096 =
    432 MiB (`mkdisk.c:25,29`; `build/<arch>/disk.img` is 453019136 bytes) to
    re-read and hash per boot, and on aarch64 that is software SHA-256 on a
    TCG-emulated CPU.  **No estimate is recorded here in place of the
    measurement** — when F4c lands, the number comes from a command.
  - **F4 needs its design doc before code** (with G3): the criterion is that a
    wrong on-disk format is already in users' images, whereas a wrong object verb
    is changed in the next commit.
- **F4 — persistence write-back.**  The RAM-copy + authorised-write-back path,
  tied to the syscall boundary as the maintainer specified.  Depends on the
  memory work MICROSCOPE R4 (RAM-disk + tmpfs + PMM zones share one accounting
  path) and interacts with the buffer cache (`kernel/mm/buffer.c`).
  **OPEN INVESTIGATION (maintainer flagged "attento a … su amd64"):** on amd64
  the kernel shares the address space with the current process and identity-maps
  usable RAM (`kernel/arch/amd64/mm/mmu.c:116`), so carving a RAM partition at
  runtime must coordinate with PMM regions and the identity map; aarch64 splits
  TTBR0/TTBR1 and does not.  This asymmetry must be settled in F1's doc, and it
  is the same class as HAL-0's uaccess divergence.
- **F5 — `nxdisk` service.**  Partitioning, formatting, mounting — a supervised
  service behind a port (`OS1nx_disk`, per the `OS1nx_<service>` standard), so
  the installer is a CLIENT and the privileged disk work is one auditable place.
- **F6 — `nxcomp` service.**  Compression/decompression as a service
  (`OS1nx_comp`).  Format decision: prefer a small, permissively-licensed,
  self-contained decompressor that is GPLv2-compatible and needs no allocator
  heroics — candidates to evaluate in F1: **zlib/DEFLATE** (zip), **miniz**
  (single-file, MIT), **LZ4** (BSD, trivial decoder), **zstd** (BSD).  Selection
  criteria: decoder size, no dynamic allocation requirement, license
  compatibility with GPLv2, and whether we need seekable/streaming.  Used to
  ship the `usr` tree compressed inside `disk.img` and expand it at install.
- **MULTI-USER IS A REQUIREMENT, not a later extension** (maintainer: "il sistema
  deve essere in grado di gestire multiutenti, un solo utente era per esempio").
  F7 and F9 are designed multi-user FROM THE START — `TYPE_NEXS_USR`
  (`mkdisk.c:103`) is already minted as "P4+ per-user", plural, and D15 forbids
  naming `usr1` as a constant.  A default installation that creates one user is
  not the same thing as a system that supports one.
- **The release ISO is a HOTLOADER, not a disk.**  It is not writable, and
  `nxsetup` must EXCLUDE it from the list of installable targets — needed as soon
  as F5 (`nxdisk`) enumerates devices.  It also stays the regression canary that
  the block contract remained backend-agnostic.
- **F7 — `nxsetup` installer (first boot, runs ONCE).**  Chooses the username,
  sizes the partitions (or RAM-only mode), asks `nxdisk` to partition/format,
  asks `nxcomp` to expand `usr`, seeds the user's environment
  (`sys.env.HOME` → `/mnt/usr1/home`, per the B2.3 decision "everything via
  sys.env"), and copies the boot chain (bootloader + kernel, shipped compressed
  in `disk.img` like the ISO carries them) onto P1 so the machine becomes
  self-booting.  Guarded by a "already installed" marker so it never runs twice.
- **F8 — `nxauth` = `su`.**  Called from the shell; switches to another user or
  to root.  Bounded by the maintainer's earlier decision (nxauth v1 = root with
  a preset password); this phase widens it to named users once F7 creates them.
  Interacts with the LEVEL model (B2) — a user session runs at PLVL_USER with
  its own home partition.
- **F9 — default user migration.**  `user/home` → `user/usr1`; the shell opens
  as `usr<name>` with its partition mounted, not as root.  **This SUPERSEDES
  B2.3a/b/c** — the home move is now a sub-step of the installer work rather
  than a standalone migration, but the two decisions already taken stand:
  separate ext4 partition mounted at `/mnt/usr1`, and all 84 hardcoded paths
  resolved through `sys.env`.
- **F10 — `make` integration.**  Compress the `usr` tree into `disk.img` at
  build time; `disk.img` gains the boot chain (bootloader+kernel) so it can
  install like the ISO; re-check `make release` and the tmpfs story
  (MICROSCOPE R2, still open) — the release ISO currently boots RAM-volatile.

---

## Programme G — Real storage & device drivers (both arches, unified HAL)

> Prerequisite for F on real hardware: today the only block backends are
> virtio-blk and a RAM disk.  Extends `docs/FUTURE_DRIVER_EXPANSION_PLAN.md` §4
> (storage controllers as block providers) and `docs/PIANO-DRIVER-MATURITY.md`
> (Fase 2 = runtime plug-and-play), both already ASTRA-shaped.

- **G1 — AHCI/SATA block provider.**  HDD/SSD over the existing `block_dev`
  contract; PCI discovery already exists (`kernel/drivers/pci/pci.c`
  `pci_enumerate`).  No FS changes: it registers like virtio-blk.
- **G2 — NVMe block provider.**  The de-facto SSD standard (PCIe); same
  contract.  FUTURE_DRIVER_EXPANSION §4 already names the target controllers.
- **G3 — partition-table parser completion.**  GPT exists (`kernel/fs/gpt.c`,
  with GPT-01/02/03 markers open); F2/F3 need role-aware partition
  identification, and multi-disk selection for the installer.
- **G4 — USB completion + device tree.**  The stack is present (`xhci.c`,
  `ehci.c`, `uhci.c`, `usb_hid.c`, `usb_core.c`) and HID works on xHCI/EHCI;
  what is missing is enumeration completeness and the device-tree/ACPI
  description path.  `kernel/drivers/usb/xhci.c:14` carries an
  `ASTRA-VIOLATION` (calls `arch_vmm_map_device()` directly) that this phase
  must clear.
- **G5 — runtime plug-and-play, unified under the HAL.**  `usb_core.c:354`
  records "Runtime hotplug / re-scan is Fase 2"; the HAL device registry is
  immutable/lockless after SMP bring-up.  Make it mutable at runtime with
  recognition + dispatch, hotplug events HCD → HAL → IPC to userland, the SAME
  mechanism on aarch64 and amd64.  This is `PIANO-DRIVER-MATURITY` Fase 2 —
  adopt that plan, do not re-derive it.

**Ordering note:** F can be developed and verified entirely on virtio-blk (the
contract is backend-agnostic), so F does not block on G.  G is what makes F work
on real hardware.  G5 and F4 both touch the HAL and should not be interleaved.

---

## Programme R — ASTRA surface reduction: everything through the object layer

> Maintainer, 2026-07-23: *"guarda tutte le syscall p9, dobbiamo adattarle e
> adattare il vfs, tutto deve passare dal layer object in maniera analoga,
> riduciamo tutte le superfici come da ASTRA"* — and: correct ALL the
> implementations already built, not only the recent work; this vision is now
> part of the plan permanently.

**The rule.** ASTRA §6.2/§6.3/§6.6: every resource is a node in a namespace,
every node is representable as a file, every operation is a message on a
capability-bearing handle.  A kernel verb that reaches a resource WITHOUT going
through the object layer is a second implementation of something that already
exists — it doubles the audit surface, and the two copies drift (this is the
same defect class as the `level_ceiling` mirror B2.1 removed, and the three
registry paths below).

**Libc is unaffected as an API.**  POSIX names stay exactly where they are —
that is the personality layer the project already mandates ("POSIX vive SOPRA
os1").  What changes is WHO implements them underneath: composition over the
object primitives instead of a private kernel verb.

### R0 — MEASURED census (2026-07-23, real files)

| surface | object layer (ASTRA) | parallel path |
|---|---|---|
| **Files** | `open()` — **496** userland call sites | `file_read` 49, `file_write` 19, `list_dir` 20, `OS1_fs_unlink` 20, `OS1_fs_write` 17, `OS1_fs_read` 8, `OS1_fs_list` 2 → **~135 sites** over `SYS_FILE_READ/WRITE`, `SYS_LIST_DIR`, `SYS_MKDIR`, `SYS_UNLINK` |
| **Registry** | `OS1_NS_REG` + `OBJ_TYPE_REGKEY` — **0 userland users** | `SYS_REGISTRY` — 14 uses in lib.c |
| **Registry (again)** | `/reg` regfs mount — the p9 namespace, ASTRA §7.6 marked DONE | so the SAME data has **three** access paths |
| **Windows** | 8 `OBJ_CTL_*` verbs on `OBJ_TYPE_WINDOW` | **17** ad-hoc `SYS_WINDOW_*`/display syscalls |
| **IPC** | `OBJ_TYPE_PORT` (capability-addressed) | 12 uses of ambient pid `SYS_SEND/RECV/TRY_RECV` |
| **Process** | `OBJ_TYPE_PROCESS` + `OBJ_CTL_KILL/STOP/CONT`, `SYS_OBJECT_WAIT` | `SYS_KILL`, `SYS_WAIT` |

Two findings worth stating plainly:
- **The object layer has already won on files** (496 vs ~135).  The reduction is
  realistic, not aspirational.
- **The capability check is NOT the gap.**  `vfs_write_allowed()` is already the
  single write-authority seam and both paths call it.  So this programme is
  about ONE IMPLEMENTATION, not about closing a hole — which is why it can be
  done without a security window opening mid-migration.

### R-phases (each: build + headless boot on BOTH arches before the next)

- **R1 — files.  DONE** `10641b6`, regressions fixed in `d8bb115`.  Deleted the
  path-based FS verbs from the kernel and composed them in libc over
  `open`/`lseek`/`read`/`write`/`close`.  `file_read(path,…)` is
  open+seek+read+close — a userland composition, not a syscall.  Removed
  `SYS_FILE_READ`, `SYS_FILE_WRITE`, `SYS_LIST_DIR` (251/252/254, retired and
  NOT reused).  Directory listing works because a directory is a file you read.
  `SYS_STAT` (263) was added afterwards to restore the file-vs-directory
  distinction the retirement broke.
  **The lesson this phase paid for, which applies to R3–R6:** when a syscall is
  retired, find every consumer of its IMPLICIT INVARIANTS, not just its callers.
  Both regressions were unwritten invariants — `stat()` and `opendir()` both
  relied on "the list primitive succeeds ONLY on a directory".
- **R1b — namespace mutation as an object verb.  NEXT.**  `SYS_MKDIR` (260) and
  `SYS_UNLINK` (259) are the last path-addressed FS verbs: namespace MUTATIONS
  with no object equivalent.  Decided form (maintainer, 2026-07-24), p9-faithful:
  the verb acts on a handle to the PARENT DIRECTORY.
  - **A distinct acquisition mode, not a new capability bit.**  `PLVL_USER` is
    `CAP_ALL` today (CAP-POLICY-01), so a new `CAP_*` would be granted to
    everyone and enforce nothing while adding ABI surface.  Root-vs-user is
    enforced by the per-namespace ACLs (the VFS write tree, registry owner), not
    by the coarse mask.  A per-service narrowing belongs to B2.4, where the
    manifests give it meaning.
  - **`-EISDIR` on WRITE stays exactly as it is** (`object.c:328-338`, where the
    refusal is deliberate and commented).  Creating or removing a node in a
    directory is a DIFFERENT operation from writing that directory as a byte
    stream, and merging the two is how ACLs get lost.  The relaxation is targeted
    and must be verified against the case it unblocks — precedent: READ on a
    directory was refused by the same function, which made the directory-read
    path unreachable and produced `ls: cannot list .`.
  - **Recursive removal stays a userland composition.**  A recursive kernel verb
    is unbounded work under a lock and a single syscall that can destroy a
    subtree; ASTRA excludes both.
  - `nxdisk` (F5) and R6 need this verb, so it is a prerequisite of programme F,
    not only of R.
- **R2 — registry: one truth.  PARTIAL** — `af39c90` made the KEY decide its
  authority instead of the door the caller came through, so the three paths
  agree ON AUTHORITY.  The COLLAPSE itself is NOT done: all three still exist.
  **REG-ERRNO-01 (found 2026-07-24) is the proof that this is not cosmetic —
  the two doors ALREADY diverge, on the error they report:**
  - `registry_del` returns `-1`, `vret`, `-ENOENT`, `-EACCES`, `0`;
    `registry_set` returns `-1`, `vret`, `-1`, `-EACCES`, `0`.  Mostly
    `0/-errno` with bare `-1`s left in — i.e. `-EPERM` in disguise, the same
    defect EXT4-ERRNO-01 fixes in ext4, still live here.
  - `registry_set` reports **OOM as `-1`** (the in-tree comment says "empty key
    or OOM" on that very return).  `ENOMEM` is one of the five conditions that
    raise the RED notification in `OS1_report_error`, so a registry allocation
    failure is filed as an amber policy denial — the most serious of the five,
    mislabelled.
  - **`regfs` is asymmetric within itself:** `regfs_unlink` returns
    `registry_del(...)` verbatim (propagates), while `regfs_create` does
    `registry_set(...) == 0 ? 0 : -1` (flattens).  So a REAL `-EACCES` from
    `registry_set` reaches userland as `-EPERM` through `/reg` and as `-EACCES`
    through `SYS_REGISTRY`.  Same event, same tree, two answers, today.

  Deliberately NOT fixed inside EXT4-ERRNO-01 — that step stays small.  It is
  the case R2 should be judged against: collapsing the three paths is what makes
  a divergence like this impossible rather than merely absent.  `/reg` is
  already a mounted namespace, so
  `open("/reg/…")` IS the object path.  Collapse the three onto it:
  `SYS_REGISTRY` becomes a compatibility shim (or goes), `OBJ_TYPE_REGKEY` has
  zero users and is a deletion candidate.  **Constraint:** the virtual
  `sys.proc.<pid>.env.*` routing lives inside `SYS_REGISTRY`'s path
  (`reg_virtual_proc_write`), and `getenv/setenv` depend on it — that routing
  must move with the data, not be lost.
- **R3 — windows.**  17 ad-hoc verbs onto the `OBJ_TYPE_WINDOW` object that
  already exists with 8 `OBJ_CTL_*` verbs.  Largest numeric reduction, highest
  risk (compositor + the ACL work from GFX-WIN-WRITE-01), so it goes after the
  two safe ones — and after **C1**, so a compositor that breaks does not take
  the diagnostic channel with it.
  **Net for this phase: `captest` AND `libctest` before and after EVERY
  sub-step, not only at the phase boundary** — the tests say more than the boots
  do, and this is the surface where an AB-BA lock inversion (GFX-COMP-RESERVE-02)
  has already landed once.  20 boots per arch on top, per §0.
- **R4 — IPC.**  Ambient pid `SYS_SEND/RECV/TRY_RECV` → ports.  The daemon
  design doc already states ambient pid IPC is the seL4 rule ports repair, and
  Phase 16 already owns the unbounded-queue DoS on the ambient path — so this
  phase converges with work already scheduled.
- **R5 — process.**  `SYS_KILL`/`SYS_WAIT` → `OBJ_TYPE_PROCESS` verbs.  Note
  `SYS_WAIT` must keep working for a REAPED pid (Phase 9b): a capability cannot
  be acquired for a dead process, so the object path alone cannot express it —
  the retained-status lookup has to be part of the design, not discovered
  afterwards.
- **R6 — the VFS itself.**  Mounts through the namespace: `vfs_mount_at()` is
  the existing p9 seam (used by `/reg`, `/proc`, ktest) and the F-programme's
  role partitions (MACHINE→`/system`, USR→`/mnt/usr1`) must mount through it
  rather than growing new special cases.  The ROOT mount stays bootstrap-special
  by necessity — you cannot mount `/` through a namespace that does not exist
  yet — and that exception is written down here so it is not mistaken for drift.

**Ordering rationale:** R1 and R2 remove implementations without changing
authority; R3–R5 change how authority is NAMED and therefore need the object
model to already be the only file/registry path.  R6 is small and unblocks the
F-programme's partition set.

---

## §H — Open points found while doing other work

Anything noticed in passing lands here immediately.  A one-line fix may be
applied on the spot, but it is still recorded.

| id | where | note | disposition |
|---|---|---|---|
| PROC-REF-01 | object.c, process.c | see §A | B1 |
| CAP-POLICY-01 | process.c level_ceiling | PLVL_USER = CAP_ALL — see §A | B2 |
| REG-PID-PARSE-01 | registry.c `reg_proc_split` | unbounded `pid = pid*10+d`, can wrap negative; harmless (lookup fails) | S4, fix with B-family registry pass |
| CPU-AMD64-01 | arch/amd64/cpu | FPU/SSE save-restore landed-and-reverted; matters before amd64-heavy work; also blocks E2 (Rust needs a settled FP/SSE context contract) | Programme B / E2 |
| UACC-AMD64-02/03/04 | arch/amd64/mm/uaccess.c | amd64 uaccess has no lock vs concurrent unmap (aarch64 does); documented TOCTOU | HAL-0 (pre-existing phase) → gates E2 |
| **MM-KHEAP-01** | kheap / sched | After `stress` exits, kheap grows to a plateau and ctxsw stays DEGRADED.  The informative detail: ctxsw **descends** rather than rising — each switch costs more, there are not more of them.  **Do not attribute this to the double-`kfree` fixed in `a636e27`**: that was corruption, not a leak, and reaching for a closed cause is how a real one stays open.  **The 2026-07-24 ctxsw numbers in PERF-AMD64-01 say NOTHING about this item** — they were taken at IDLE, and this defect is defined by what happens *after* a load ends ("alla sua chiusura sembra che il sistema sia ancora sotto stress").  Written here so nobody reads "ctxsw measured, fine" and treats both as covered. | open.  **The measurement that characterises it is three-point on ONE boot:** ctxsw + kheap at idle → prolonged `stress` → ctxsw + kheap after it exits.  `nxmemstat --log <iv> --run /bin/stress` exists for precisely this, and `tools/nxrun.sh` can now drive it.  It also produces the loaded-vs-idle number PERF-AMD64-01 needs to reconcile its 350 baseline, so the two are one measurement.  NOT ahead of the §0 work order. |
| **HAL-0b** (runtime half) | arch/amd64/mm | image half closed in `a636e27`; the runtime identity map (RAM mapped twice, kernel alias in user space) and missing NX are untouched, so the charter's W^X claim is not upheld on amd64 | HAL-0, before E2 |
| **PERF-AMD64-01** | — | "amd64 feels slower than aarch64 despite TCG" — a SENTENCE, never measured, never reproduced.  The actual ctxsw collapse had three concrete causes, all found and fixed: a 16 MiB `kmalloc` per directory listing (the buffer was sized on `OBJ_MAX_IO_BYTES` instead of on the caller's request), a double page-table walk in the string uaccess (the standalone pre-check repeated the `i==0` iteration), and `OBJ_CTL_STAT` re-running a full `vfs_stat()` after `handle_create` had already resolved the path.  Maintainer confirmed "ctxsw tornato ok". | **MEASURED 2026-07-24, NOT CLOSED — and the reason it cannot close is the point.**  Idle context switches on HEAD `922cc71` via `nxmemstat --log 2`: **amd64 213.9 switch/s** (default accel), **234.0** under `-accel hvf -cpu host`, **208.3** under explicit `-accel tcg`; **aarch64 167.0** (TCG is the only accelerator its QEMU offers).  `HZ` is one shared constant (100, `drivers/timer.h:18`), so the two arches are comparable on that axis.  **What this does establish:** amd64 is not slower than aarch64 on this metric — it is HIGHER — so the sentence that opened this row is not reproduced.  **What it does NOT establish, and why closing here would have repeated the "1 in 4" mistake:** the healthy baseline reported by the maintainer is ~350 switch/s (the collapse was 350→70, the recovery "ctxsw tornato ok"), with ~130/150 after stress.  **None of those numbers exists anywhere in the tree** — `grep -rn 350 docs/` finds nothing near ctxsw, and the "perf brief" that `sysstats.h` cites is not a file under `docs/`.  So 213.9 would be judged against a baseline with no recorded provenance and no recorded measurement conditions.  One clean explanation was checked and **excluded by measurement**: `tools/run-stress.sh:14` documents the campaign ran amd64 with `-accel hvf -cpu host` while `nxrun.sh` passes no accelerator at all, but HVF idle (234.0) vs TCG idle (208.3) is ~12%, not the ~40% needed to reach 350.  **The likeliest remaining explanation is that 350 was measured under LOAD, not at idle** — the campaign drives `/bin/stress` — and an idle rate and a loaded rate are simply different quantities.  **What settles it:** the three-point measurement on ONE boot — idle → prolonged `stress` → after it exits — which is exactly what `nxmemstat --log <iv> --run /bin/stress` was built for.  That measurement belongs with **MM-KHEAP-01** and is done there, not here. |
| **ABI-FILE-01** | user/sys/lib | `struct FILE` grew ~4.3 KB → ~8.4 KB (the read buffer).  Harmless ONLY because everything is statically linked and `make` rebuilds `disk.img` wholesale.  The moment `libos1.so` is shared and apps ship separately, `struct FILE` is public, versioned ABI. | **gate for programme D** — decide versioning before D3 |
| **TOOL-VERIFY-01** | tools/ | `tools/nxrun.sh` and `tools/qmp_keys.py` are UNTRACKED; `nxrun.sh` is the repeated-boot harness the §0 gates depend on.  Its verdict currently reports `captest`/`libctest` results but does not FAIL on them — only faults, heap corruption and no-boot set the exit status, so a run with `captest 60/64` still prints `ok`. | track them, and make test failure set the exit status, before they gate a phase |
| **REG-ERRNO-01** | registry.c / regfs | mixed `-1` / `-errno` conventions in `registry_set`/`registry_del`; OOM reported as `-1` (so `ENOMEM` is classified as an amber policy denial); and `regfs_unlink` propagates the provider errno while `regfs_create` flattens it — the `/reg` door and the `SYS_REGISTRY` door already give DIFFERENT answers for the same refusal.  Full detail in §R under R2. | **R2** — do not fix inside EXT4-ERRNO-01 |
| **capreg** `vfs-unlink-/reg`, `vfs-write-/reg` | user/bin/capreg.c:101,116 | Both FAIL on `7ac519b` + EXT4-ERRNO-01.  **NOT attributable to EXT4-ERRNO-01, proved by enumerating the modified sites:** the change touches exactly three — `vfs_unlink` (`!mnt`→`-ENOENT`, absent op→`-ENOSYS`), `vfs_create` (identical), and libc `mkdir()`.  `regfs` provides BOTH `.unlink` and `.create` (`registry.c:858`), so neither new branch is reachable on `/reg`; ext4 is not on the `/reg` path at all; and `capreg` never calls `mkdir()`.  Every branch goes non-zero→non-zero or 0→0.  Also structurally safe: `capreg.c:111`'s `== -EACCES` assertion rides `vfs_write_allowed` in the DISPATCHER (`syscall_dispatch.c:1127`), which returns before `vfs_unlink` is reached, and `OS1_fs_unlink` is a raw syscall with no `errno_ret` translation. | open, unattributed — likely REG-ERRNO-01 territory; one build on `7ac519b` would settle provenance empirically |
| — | tools/nxrun.sh | The fault detector greps `PANIC\|PAGE FAULT\|PROTECTION FAULT`, which matches amd64's **expected** `[FAULT] PAGE FAULT: … — terminating` from `/bin/crash` but not aarch64's `[FAULT] Data Abort (Lower EL): … — terminating`.  So running `crash` under the harness reports FAULT on one arch and ok on the other, for identical correct behaviour.  The right discriminator is an ORDERLY handled fault (`[FAULT] … — terminating`) versus `PANIC`.  Fix before the fault tests use the harness as a gate. | with the deferred fault tests |
| — | ext4 / stdio | **The 4 KiB block-granularity trap, which cost two regressions.**  ext4 fetches — and for a write read-modify-writes — a WHOLE 4 KiB block for any partial access.  So unbuffered byte-at-a-time I/O touches one 4 KiB block PER BYTE: that, not the syscall count, is what made doom's savegame take minutes.  `FILE` therefore buffers BOTH directions at 4096 (= one block), and the direct fd path in `fread`/`fwrite` is for console and pipe streams ONLY (`path[0] == '\0'`).  **Never route a file stream around those buffers** — `a636e27` had to fix `fgetc`/`fputc`/`fputs` for doing exactly that. | standing note |
| — | verification | `writetest` is OBSOLETE and must not be used as a verification tool.  `user/bin/base-nexs` is a submodule outside the build whose reference `lib.c` still calls the retired `_sys_file_read`: **do not delete it** — it is the canary proving R1's ABI break has consumers outside the tree, and making it link is the first test of dynamic linking in programme D. | standing note |
