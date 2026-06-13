# B3 closing plan — kernel sandboxing primitive (USR-SEC-03 #79)

> **Purpose**: self-contained handoff to finish Phase B microphase **B3**
> (epic #93). Batches 1–5 have landed; the only open member of #93 is
> **USR-SEC-03 #79 (no sandboxing)**. This document specifies the last two
> batches — a *kernel-level* capability primitive — and draws the line
> between what B3 delivers and what belongs to the B5 isolation epic (#95).
>
> **Status (2026-06-13)**: PROPOSED, not started. Stop point agreed with
> maintainer; implementation begins on confirmation of the three open
> decisions in §6.

---

## 1. Where B3 stands

Epic #93 ("Coherent ABI & capability-based access control") member status:

| Finding | What | State |
|---|---|---|
| ABI-01 #88 | single syscall numbering (`syscall_nums.h`) | ✅ batch 1 (`0bef4c3`) |
| ABI-02 #89 | negative-errno model | ✅ batch 1 |
| ABI-SYS-01 #75 | numbers enforced kernel↔userland | ✅ batch 1 |
| ABI-04 #91 | capability checks on kill/focus/window/file | ✅ batch 2 (`11f642a`) |
| EXT4-02 #57 | FILE_WRITE access control (/bin,/sys) | ✅ batch 2 |
| LIB-REG-02 #73, USR-SEC-01 #77 | registry ownership / routing | ✅ batch 2 |
| USR-SEC-02 #78 | send/kill accept arbitrary PIDs | ✅ batch 2 + 5 |
| ABI-03 #90 | per-process fd table | ✅ batch 3 (`f9a0b09`) |
| SCHED-DOS-01 #122 | anti fork-bomb quotas | ◑ batch 4 (`51c3179`) — per-window/IPC quotas open |
| SCHED-DOS-02 #122 | orphan reparenting + descendant kill | ✅ batch 5 (`5f5ae7e`) |
| IPC-01 #85 | blocking-recv lost wakeup + arg clobber | ✅ batch 5 (`225e294`) |
| **USR-SEC-03 #79** | **no sandboxing at any layer** | **❌ this plan** |
| ELF-01 | crafted ELF maps into kernel page tables | tracked separately (mm/elf) |

So B3's last deliverable is the sandboxing primitive. Everything else in
#93 is done.

## 2. Scope boundary — what B3 delivers vs. what B5 owns

The B5 epic (#95) is literally titled *"Service isolation (seL4) & Plan 9
namespace"*. To avoid building the same thing twice, B3 delivers the
**kernel primitive** (a capability mask enforced at the syscall boundary,
monotonic across the process tree). B5 builds the **policy and topology**
on top of it.

**In scope (B3 batch 6+7):**
- A fine-grained capability mask on `struct process`, replacing the flat
  3-bit `permissions` with named capabilities (the existing SYSTEM/ROOT/USER
  profiles become composed aliases — zero behavioural change for current
  callers).
- Enforcement at the **already-gated** syscall surfaces (spawn, FS write,
  IPC send, window/focus, registry write).
- **Monotonicity**: a child's capabilities are `requested & parent's` — you
  can only *drop* capabilities down the tree, never gain them. No escalation
  is possible by construction. SYSTEM bypasses, as today.
- A `spawn_caps(path, caps)` syscall for restricted spawn; plain `spawn`
  stays a full-USER spawn (no breaking change).
- `/bin/sandboxtest` proving each gate denies (-EPERM) and the monotonic cut.

**Out of scope (deferred to B5 #95, noted on #79):**
- Plan 9 per-process namespaces / mount views.
- Transferable seL4-style capability tokens (these need an object table;
  the bitmask is the precursor).
- Per-syscall seccomp-style filters.
- Per-window (#68/#69) and per-IPC-queue (#84/#85) quotas — the remaining
  half of #122; natural to do alongside the B5 compositor/IPC decoupling.
- ELF-01 (ELF mapping into kernel page tables) — an mm/loader fix, not a
  capability question.

## 3. Design

### 3.1 Capability mask

Today (`kernel/include/kernel/sched.h:123-125`):

```c
#define PROC_PERM_SYSTEM (1 << 0) /* Cannot be killed, has kernel access */
#define PROC_PERM_ROOT   (1 << 1) /* Can spawn and kill other processes */
#define PROC_PERM_USER   (1 << 2) /* Standard user app permissions */
```

`struct process.permissions` is read in 5 places in the dispatcher and in
`process_kill_allowed`. Proposal: keep the field, widen its meaning to a
32-bit capability set, and define the three legacy names as composed
aliases so every existing test (`x & PROC_PERM_SYSTEM`, etc.) keeps working.

```c
/* Fine-grained capabilities (B3 batch 6, USR-SEC-03 #79). */
#define CAP_SPAWN     (1u << 3)  /* SYS_SPAWN / spawn_caps */
#define CAP_FS_WRITE  (1u << 4)  /* SYS_FILE_WRITE + open-for-write */
#define CAP_IPC_ANY   (1u << 5)  /* SYS_SEND to non-relatives */
#define CAP_WINDOW    (1u << 6)  /* SYS_CREATE_WINDOW + SET_FOCUS(self) */
#define CAP_REG_WRITE (1u << 7)  /* SYS_REGISTRY write op */

/* Profiles = composed aliases (existing bit tests unchanged). */
#define CAP_USER_DEFAULT \
  (PROC_PERM_USER | CAP_SPAWN | CAP_FS_WRITE | CAP_IPC_ANY | \
   CAP_WINDOW | CAP_REG_WRITE)
```

SYSTEM and ROOT keep their bypass semantics. A plain USER spawn grants
`CAP_USER_DEFAULT` so today's apps (shell, writetest, fdtest, doom, …) are
unaffected.

### 3.2 Enforcement points

Each gate is a surface *already* validated in the dispatcher — we only add a
capability test. Mapping:

| Capability | Gate (file:symbol) | Denied behaviour |
|---|---|---|
| `CAP_SPAWN` | `syscall_dispatch.c` `SYS_SPAWN` / `spawn_caps`, before the quota gates | -EPERM |
| `CAP_FS_WRITE` | `SYS_FILE_WRITE`; `sys_write` FD_FILE path; `SYS_OPEN` write mode | -EPERM (the /bin+/sys ACL still applies *on top* for holders) |
| `CAP_IPC_ANY` | `SYS_SEND` (`sys_ipc_send`) | restrict to parent + descendants; else -EPERM |
| `CAP_WINDOW` | `SYS_CREATE_WINDOW`, `SYS_SET_FOCUS` | -EPERM |
| `CAP_REG_WRITE` | `SYS_REGISTRY` op==write (`registry_set` owner path) | -EPERM (reads stay open) |

**Kill needs no capability**: the descendant model from batch 5
(`process_kill_allowed` ancestry walk) is already the capability-correct
policy.

`CAP_IPC_ANY` is the one new *relationship* check: without it, a process may
only `send()` to its parent or a descendant (reuse the ancestry walk added
in batch 5). This is what makes a sandboxed worker unable to talk to
arbitrary system services.

### 3.3 Monotonicity (the single security invariant)

In `process_create()` (`kernel/sched/process.c:531`), when a non-SYSTEM
creator requests a capability set for a child:

```c
child->permissions = requested_caps & creator->permissions;
```

A child can never hold a capability its creator lacks. The whole tree is
therefore bounded by the root grant; there is no path to escalation. SYSTEM
creators (and the kernel-internal `current_process == NULL` path) bypass the
mask.

### 3.4 New syscall: `spawn_caps`

`SYS_SPAWN` stays **byte-for-byte compatible** (one argument, grants
`CAP_USER_DEFAULT`). Restricted spawn gets a new number in the NEXS-private
range (≥234, next to `TRY_RECV`=233 / `SET_FOCUS`=232):

```c
#define SYS_SPAWN_CAPS 234
long spawn_caps(const char *path, unsigned long caps);  /* userland */
```

**Why a new syscall and not a second argument on `SYS_SPAWN`:** existing
callers invoke `spawn(path)` with whatever garbage sits in arg1; reusing it
as a capability mask would silently sandbox every legacy caller. A distinct
number is unambiguous and keeps the diff additive (mirrors how `open` was
added in batch 3).

## 4. Batches

### Batch 6 — kernel primitive (~300–400 LOC)

1. `sched.h`: capability defines + `CAP_USER_DEFAULT` alias; doc comment.
2. `process.c` `process_create`: monotonic `requested & creator` cut;
   `process_init` unchanged.
3. `syscall_dispatch.c`: the 5 capability tests at the table above;
   `SYS_SPAWN` grants `CAP_USER_DEFAULT`; new `SYS_SPAWN_CAPS` case applying
   the requested mask through the monotonic cut.
4. `sys_ipc_send`: relative-only send when `!CAP_IPC_ANY` (ancestry walk).
5. `include/api/syscall_nums.h`: `SYS_SPAWN_CAPS 234`.
6. userland: `_sys_spawn_caps` stub in **both** `user/arch/aarch64/syscall.S`
   and `user/arch/amd64/syscall.S` (and the stale `user/sys/lib/syscall.S`
   for consistency); `spawn_caps` wrapper + `CAP_*` defines exposed in
   `include/api/os1.h`.
7. `user/bin/sandboxtest.c` + Makefile (BIN_ELFS + link rule, mirror
   `fdtest.elf`): spawn a child with `caps = CAP_WINDOW` only and assert
   - `spawn()` of a grandchild → -EPERM (no CAP_SPAWN),
   - `file write` → -EPERM (no CAP_FS_WRITE),
   - `send()` to a non-relative service PID → -EPERM (no CAP_IPC_ANY),
   - a *permitted* op still works,
   - a child requesting MORE than the parent holds gets the parent's subset
     (monotonic cut visible).

**Verification (mandatory, as every batch):** build BOTH arches
(-Werror clean), headless QMP boot BOTH; `sandboxtest` all-pass; regression
`fdtest` 8/8, `writetest` 3/3, `forkbomb` plateau 32, `ipc_send`/`ipc_recv`
delivery, KTEST 5/5, zero panics. Closes the kernel half of #79.

### Batch 7 — integration + epic closure (small)

1. `init` assigns reduced profiles to system services it spawns
   (e.g. `notify_srv` without `CAP_SPAWN`/`CAP_FS_WRITE`) — proves the
   primitive in the real boot path. Gate behind a check that the services
   still function (notify_srv only needs IPC recv + window).
2. Docs: MANUAL §6 (process model) + §7 (syscall table) gain the capability
   model; PHASE-B-PLAN B3 batch 6/7 entries; PROJECT_CHARTER capability row;
   REVIEW §8 commit rows.
3. **Close epic #93** with the batch 1–7 summary.
4. Comment on #79: kernel primitive done; the namespace/isolation half is
   re-slotted under B5 epic #95 (with the per-window/IPC quota residue of
   #122).

## 5. Risks & notes

- **ABI-07** (`SYS_SPAWN` runs `process_create` + `process_load_elf` with
  IRQs disabled across blocking disk I/O) is pre-existing; `spawn_caps`
  shares the path and must **not** make it worse — copy the structure
  verbatim, don't extend the critical section.
- The legacy `fd≥100` window-write alias and the window text-sink path are
  unaffected (no FS write).
- `registry` reads must stay open (services read `srv.*` keys); only the
  write op is gated.
- Repeat-error class to watch (documented across sessions): `*/` or `/*`
  sequences inside C comments when writing `CAP_*` lists; spawned apps steal
  keyboard focus, so QMP-typed shell commands after a spawn go to the app —
  use `counter`/non-focus bins or kill the child first.

## 6. Open decisions (need maintainer confirmation before coding)

1. **Granularity** — the 5 capabilities above *(recommended)*, or finer
   (per-path FS, per-service IPC)? Finer needs an object/namespace table →
   that is B5, not B3.
2. **Shell default** — spawned apps stay full-USER *(recommended: opt-in
   sandbox via `spawn_caps`, zero breakage)*, or sandbox-by-default (safer
   but immediately breaks writetest/fdtest/doom and any writer)?
3. **Ordering** — start batch 6 now, or land the visible graphics bugfix
   first (#118: windows don't repaint without mouse / dead windows linger)
   and do batch 6 after?

---

*Linked: `docs/PHASE-B-PLAN.md` (B3 batches 1–5), `docs/review/REVIEW.md` §8
(commit table), `docs/ASTRA.md` (implementation method). Issue #79 (this
work), epic #93 (B3), epic #95 (B5 isolation).*
