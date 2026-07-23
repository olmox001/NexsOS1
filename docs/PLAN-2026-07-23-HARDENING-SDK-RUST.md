# PLAN 2026-07-23 — Hardening, formal diagnostics, portable SDK, Rust core

Maintainer directive, 2026-07-23.  Five programmes, executed in order, each one
task at a time: **study the surface → write the plan → apply it → test on both
architectures → refine → next**.  Both arches build and boot at every task
boundary; the maintainer drives `make run` interactively, this plan's own gate
is the headless boot plus the build.

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
| PROC-REF-01 | S2 | object.c, process.c | a `struct process *` outlives the lock that validated it (`sys_port_send_caps` `rcv`, `process_redirect_child_fd_from`) | OPEN → B1 |

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
| B1 | PROC-REF-01 | **DONE** — resolved by holding sched_lock across lookup+use instead of adding a process-wide refcount.  `sched_lock` exposed in sched.h (it pins the pool); three sites fixed: `sys_cap_grant` (also closes OBJ-GRANT-REAP), `sys_port_send_caps` rcv, `dispatch_spawn` src.  Order sched→object→kmalloc; handle-table allocation refused under sched_lock (a real target always has one).  Both arches boot clean. |
| B2 | CAP-POLICY-01 — per-app / per-service capability sets so PLVL_USER stops meaning CAP_ALL; folds in USR-SEC-01 and ASTRA §7.11 Q5.  Large; likely wants an app manifest + a nxexec-assigned cap set.  Do NOT narrow universally — stage behind a default-permissive flag and tighten per service. | pending |

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
