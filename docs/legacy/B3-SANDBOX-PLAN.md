# B3 closing plan — privilege levels, capabilities & legacy purge

> **Purpose**: self-contained handoff to finish Phase B microphase **B3**
> (epic #93). Batches 1–5 have landed; this document specifies the final
> batches: a **4-level privilege model** with fine-grained capabilities
> (closing USR-SEC-03 #79) **plus the full userland legacy purge** so B3
> leaves nothing in legacy. It draws the line between what B3 delivers and
> what belongs to the onion epic #120 / the B5 isolation epic (#95).
>
> **Status (2026-06-13)**: design CONFIRMED by maintainer (the three §6
> decisions are answered below). Scope = sandboxing + legacy purge.
> Maintainer's process-area WIP (forkbomb/top/busybox) is done; work may
> proceed to completion without coordination stalls.

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
| **USR-SEC-03 #79** | **no sandboxing at any layer** | **❌ batch 6 (this plan)** |
| **USR-TTY-01 #123** | **stdout → UART, no shell inheritance; legacy ABI** | **❌ batch 7 (this plan)** |
| ELF-01 | crafted ELF maps into kernel page tables | tracked separately (mm/elf) |

## 2. Scope (CONFIRMED) — B3 = sandbox + legacy purge

Maintainer decision: **B3 delivers the sandboxing primitive AND purges all
userland legacy** so nothing legacy is left after B3. The onion/POSIX work
(#120) and Plan-9 namespaces / seL4 tokens (B5 #95) remain separate.

**In scope (batches 6 + 7):**
- **Batch 6 — privilege model:** a 4-level hierarchy (§3.1) with per-level
  capability presets and ceilings, replacing the flat 3-bit `permissions`;
  monotonic spawn (§3.3); enforcement at the already-gated syscall surfaces
  (§3.2); `spawn_level`/`spawn_caps` syscall; `/bin/sandboxtest`.
- **Batch 7 — legacy purge:** remove the `fd ≥ 100` window-id write alias;
  remove the 1023-byte window-write truncation (ABI-06) via bounce buffer;
  delete the stale unbuilt `user/sys/lib/syscall.S`; **stdout inheritance**
  so a child spawned by the shell prints into the shell's terminal, not its
  own window / UART-only (USR-TTY-01 #123 problem 1).

**Out of scope (deferred):**
- Modern terminal protocol (xterm/VT, scrollback, truecolor) — USR-TTY-01
  #123 *problem 2*, explicitly post-B3 (with #120 / B5).
- Plan 9 per-process namespaces / mount views; transferable seL4 tokens — B5
  #95 (the bitmask + level model here is the precursor).
- Per-syscall seccomp filters; per-window (#68/#69) and per-IPC-queue
  (#84/#85) quotas (the #122 residue) — B5.
- Full POSIX libc on services (the "onion") — epic #120.
- ELF-01 — an mm/loader fix, not a capability question.

## 3. Design

### 3.1 Privilege levels (the resolver foundation)

A process has exactly one **privilege level**. This is the root of the
capability tree and the resolver for the future multi-user model.

```c
/* Privilege levels (B3, USR-SEC-03 #79). Lower number = more privilege. */
#define PLVL_MACHINE 0  /* the machine's own identity — NOT a login user;
                         * unkillable, bypasses capability checks, root of
                         * the tree and resolver for future real users */
#define PLVL_ROOT    1  /* administrator */
#define PLVL_USER    2  /* standard application */
#define PLVL_GUEST   3  /* least privilege */
```

`machine` is deliberately not loginable: it is the kernel/system identity
(today's `init`, `notify_srv`, the kernel-internal `current_process == NULL`
path). When real users land, login sessions slot in at `root`/`user`/`guest`
and resolve their machine-level services through `machine`.

### 3.2 Capabilities and per-level presets/ceilings

Five fine-grained capabilities, one per already-gated syscall surface:

```c
#define CAP_SPAWN     (1u << 0)  /* SYS_SPAWN / spawn_level / spawn_caps   */
#define CAP_FS_WRITE  (1u << 1)  /* SYS_FILE_WRITE + open-for-write        */
#define CAP_IPC_ANY   (1u << 2)  /* SYS_SEND to non-relatives              */
#define CAP_WINDOW    (1u << 3)  /* SYS_CREATE_WINDOW + SET_FOCUS(self)    */
#define CAP_REG_WRITE (1u << 4)  /* SYS_REGISTRY write op                  */
#define CAP_ALL       (CAP_SPAWN|CAP_FS_WRITE|CAP_IPC_ANY|CAP_WINDOW|CAP_REG_WRITE)
```

Each level has a **ceiling** (max grantable) and a **default preset** (what
a process gets if the spawner doesn't specify):

| Level | Ceiling | Default preset | Notes |
|---|---|---|---|
| `machine` | CAP_ALL (+bypass) | CAP_ALL | unkillable; checks bypassed |
| `root` | CAP_ALL | CAP_ALL | admin |
| `user` | CAP_ALL | CAP_ALL | preserves today's behaviour |
| `guest` | CAP_WINDOW | CAP_WINDOW | can draw; no spawn/fs-write/reg-write/arbitrary-IPC |

`user`'s ceiling stays CAP_ALL so every current app (shell, writetest,
fdtest, doom, …) is unchanged — `SYS_SPAWN` yields a full `user`. Tightening
is opt-in via `spawn_level`/`spawn_caps`. Mapping to the flat aliases kept
for source compatibility: `PROC_PERM_SYSTEM`→`machine`, `PROC_PERM_ROOT`→
`root`, `PROC_PERM_USER`→`user`.

### 3.2.1 Enforcement points

Each gate is a surface *already* validated in the dispatcher — we only add a
capability test:

| Capability | Gate (file:symbol) | Denied |
|---|---|---|
| `CAP_SPAWN` | `SYS_SPAWN`/`spawn_*`, before quota gates | -EPERM |
| `CAP_FS_WRITE` | `SYS_FILE_WRITE`; `sys_write` FD_FILE; `SYS_OPEN` write mode | -EPERM (the /bin+/sys ACL still applies on top) |
| `CAP_IPC_ANY` | `SYS_SEND` (`sys_ipc_send`) | restrict to parent + descendants (batch-5 ancestry walk); else -EPERM |
| `CAP_WINDOW` | `SYS_CREATE_WINDOW`, `SYS_SET_FOCUS` | -EPERM |
| `CAP_REG_WRITE` | `SYS_REGISTRY` write op | -EPERM (reads open) |

Kill needs no capability: the batch-5 descendant model is already the
capability-correct policy.

### 3.3 Monotonicity (the single security invariant)

In `process_create()`, for a non-`machine` spawner requesting `(level, caps)`
for a child:

```c
child->level = max(req_level, creator->level);          /* never more privileged */
child->caps  = req_caps & level_ceiling[child->level] & creator->caps;
```

Three clamps, all in one place: a child is **never more privileged** than its
creator (level can only move toward `guest`), **never exceeds its level's
ceiling**, and **never holds a capability the creator lacks**. Escalation is
impossible by construction. `machine` creators (and `current_process == NULL`)
bypass.

### 3.4 New syscall: restricted spawn

`SYS_SPAWN` stays byte-for-byte compatible (one argument; yields a full
`user`). Restricted spawn gets a new NEXS-private number (≥234, next to
`TRY_RECV`=233 / `SET_FOCUS`=232):

```c
#define SYS_SPAWN_CAPS 234
/* userland */
long spawn_level(const char *path, int level);              /* preset for level */
long spawn_caps (const char *path, int level, unsigned long caps); /* explicit */
```

A distinct number (not a second arg on `SYS_SPAWN`) avoids silently
sandboxing legacy callers that pass garbage in arg1 — mirrors how `open` was
added in batch 3.

## 4. Batches

### Batch 6 — privilege levels + capability primitive (kernel)

1. `sched.h`: `PLVL_*`, `CAP_*`, `level_ceiling[]`/preset table; add
   `uint8_t level; uint32_t caps;` to `struct process` (keep `permissions`
   as a compat alias of `caps`, or migrate the 5 readers). Flat
   `PROC_PERM_*` become aliases.
2. `process.c` `process_create`: the three-clamp monotonic cut (§3.3);
   privileged checks switch from `permissions & (SYSTEM|ROOT)` to
   `level <= PLVL_ROOT`; unkillable check → `level == PLVL_MACHINE`.
3. `syscall_dispatch.c`: the 5 capability tests (§3.2.1); `SYS_SPAWN` →
   full `user`; new `SYS_SPAWN_CAPS` applying `(level, caps)` through the cut.
4. `sys_ipc_send`: relative-only send when `!(caps & CAP_IPC_ANY)` (reuse the
   batch-5 ancestry walk).
5. `syscall_nums.h`: `SYS_SPAWN_CAPS 234`.
6. userland: `_sys_spawn_caps` stub in **both** arch `syscall.S` (+ the stale
   one if not yet deleted); `spawn_level`/`spawn_caps` wrappers + `PLVL_*`/
   `CAP_*` in `include/api/os1.h`.
7. `user/bin/sandboxtest.c` + Makefile: spawn a `guest` child and assert
   `spawn`/`file write`/`send`-to-non-relative all → -EPERM, a permitted op
   works, and a child requesting more than the level/parent allows gets the
   clamped subset.

**Verify (mandatory):** build BOTH arches (-Werror), headless QMP boot BOTH;
`sandboxtest` all-pass; regression `fdtest` 8/8, `writetest` 3/3, `forkbomb`
plateau 32, `ipc_send`/`ipc_recv` delivery, KTEST 5/5, zero panics.

### Batch 7 — userland legacy purge

1. **Remove `fd ≥ 100` window-id alias** in `sys_write`; any remaining caller
   migrates to the fd table / `printf_win`.
2. **Remove the 1023-byte truncation** on `FD_WIN` writes — use a kmalloc
   bounce buffer capped at `SYSCALL_MAX_IO_BYTES` like the FD_FILE path
   (retires ABI-06).
3. **Delete `user/sys/lib/syscall.S`** (stale unbuilt aarch64 duplicate).
4. **stdout inheritance (USR-TTY-01 #123 problem 1):** at spawn, a child
   inherits the parent's fd 1/2 *target window* (the shell's TTY), so output
   from a shell-launched program lands in the shell terminal instead of the
   child's own window / UART-only. UART mirror kept as the serial log.
   `create_window` still lets a program open its own surface and repoint its
   stdout. Verify: a CLI program run from the shell prints in the shell
   window on both arches; windowed apps unaffected.
5. Docs: MANUAL §6/§7, PHASE-B-PLAN B3 batch 6/7, CHARTER, REVIEW §8; close
   epic #93; comment #79 (kernel primitive done) and #123 (problem 1 done,
   problem 2 = protocol, post-B3).

## 5. Risks & notes

- **ABI-07**: `SYS_SPAWN` runs `process_create`+`process_load_elf` with IRQs
  disabled across blocking disk I/O. `spawn_caps` shares the path — copy the
  structure verbatim, don't widen the critical section.
- stdout inheritance interacts with focus/compositor: the child writes into
  the shell's window while the shell also renders its prompt — interleaving
  is expected (that's how a real TTY foreground process behaves). Keep it
  simple; full job control is out of scope.
- `registry` reads must stay open; only the write op is gated.
- Repeat-error class: `*/` or `/*` inside C comments when writing `CAP_*`
  lists; spawned apps steal keyboard focus, so QMP-typed shell commands after
  a spawn go to the app — use non-focus bins or kill the child first.

## 6. Decisions — RESOLVED (2026-06-13, maintainer)

1. **B3 scope** → **sandbox + legacy purge** (this doc). #120 onion stays
   separate.
2. **Privilege model** → **4 levels** `machine`/`root`/`user`/`guest` with
   per-level capability presets, changeable by a higher level up to that
   level's ceiling. `machine` is not a login user — it is the machine's
   identity and the resolver for future real users.
3. **Ordering** → start now; maintainer's process-area WIP is finished, no
   coordination stall; carry through to completion.

---

*Linked: `docs/PHASE-B-PLAN.md` (B3 batches 1–5), `docs/review/REVIEW.md` §8,
`docs/ASTRA.md`. Issues: #79 (sandbox), #123 (TTY/printing), epic #93 (B3),
#120 (onion userland), epic #95 (B5 isolation).*
