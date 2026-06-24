# Process Kill Model — by Type, Child, and Capability

**Status:** SPEC / TODO for the next implementer. Branch `comprehensive-review`
is currently reset to **`origin/main`** (`a36ed62`) — the last stable state: the
full perf campaign + every compositor fix (chrome-clip `fa9414a`) + the
maintainer's further compositor improvements ("compositor theme/style upgrade").
The previous attempt at this feature was reverted because it introduced a
use-after-free; it is preserved (see *Prior attempt* below) for reference only —
**do not cherry-pick it**, re-implement cleanly from this spec.

Maintainer: ramellaolmo01@gmail.com. Both arches (`aarch64` TCG + `amd64` HVF)
MUST be tested for every change (`make run`, and the headless harness below).

---

## 1. The model the maintainer wants

A spawned process is one of two kinds, and the kill must respect the kind:

| Kind | Definition | On kill of an ancestor |
|------|------------|------------------------|
| **Foreground / in-shell job** | Has **no top-level window of its own**; it runs "inside" the launcher's window/terminal (writes its stdout to the launcher's ctty window). E.g. `/bin/stress`, a CLI tool run from the shell. | **KILLED** together with the ancestor. |
| **Independent / windowed app** | **Owns its own top-level window** (it called `create_window`). E.g. a counter window, `doom`, `demo3d`, `forkbomb`'s per-child counters. | **SPARED** — left running; the user closes it manually via its own red button. |

Maintainer statements this encodes (verbatim intent):

- "se killiamo la shell vengono chiusi tutti i processi figli … **NON va bene**" —
  killing the shell must **not** blanket-kill every descendant.
- "il funzionamento corretto in caso di processi aperti come nuove finestre è di
  **lasciarle in esecuzione**" — windowed children keep running.
- "devono essere killati **solo i processi senza finestre o in esecuzione
  all'interno della finestra corrente**" — kill only windowless / in-shell jobs.
- "stress ok, se chiudo la shell **smette di aprire processi**" — closing the
  shell kills the windowless `stress` child (so it stops spawning).
- "in caso di forkbomb, alla sua terminazione era corretto che **le finestre
  rimanessero aperte** e che fosse l'utente a doverle chiudere" — terminating
  `forkbomb` leaves its counter windows open.
- "se dalla shell apro dei programmi … se li apre come **nuovi (non come figli)**
  allora possono rimanere in background; se sono **figli** (programmi aperti
  all'interno della shell), **devono essere killati**."
- "**abbiamo già una distinzione per tipo di processo, che devi sfruttare**" —
  reuse the existing window-ownership signal, do not invent a new one.

### The three axes

1. **By TYPE** — windowed vs windowless. The existing per-process signal is
   `compositor_get_window_by_pid(pid)` (kernel/graphics/compositor.c): returns a
   window id `> 0` iff `pid` owns a top-level window, else `-1`. This is the
   "distinzione per tipo" to exploit.
2. **By CHILD** — the `parent_pid` tree (`struct process.parent_pid`). A kill
   walks descendants; window-owning descendants (and their subtrees) are pruned.
3. **By CAPABILITY** —
   - *Killing* is already gated by `process_kill_allowed()` (ABI-04): a process
     may kill itself or a descendant; machine/root may kill anything;
     `process_terminate()` still protects machine-level targets.
   - *Spawning* should be gated by capability too: "ogni processo in base ai
     permessi che ha (capacitables) dovrebbe essere in grado di aprire nuovi
     processi o meno". Today USER apps spawn freely (the test bypassed this).
     A `CAP_SPAWN` (or reuse of the level model) should gate `SYS_SPAWN`, so an
     unprivileged app cannot fork-bomb. See kernel/include/api/caps.h and the
     `level_for_path` / per-path presets (ASTRA F1).

---

## 2. Required kill semantics

`kill(P)` from an **external** caller (window close button, `nxproc kill`,
Ctrl+C of a foreground job, `OBJ_CTL_CLOSE`/`OBJ_CTL_KILL`):

```
terminate(P)                       # P is the explicit target — always killed
for each descendant D of P (BFS over parent_pid):
    if D owns a window:            # compositor_get_window_by_pid(D) > 0
        skip D and its whole subtree   # independent app — leave running
    else:
        terminate(D); recurse into D   # windowless in-shell job — kill
```

- **Self-exit** (`SYS_EXIT`, a process killing **itself**) keeps single-process
  semantics: orphaned children reparent to a live ancestor (existing
  `__reparent_children`), they are NOT subtree-killed.
- **Fault-kill** (a process that faulted) kills only the faulting process.

---

## 3. Existing mechanisms to reuse (do not reinvent)

- `compositor_get_window_by_pid(int pid)` — the TYPE signal. **Takes
  `compositor_lock`.**
- `struct process.parent_pid`, `process_pool[]`, `MAX_PROCESSES` (=128),
  `__process_find_by_pid()` (caller holds `sched_lock`).
- `process_terminate(int pid)` — single-process teardown (handles the
  running/sleeping/created states + per-CPU reap).
- `process_kill_allowed(caller, pid)` — the capability gate.
- `__reparent_children`, `__child_count_dec` — orphan handling.
- `window_request_close(int pid)` (process layer, GFX-COMP-03 #69) — the seam
  the compositor calls; route the new subtree logic through here so graphics
  does not touch process internals directly.

Wire the new logic into: `window_request_close` (close button), the object-ctl
`OBJ_CTL_CLOSE`/`OBJ_CTL_KILL` (kernel/core/object.c), and `SYS_KILL` of another
process (kernel/core/syscall_dispatch.c). Leave `SYS_KILL` of self =
single-process.

---

## 4. CONCURRENCY PITFALLS (the two bugs that sank the first attempt)

> The first attempt crashed twice under SMP. Both are real; design around them.

### Pitfall A — `sched_lock` → `compositor_lock` is AB-BA
`process_terminate()` calls `compositor_destroy_windows_by_pid()` **while holding
`sched_lock`** (compositor uses *trylock* there, so it's safe). But
`compositor_get_window_by_pid()` takes `compositor_lock` with a **blocking**
`spin_lock`. So the TYPE check **must NOT run under `sched_lock`** — that is the
exact inversion `process_terminate` is careful to avoid.

**Fix pattern (verified working in the prior attempt):** snapshot every live
process's `(pid, parent_pid)` under `sched_lock`, release it, then do the BFS +
`compositor_get_window_by_pid()` checks **lock-free**, then call
`process_terminate()` on the selected pids (root first). Snapshot is consistent
because `process_create()` inserts under `sched_lock`.

### Pitfall B — freeing a `PROC_CREATED` victim mid-construction (UAF)
**Symptom:** `Data abort … FAR=0xcccccccccccccccc … get_next_table ←
arch_vmm_map ← process_load_elf_args ← dispatch_spawn`.

`dispatch_spawn` (kernel/core/syscall_dispatch.c) runs
`process_create()` → `process_load_elf_args()` (maps the ELF into the new
child's page table) with only **local** IRQs disabled. The child is already in
`process_pool[]` as `PROC_CREATED`, so a subtree-kill on **another CPU** finds it
and `process_terminate()` immediately frees its page table (the CREATED →
immediate-free path) **while the creator is still mapping into it** → UAF.

**Fix (validated):** `process_terminate()` must **defer** freeing a
`PROC_CREATED` victim — set a `kill_pending` flag on it and leave it CREATED (do
NOT flip it to DEAD, or the immediate-free path elsewhere will reap it and
double-free). The creator finalizes after the load, under `sched_lock`:
`process_finalize_spawn(p)` = if `kill_pending` → release it; else enqueue
(CREATED→READY) under the same lock so a kill can't slip in between. The load-
failure path frees directly (`process_abort_spawn`). The prior attempt's diff for
exactly this is in the backup (see below) and worked — reuse it.

### Pitfall C — `__dequeue_task` UAF on rapid termination (UNRESOLVED)
**Symptom:** `Data abort … FAR=0xfffe…cccc… __dequeue_task ← irq_handler ←
handle_el0_64_irq` while **rapidly closing many windowed processes** (closing
the spared counter windows one after another).

This is a freed/poisoned process still linked in a per-CPU runqueue (or a
double-dequeue) during termination under preemption. **Root-cause this first** —
it may be a *pre-existing* `process_terminate` ↔ `schedule()`/`__dequeue_task`
runqueue race (verify whether it reproduces at `fa9414a` WITHOUT any kill-model
change, by closing several windows fast). The kill model must not ship until
rapid multi-process termination is panic-free on **both** arches.

---

## 5. Secondary issues observed (fold in if cheap, else file)

- **Ctrl+C on a foreground job doesn't fire.** Delivery is fine (keyboard folds
  Ctrl-C → `0x03` `IPC_TYPE_INPUT` to the focus pid; `run_foreground` in
  user/sys/bin/nxshell.c matches it; `process_wait` is non-blocking;
  `create_window` does not steal focus). The break is `run_foreground` **falsely
  detaching** when it catches one of `stress`'s back-to-back transient gui-lane
  windows via `window_of_pid` (shell.c:205). Fix = debounce the detach (don't
  treat a momentary window as "this is a GUI app, detach"), or only detach a
  child that has held a window across N polls.
- **`demo3d` rare (1×/2h) close-doesn't-kill:** window destroyed by the
  compositor but the process survived (still `kill`-able afterward). Suspected
  fault-kill reap race in the window/terminate path ("funzione wins").
- **`amd64` release ISO unstable on UTM** (differs from QEMU `make run`, which
  boots clean). The self-contained ramdisk ISO path (`make release`,
  MICROSCOPE-RELEASE-STORAGE.md) crashes under UTM — investigate separately from
  the kill model.
- **`aarch64` feels a bit slow** under `make run` (TCG; doom ~10fps is largely
  TCG CPU-emulation of the game's software renderer, not a kernel gate).

---

## 6. Test plan (BOTH arches, every change)

Headless harness (reuse `tools/run-stress.sh` mechanism; `tools/qmp_type.py
<sock> "cmd"$'\n' <predelay>` types into the guest):

1. **Boot smoke** — 0 `PANIC|Unhandled|NESTED|Data abort`, reach Shell + Dock.
2. **Spare windowed children** — `forkbomb` (own window) spawns ~30
   `/bin/counter` (each own window); `kill <forkbomb_pid>` from the shell ⇒ only
   forkbomb (+ any counter still window-less mid-startup) dies; the windowed
   counters stay open. (The prior attempt verified `root=9 snap=33 kill=2`.)
3. **Kill windowless child** — closing the shell kills `stress` (windowless) so
   it stops spawning.
4. **Rapid close stress** — open many windows, close them in fast succession;
   must stay panic-free (this is where Pitfall C bit).
5. **Spawn-during-kill** — kill a subtree while a member is mid-`spawn`
   (Pitfall B). Use a tight spawner (`forkbomb`) and kill during the spawn burst.

`/bin/counter` prints a UART heartbeat every ~256 frames; spawn/terminate are
otherwise demoted to `pr_debug`, so add temporary `pr_info` markers (remove
before commit) to read pids/decisions from the serial log.

---

## 7. Prior attempt (PRESERVED — reference only, do not merge)

Preserved as patches in **`../nexs-kill-wip-backup/`** (sibling of the repo on
the maintainer's machine):
- `0001-…subtree…patch` — the committed subtree-kill.
- `0002-…full-delta…diff` — the complete delta from `fa9414a` (everything,
  incl. the PROC_CREATED UAF guard).
- `0003-…CREATED-uaf-fix.diff` — just the PROC_CREATED-mid-load UAF guard.

(The throwaway `wip/process-kill-model` branch was deleted after backup; the
patches above are the record. Re-implement cleanly from this spec — do not try
to replay the patches verbatim.)

What it got RIGHT (reuse): the window-aware subtree BFS (snapshot-then-check,
Pitfall A), and the `kill_pending` + `process_finalize_spawn`/`process_abort_spawn`
deferral (Pitfall B). What it got WRONG / left open: Pitfall C
(`__dequeue_task` UAF on rapid close) — never resolved; that is the blocker.
