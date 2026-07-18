# PLAN 2026-07-17 — Process model, jobs, error-notification & library stratification

Living plan for the maintainer's 2026-07-17 directive. Ordered by phase; each
phase names the **kernel (ASTRA primitive)** work and the **library** work
separately, per the standing rule:

> Optimise kernel-side with ASTRA first, THEN expose it in our library as two
> distinct, non-duplicated layers: `OS1_*` (NEXS logic) and the POSIX/libc
> names (a thin compatibility mapping over `OS1_*`). Kernel and `lib.c`
> functions are **standard**, not app-shaped; apps get **ported** to them.

## Guiding principles (from the directive)
- No duplicated logic. One seam per concern; personalities map onto it.
- `fd` is a capability handle routed through the object manager + registry
  (ASTRA §6.2/§7.1) — partly true today, must be made uniform.
- Userland errors MUST be surfaced through the existing systems: the userland
  fault-recovery path (`fault_notify_user`, red popup) and the notify
  severities info/warn/error. `lib.c` and every future portability layer use
  this uniformly.
- Process identity (name/serial) must be stable enough to serialise processes
  for the bar, the app icons, jobs, and kill — nxexec is the authority that
  hands the identity (and icon) down.
- Separated (detached) process ≠ killed with its parent, but `jobs` still
  tracks it; integrated background processes are a new model on spawn/fork.
- Apps we forked (doom) were adapted to our OLD incomplete libc — those hacks
  get reverted as the library becomes standard.

## Known live bugs → mapped to phases
> ALL THREE ORIGINAL ENTRIES ARE NOW RESOLVED — kept with their outcomes because
> the diagnosis path is the useful part.  Stale wording here is what made a later
> reviewer repeat the "nxline/nxjobs are stubs" claim, so it is corrected in
> place rather than deleted.

- ~~doom loops on savegame LOAD~~ → **FIXED** (17b/17d, device-verified): the
  cause was `saveg_read8()` returning an UNINITIALISED byte on a short read, so
  the loops span on garbage.  Not an I/O-model bug at all.  Remaining doom work
  (revert its old-libc hacks, slow positional `fread`) is Phase 7.
- ~~lua test children stay SLEEPING~~ → **FIXED** (17q/17w, device-verified).
  The stated cause (missing ctty/stdin relay) was WRONG: the tests never feed a
  nested lua from the keyboard — they redirect (`< file`) or pipe.  The real
  causes were, in order: no shell redirection, no pipes, `lua_stdin_is_tty()`
  hardcoded to 1, and `nxshell -c` discarding the child's exit status.
- ~~`nxline.h` / `nxjobs.h` are STUBS~~ → **WRONG, AND ALWAYS WAS BY NOW**:
  nxline is 562 lines (history ring, Ctrl-R reverse search, completion) and
  nxjobs became real in Phase 2 (waitpid status, STOPPED, stop/cont, %N/name
  resolution).  Phase 6's real remaining work is PORTING kilo and the lua REPL
  onto them, since both still hand-roll their own line input.

---

## STATUS INDEX (realigned 2026-07-18)

Read this first: the phase SECTIONS below were written before the work happened
and several still describe finished work in the future tense.  This index is the
authoritative status; where a section disagrees with it, the index wins.
Phase NUMBERS are historical labels, not execution order — the order is the one
listed here.

| # | Phase | Status |
|---|-------|--------|
| 0 | Error visibility (`OS1_report_error`) | **DONE** |
| 1 | Truncation model + `truncate`/`ftruncate` | **DONE** (last item, ftruncate, closed 17s) |
| 2 | Process primitives (exit_code / waitpid / STOPPED) | **DONE**, device-verified |
| 3 | Process identity & serialisation | **DONE**, device-verified |
| 4 | TTY / redirection / pipes / job re-attach | **DONE** except (b) → moved to 11 |
| 9 | nxexec as THE standard executor (R6 daemon) | **IN PROGRESS** |
| 5 | fd / object / registry standardisation | **2 of 3 items already DONE** |
| 6 | nxline / nxjobs — port to userland | **model DONE; porting TODO** |
| 12 | Service standardisation, ALL services + `/sys/services` | TODO |
| 10 | POSIX/libc completion as a repeatable gate | TODO |
| 10a | LIBRARIES: no shared logic w/ kernel (sep. from 12) | WIP parked in `test/`; redesign per objects |
| 11 | Users / capabilities / filesystem (ASTRA) | BLOCKED on design doc |
| 7 | doom revert + lua finish | TODO (last: depends on 9/10) |
| 8 | naming → bar/icons | **FOLDED INTO 3** (was a duplicate) |
| 9b | exit status must survive reaping (ROOT CAUSE) | **DONE**, device-verified (47/47) |
| 9c | shell → service for NON-interactive launches | **NEXT** |
| 9d | ctty handback; interactive jobs move too | after 9c |
| 14 | window management kernel-side; nxwins as service | NEW — doc first |
| 15 | split services from CLI/GUI interfaces | NEW — after 12 + 14 |
| 16 | ROADMAP §1.C scheduler/IPC blockers | NEW — gates 9d |
| 13 | Orphaned ASTRA §7.11 structural items | NEW — see below |

### Corrections this realignment applied
- **Phase 8 was a duplicate of Phase 3** ("nxexec provides display name + icon to
  bar/dock") — Phase 3 already shipped exactly that (17h/17j).  Folded in.
- **Phase 5**: "every fd is a handle" and "POSIX fd calls are thin compat over
  `OS1_object_*`" are ALREADY TRUE (the fd table was absorbed into the object
  table; `SYS_OPEN/CLOSE/READ/WRITE/LSEEK` are `sys_handle_*`/`sys_object_*`).
  Only *process/job state in the registry* remains → maintainer chose the
  VIRTUALISED (`/proc`-like) form.
- **Phase 6**: the plan called `nxline.h`/`nxjobs.h` STUBS.  They are NOT: nxline
  is 562 lines with history ring + Ctrl-R search + completion, and nxjobs became
  real in Phase 2.  What actually remains is the PORTING half (kilo and the lua
  REPL still hand-roll their own line input).
- **Phase 9**: the shape decision is CLOSED (maintainer chose the R6 privileged
  daemon), and its three prerequisites are already implemented — they had no
  phase home before: `OBJ_TYPE_PORT` + `OS1_NS_PORT` (device-verified),
  `owner_pid` + `OBJ_CTL_SETOWNER`, and the `source_pid` fd-source split.
- **Phase 9 depends on Phase 12**: the daemon is not actually "the standard
  executor" until it is started from the boot path, which is Phase 12 work.
- **Phase 4(b)** was deferred *pending a design doc*; that doc now exists
  (`DESIGN-2026-07-18-NXEXEC-DAEMON.md`), so the deferral reason is spent — the
  work moves to Phase 11 under its recorded scope limits.
- **ASTRA §7.11 is itself stale**: it still lists `OS1_fs_write` as taking the
  ambient path, which the C1–C2 capability closure removed.

## Phase 9 — subphases (maintainer 2026-07-18: resolve each node as its own
## subphase, CHAINED so every later phase starts simpler)

### 9b — exit status must survive reaping (ROOT CAUSE, do this FIRST)
**Found by the lua suite still failing at main.lua:72 after redirection, pipes,
argv[0] and status propagation were all verified working.**  It is not a lua
bug, nor a shell bug: it is a kernel lifecycle gap.

`process_wait()` writes `*out_code` ONLY when it finds the corpse
(PROC_DEAD/PROC_ZOMBIE).  If the scheduler reaper already drained it, the call
returns -2 with `out_code` UNTOUCHED — so a caller that initialised it to 0
reports SUCCESS.  A child that fails FAST is exactly the case that gets reaped
before its parent polls, which is why `assert(not os.execute(bad_program))`
still fails: the failing lua exits immediately and is collected before nxshell
looks.  `system()` already documents the hole ("a -2 'reaped elsewhere' leaves
code 0").

There are no real ZOMBIE semantics: a corpse is not retained until its owner
reaps it.  This silently corrupts EVERY exit-status consumer — `system()`,
`waitpid()`, jobs' "Done(N)" — not just lua, and it is timing-dependent, so it
looks like flakiness rather than a bug.

Fix direction: retain the status until the owner collects it (POSIX zombie
semantics), keyed on the LOGICAL owner (`owner_pid`) so it works when a service
did the spawn.  Cheapest correct form: a small reaped-status table the reaper
writes and `process_wait` consults, so corpses can still be freed eagerly.

**Do this before 9c/9d**: without it, migrating the shell onto the service would
be debugged against a status channel that is itself unreliable.

### 9c — shell uses the service for NON-INTERACTIVE launches (option b)
Maintainer: do (b) first "solo per finalizzare la logica".  `system()`,
`os.execute` and graphical launches go through the daemon; INTERACTIVE
foreground jobs keep the in-process path for now.  Rationale: `SETOWNER`
restores AUTHORITY (verified) but NOT the controlling terminal, so moving
interactive jobs first would break Ctrl-Z/Ctrl-C — the one part of job control
already validated on device.

### 9d — ctty handback, then interactive jobs move too (option a)
Needs a kernel verb to reassign `ctty_win` (or to carry it in the spawn
request), so a job spawned BY the service still has the REQUESTER as its
controlling terminal.  Only then can 9c's split be collapsed and the
in-process path deleted.  Dependencies to resolve here: keyboard relay, Ctrl-Z
suspension, and the window-ownership probe that decides GUI-vs-terminal.

## Phase 14 — Window management into the kernel; nxwins becomes a service
Maintainer directive: verify nxwins, move window MANAGEMENT kernel-side, leave
nxwins as a service.  Flagged as requiring **development, study and planning**
before code — treat like R6: a design doc first.
- Study first: what nxwins does today vs what the compositor already owns
  (window objects and OBJ_CTL_MINIMIZE/RESTORE/FOCUS/CLOSE already exist as
  capabilities, ASTRA §6.7 — so part of "management" may already be kernel-side
  and the real work is deciding the BOUNDARY, not writing new code).
- Chaining benefit: 9d needs the window-ownership probe (GUI-vs-terminal
  detection).  If window state becomes a kernel-owned, queryable fact, that
  probe stops being a debounced heuristic — 9d gets simpler as a result, which
  is exactly the "each phase makes the next one easier" ordering the maintainer
  asked for.

## Phase 15 — Split services from their CLI/GUI interfaces
Follows 12 and 14: once every service is supervised, port-addressed and
capability-scoped, its INTERFACE (a CLI tool, a GUI window) becomes a separate
client of the same port — so one service can be driven from the terminal, the
dock, or a script without three implementations.  Depends on 12 (services
relocated + per-service caps) and 14 (windows as a service).

## Phase 10a — LIBRARIES: userland stops sharing ANY logic with the kernel
> **This is the LIBRARIES phase.  It is SEPARATE from Phase 12 (services)** —
> maintainer, 2026-07-18.  An earlier revision of this plan said "do it WITH
> Phase 12's /sys/services move"; that was wrong.  Libraries and services are
> different boundaries and merging them would couple two large moves that can
> and should be verified independently.

### MEASURED STATE (2026-07-18, verified — not estimated)
The PRIMARY violation is userland compiling KERNEL SOURCE, not header direction:

| what | where | scale |
|---|---|---|
| userland compiles kernel `.c` files | `user/sys/lib/lib.c:576-578` | **1567 lines of kernel code inside EVERY user ELF** |
| dual-compiled conditionals | `kernel/lib/math.c` (9), `vsnprintf.c` (3), `string.c` (0) | 12 `#ifdef KERNEL` sites |
| kernel reaches into userland API | `kernel/include/kernel/elf.h:4` → `"../../../../include/api/elf.h"` | a relative-path escape out of the kernel tree |
| userland API reaches into kernel | `include/api/elf.h:4` → `<kernel/types.h>` | **circular**: kernel → api → kernel |
| the rule already exists and is broken | `Makefile:100`: *"the kernel must never grow includes from it (ASTRA layer separation)"* | stated, then violated two lines later |

**Latent bug found while measuring** (nobody had reported it): both `elf.h` files
use the SAME include guard `_KERNEL_ELF_H`, and the kernel one opens `#ifndef`
without ever DEFINING it — the userland one defines it.  So if the userland
header is included first, the kernel header's entire body is silently skipped:
which ELF definitions exist depends on INCLUDE ORDER.  That is the concrete cost
of the entanglement, and it is invisible in a passing build.

**Why the source-compilation case is the severe one**: the 12 conditionals mean
those files are not "a utility userland happens to reuse" — they are ONE FILE
THAT IS TWO DIFFERENT IMPLEMENTATIONS behind `#ifdef KERNEL`.  So the fix is not
to copy them; it is to RESOLVE the conditionals into two independent
implementations (maintainer directives 1 and 2).  `string.c` has ZERO
conditionals, so for it "separate logic" would be pure duplication unless the
two are allowed to diverge BY PURPOSE (kernel: minimal freestanding; userland:
POSIX-complete) — that choice has to be made explicitly, not by default.

### Critique of the existing docs (they are obsolete on this point)
`docs/R0-CENSUS-LIB-SPLIT.md` (R0.3) proposes moving the 9 shared headers to a
neutral `include/abi/` — "contratto, nessuna implementazione".  That directory
now exists and has been grown further during Phase 4/9 (caps.h, object.h,
syscall_nums.h, posix_types.h …).

**The maintainer's ruling is that `abi/` is CONCEPTUALLY WRONG, and on
reflection that is right**: a shared header directory is still shared
COMPILE-TIME coupling between kernel and userland.  It renames the dependency
instead of removing it — both sides still have to be rebuilt together, and a
struct layout or a number changed on one side silently redefines the other.
Measured today: `Makefile:101` gives the KERNEL build `-Iinclude/abi
-Iinclude/api`, i.e. the kernel compiles against the USERLAND api headers, and
11 kernel files include them.

### The ASTRA-conformant target (maintainer directives 1 + 2)
- **Information flows as OBJECTS, queried dynamically**, not as headers compiled
  into both sides.  The kernel is instrumented to EXPOSE what userland needs
  (types, rights, verbs, limits) through the object/registry model it already
  has — self-describing and discoverable, which is what §6.2/§6.6 already say
  about "every node representable as a file, every operation a message".
- **Userland libraries are independent**: no logic shared with the kernel, no
  `#include` of kernel sources (the USR-LIB-01 defect), no kernel-owned struct
  reused as a wire format.
- **Kernel includes live ONLY in the kernel's own lib** — nothing under
  `kernel/` may be reachable from a userland translation unit.
- **`vsnprintf` gets the same treatment** (directive 2): distinct
  implementations, communicating per ASTRA rather than sharing a file with an
  `#ifdef KERNEL` seam.

### The one honest tension — state it, do not paper over it
A *minimal* bootstrap contract is unavoidable: the syscall entry convention plus
ONE discovery verb, because a process cannot ask the kernel anything before it
can make a call at all.  Everything above that (object types, rights bits, verb
numbers, message layouts) can become object-mediated and discovered.  The design
work of this phase is deciding exactly where that irreducible line sits — and
keeping it as thin as possible — NOT pretending it can be zero.

### Verification (directive 3)
Formal check with **coccinelle** (`spatch`), which the plan already lists as
available: a semantic patch that FAILS the build if any userland translation
unit reaches into `kernel/` (include or symbol), so the boundary is enforced
mechanically rather than by review.  This is the same "make the audit a
repeatable gate" idea as Phase 10, applied to the layering instead of to the
POSIX surface.

## Phase 10a (WIP parked) — USR-LIB-01: split the libc off the KERNEL SOURCES
Maintainer started this 2026-07-18 and parked it for a planned approach; the
WIP is preserved under `test/usr-lib-01-libc-split/` (patch + the three modules
+ a README with the open questions).  Nothing is built from there.

**The defect is real and was already flagged in-tree** (`lib.c`:
"USR-LIB-01 (W2 BAD-IMPL) Directly #includes kernel/lib C sources"):

```c
#include "../../kernel/lib/math.c"      /* userland COMPILING kernel sources */
#include "../../kernel/lib/string.c"
#include "../../kernel/lib/vsnprintf.c"
```

This inverts the layering the whole plan rests on — the POSIX personality is
supposed to sit ABOVE the OS1 base API, not reach into the kernel tree.  It also
blocks two things already scheduled: ASTRA §6.4 B5 (SRL/HAL source-tree split)
and Phase 12 ("services divided from the standard libc, integrated modularly"),
because the dependency currently runs the wrong way.

Decisions to take BEFORE re-applying (this is why it is a phase, not a patch):
1. **Duplication vs deliberate divergence.** Splitting creates two copies of
   string/math/vsnprintf.  That is right ONLY if they are ALLOWED to diverge
   (kernel: minimal freestanding; userland: POSIX-complete).  Otherwise it
   breaks "no duplicated logic" in a fresh place.  Write the rule down.
2. **The `#ifndef KERNEL` float guard in vsnprintf** loses its reason to exist
   after a split and must be resolved deliberately.
3. **Verification**: formatting is exercised by nearly every program, so a
   regression is broad but quiet.  Re-run the host validation harness
   (`%f`/`%e`/`%g`/`%p` vs the host libc) against the split copy — a clean build
   proves nothing here.
4. **Sequencing**: do it WITH Phase 12's `/sys/services` move, not before, or
   the same files get relocated twice.

## Phase 16 — ROADMAP §1.C scheduler/IPC blockers (previously UNOWNED)
Found by cross-review 2026-07-18 and VERIFIED in
`docs/ROADMAP-ASTRA-SERVERIZATION.md` §1.C.  That document calls these
load-bearing for every server thread and says they "must land in Phase 1", but
no phase here owned them — so they were invisible to this plan while Phase 9
was busy building a service on exactly that substrate.

- **`SCHED-CREATED-LEAK-01`** — a process killed while `PROC_CREATED` leaks.
  Directly in the spawn path the execution service now drives.
- **`SCHED-05 extended` — `kernel_ipc_send` unbounded queue = kernel-heap DoS.**
  Note the new `OBJ_TYPE_PORT` does NOT inherit this: its queue is bounded
  (`PORT_QUEUE_MAX`).  The AMBIENT pid-addressed `ipc_send` is still unbounded,
  and 9c/9d run over it, so this is a real prerequisite rather than a
  theoretical one.
- **`SCHED-05 AB-BA`** — `sched_lock → msg_lock → cpu->sched_lock` inversion;
  ROADMAP asks for a re-audit once server threads raise IPC concurrency, which
  is precisely what the exec daemon does.

Sequencing: these gate 9d (interactive jobs over the service) more than 9c.

## Phase 13 — Orphaned structural items (ASTRA §7.11) — NEW
These are recorded as open in ASTRA but had NO owning phase, so they were
invisible to planning.  Listed here to be scheduled or explicitly dropped,
not silently forgotten:
- **SRL/HAL source-tree split** (§6.4 B5): no `kernel/srl` / `kernel/hal` exist.
  Overlaps Phase 12's `/sys/services` move — do them together.
- **FPU/SSE save-restore on context switch** (CPU-AMD64-01 #38): landed and
  REVERTED the same day; unresolved, not merely unattempted.  Matters before any
  amd64-heavy workload.
- **DIR-03**: the full blocking `OS1_event_wait` (IPC/timer/window/process
  readiness) — only the input leg exists.  A POSIX-compat gap.
- **DIR-05**: DWARF `file:line` backtraces and a kernel "recovery mode"
  (quiesce a subsystem instead of `panic()`).
- **DIR-02/07**: system-driven desktop-resize broadcast (still per-window).

## Progress log
- **2026-07-17a**: Phase 0 seam `OS1_report_error()` landed (lib.c + os1.h),
  wired into `open()` + capability-routed `OS1_fs_write`. Integrated with the
  maintainer's concurrent `OS1_fs_write` (handle_create WRITE|CREATE) and
  `nxexec.h` (`nxexec_window_stable`).
- **2026-07-17b**: doom — root of the load hang found: `saveg_read8()` returned
  an UNINITIALIZED byte on short read, so the tclass loops spun on garbage.
  Fixed (`= 0`), added `savegame_error` guards to both `P_UnArchive*` loops,
  and `printf` stage trace in `G_DoLoadGame` (reaches serial = the "reinsert
  UART debug"). A bad save now ends visibly as "Bad savegame" instead of
  hanging. ORIGCODE audit: most gates are absent-hardware (joystick/cdmus/
  sound) — legit; `I_Error` already `exit(-1)`s even with ORIGCODE off.
- **2026-07-17c**: Phase 1 decision — offset-0 write == whole-file-replace is
  KEPT as the FS standard (verified: doom sequential + kilo both correct; it
  was NOT the doom bug). Added POSIX `truncate(path,len)` over that primitive
  (lib.c + unistd.h); real `unlink()` (was declared-but-undefined). `ftruncate(fd)`
  deferred → needs an fd->path / OBJ_CTL_TRUNCATE kernel verb (Phase 2/5).

- **2026-07-17d**: doom savegame load VERIFIED working on-device (full
  `[doom-load]` trace, `eof-check err=0`). Maintainer: load is very slow →
  logged as a **lib I/O optimisation** item (positional per-byte fread through
  the FILE layer + handle-per-call; batch/buffer it). doom + remaining userland
  deferred to Phase 7. Proceeding to Phase 2.

- **2026-07-17e**: Phase 2 (part 1) — per-process **exit_code** landed. Kernel:
  `struct process.exit_code`, set in `sys_exit()`, delivered by
  `process_wait(pid, *out_code)` and `sys_object_wait()` (spare `arg` = user
  int* status). Ambient `SYS_WAIT` ABI untouched (passes NULL) — zero risk to
  nxinit/nxexec/nxjobs which only test `!= -1`. Libc: `OS1low_process_wait_status`,
  a REAL `waitpid()` (was declared-but-undefined), `system()` returns the true
  status, `WEXITSTATUS` fixed to the standard `>>8` encoding. Builds clean.
  REMAINING in Phase 2: STOPPED state (Ctrl-Z/bg), and a "killed vs exited"
  flag (both report code 0 today). Next: Phase 3 (process identity/naming) or
  wire nxjobs to show "Done (N)".

- **2026-07-17f**: Phase 2 COMPLETE + nxinit/nxexec/nxjobs standardised.
  Kernel: `exited` flag (killed-vs-exited: killed dies without sys_exit ->
  process_wait yields -9 -> WIFSIGNALED); `PROC_STOPPED` state + `process_stop`/
  `process_cont` (modelled on PROC_SLEEPING; scheduler picker skips STOPPED) +
  `OBJ_CTL_STOP`/`OBJ_CTL_CONT` (DESTROY-gated, capability path). Libc:
  `OS1_process_stop`/`_cont`, `__wait_encode` (exit vs killed), `WIFSIGNALED`/
  `WTERMSIG` real. Standardised: **nxjobs** (waitpid status -> "Done (N)"/
  "Killed", NXJOB_STOPPED, real `bg`/`fg` via stop/cont), **nxexec**
  (Ctrl-Z -> OS1_process_stop -> NXEXEC_JOB_STOPPED), **nxshell** (registers
  Ctrl-Z'd job, real bg/fg), **nxinit** (`service_gone()` waitpid helper logs
  WHY a service died). Next: Phase 3 (process identity/naming).

- **2026-07-17g**: Phase 2 boots clean on QEMU (no panic from PROC_STOPPED /
  exit_code). Phase 3 started: fixed the process-name truncation
  (process_create copied 15 of the 32-byte field -> "/sys/bin/nxlaun"); now
  full PROCESS_NAME_MAX. Identity finding: nxbar serialises by window TITLE +
  pid (OS1_window_enum); nxicon resolves icons by HEURISTIC title/exe
  classification. The stable name/icon-from-nxexec mechanism is the open
  design decision (registry vs kernel field vs title convention).

- **2026-07-17h**: Phase 3 identity (maintainer chose window-title + registry).
  nxexec now assigns ONE canonical display name (exe basename) and publishes it
  both ways: titles its hosted window with it, and registers
  `sys.proc.<pid>.name`/`.icon` on spawn (unregisters when the hosted job
  exits). nxbar prefers that registry identity over the app's own window title
  for the focused-app label. Builds. OPEN follow-ups: nxui/dock tile labels +
  ICONS should read the same `sys.proc.<pid>.icon` (nxicon); a pruner for
  `sys.proc.<pid>` keys leaked by DETACHED GUI apps (harmless to display — dead
  pids have no window — but they accumulate).

- **2026-07-17i**: Ctrl-Z VERIFIED working on-device (`^Z` -> `[1] Stopped`,
  `jobs`, `fg %1` resumes). Root cause fixed: a foreground REPL is usually
  PROC_SLEEPING (OS1_sleep poll) at Ctrl-Z, but process_stop only accepted
  RUNNING/READY; now it stops SLEEPING too (its wake sources only re-ready a
  PROC_SLEEPING task, so STOPPED sticks) + __enqueue_task STOPPED guard +
  process_cont sets READY before enqueue. Test surfaced state-name gaps (a
  STOPPED proc showed "UNUSED"): fixed nxproc_state_str (+ nxsettings via it),
  kernel proc_state_name. Maintainer flag: nxwins now shows the registry
  identity (sys.proc.<pid>.name) like nxbar. Minor open: `fg <name>` (bare
  command name, not %N) says "no such job" — matches POSIX but could add
  %name-prefix matching.

- **2026-07-17j**: Phase 2 & 3 COMPLETE. Phase 2 polish: `fg`/`bg` accept a
  job NAME (nxjobs_resolve: %N / N / %name / name / none), so `fg lua` works.
  Phase 3 finish: nxui dock resolves tile icons from `sys.proc.<pid>.icon`
  (canonical identity) with title fallback (threaded vpid through the tile
  arrays); nxexec_prune_identities() GCs stale sys.proc.<pid> keys of dead
  detached apps, driven by nxbar on a ~5 s throttle. Next: **Phase 4** (TTY /
  controlling terminal) — the lua nested-spawn SLEEPING hang.

- **2026-07-17k**: Phase 2+3 verified (3/4 headless boots clean). NOTE: an
  INTERMITTENT kernel panic in the K3-userland bring-up window (~1/4 boots) —
  the pre-existing SMP boot-race class (ASTRA §7.9-7.10), NOT from these
  changes (they run post-boot). Worth its own hardening pass later. Phase 4
  analysis (below) started.

- **2026-07-17l**: Fixed hosted-terminal Ctrl-Z bug (maintainer report):
  nxexec_run_foreground now takes `job_control` — nxshell passes 1 (Ctrl-Z
  suspends + shell adopts the job), nxexec.c passes 0 (standalone hosted
  terminal RELAYS Ctrl-Z to the child, never suspending/orphaning it or
  closing the window; only Ctrl-C / program exit close it). Also confirmed:
  simple nested `os.execute('lua -e ...')` WORKS (`42 / true exit 0`) — Phase 4
  hang is specific to certain all.lua cases, not nested spawn in general.

- **2026-07-17m**: Revised the hosted-terminal Ctrl-Z (prev no-op relay was
  wrong — "not detected"). Now Ctrl-Z is DETECTED and SUSPENDS the hosted job;
  the terminal stays open showing "[suspended — Enter to resume, Ctrl-C to
  close]" (nxexec_wait_stopped_action): Enter -> OS1_process_cont + keep
  watching, Ctrl-C -> kill + close. Reverted the vestigial job_control flag.
  main.lua CONFIRMED uses shell redirection: `lua - < %s > %s`, `... | lua`
  (lines 82/90/93) -> Phase 4 needs nxshell `<`/`>`/`|` redirection (fd setup
  on spawn), not just a kernel ctty-stdin path.

- **2026-07-17n**: Phase 4 APPROACH APPROVED (maintainer): extend the spawn
  ABI with fd redirection, then wire nxshell `<`/`>`/`|`. Feasibility: SYS_SPAWN
  today carries only path/argc/argv/flags (flags = SPAWN_FLAG_DETACHED only);
  child fds 0/1/2 are always the console (process_install_stdio). Chosen model:
  PARENT opens the redirect targets (reusing the now-working open() with
  O_CREAT/O_TRUNC/errno), then passes {child_fd, parent_fd} pairs to spawn; the
  kernel DUPs the parent's handle into the child's fd slot (reusing the handle
  dup/grant mechanism) — no string marshaling, POSIX fork+dup2 semantics, child
  read(0)/write(1) then hit the FILE object. IN PROGRESS.

- **2026-07-17o**: Plan review before coding Phase 4 — one correction. Audited
  every `RUN(...)` in main.lua: NO test feeds a nested lua from the live
  keyboard; they ALL feed input via `< file` or `| pipe` and capture via
  `> file` / `2> file` (`2>` at line 71). So the real blocker is REDIRECTION +
  PIPES, not the interactive stdin-down-the-ctty-tree path the Phase 4 analysis
  section describes — that path is likely UNNEEDED for the lua suite. The
  generic {child_fd, parent_fd} dup model already covers `2>` (child_fd = 2) for
  free, and pipes reuse the SAME spawn-redir mechanism once a pipe kobject
  exists (parent makes a pipe; producer spawned with fd1←write-end, consumer
  with fd0←read-end). INCREMENT ORDER: (1) file redirection `< > >> 2>` (kernel
  dup-into-child-slot + libc + nxshell parser) — unblocks the majority
  (`lua %s > %s`, `lua - < %s > %s`); (2) pipes `|` (pipe kobject, reuse the
  mechanism); (3) revisit whether interactive ctty-stdin is still needed.
  Starting increment (1).

- **2026-07-17p**: Phase 4 increment (1) — FILE REDIRECTION `<`/`>`/`>>`/`2>`
  IMPLEMENTED, builds clean, boots. Kernel: `struct spawn_redir {child_fd,
  parent_fd}` + `SPAWN_MAX_REDIR` (caps.h); `process_redirect_child_fd()`
  (object.c) dups the spawner's handle into the child's fd slot, dropping the
  console ref (fork+dup2 as a capability dup, shared FILE offset); SYS_SPAWN
  reads arg4/arg5 = redir[]+count, `dispatch_spawn` applies them before the
  child runs (bad fd aborts the spawn). ABI: `_sys_spawn` stub now ZEROES
  x4/x5 (r8/r9) so old 4-arg callers can't leak garbage as nredir; new
  `_sys_spawn_redir` passes them through (both arches). Libc:
  `OS1low_process_spawn_redir` (os1.h + lib.c). nxexec.h:
  `nxexec_spawn_search_redir`. nxshell: `strip_redirections()` parses
  `<`/`>`/`>>`/`2>` (spaced + attached), opens targets, spawns with redir,
  closes its copies; centralised in `spawn_search_args` so the default path AND
  `exec` get it. VERIFIED-BY-CODE end-to-end: libc stdin/stdout/stderr FILEs
  have fd 0/1/2 and route through read()/write() → the handle table → the dup'd
  FILE object; read(0) on a FILE returns 0 at EOF (not -EAGAIN/block like a
  console) — the direct fix for the "inner reader sleeps forever" hang. AWAITING
  on-device verification. NEXT: increment (2) pipes `|` (pipe kobject).

- **2026-07-17q**: Increment (1) file redirection VERIFIED on-device — all 4
  cases pass: `lua -e "print(7*6)" > r.txt` → `42`; `lua - < s.lua` → `6` AND
  RETURNS TO PROMPT (the nested-input hang is fixed at the root); `... 2> e.txt`
  → `boom`; `lua - < s.lua > o.txt` → `6`. Running the real suite (`all.lua`)
  now advances PAST the interpreter tests (`progname: /bin/lua`, `Lua 5.4.6`)
  and stops at the FIRST PIPE (main.lua:93 `echo "..." | lua > %s`): echo (a
  builtin) printed `| /bin/lua > /home/.luatmp_2` literally, no temp file made.
  DISCOVERY that reshapes increment (2): EVERY pipe in main.lua (93,125,244,246,
  249,253,260) is `echo "<string>" | lua <args>` — the LHS is ALWAYS the echo
  builtin, never a spawned streaming producer. So `echo "x" | cmd` ≡
  `echo "x" > tmp; cmd < tmp; rm tmp`, i.e. it can REUSE increment (1)'s file
  redirection (shell materialises the bounded builtin output to a temp file)
  with NO new kernel primitive — vs a full OBJ_TYPE_PIPE kobject (real general
  pipes, but writer-count/blocking/EOF complexity). Fork raised to maintainer.

- **2026-07-17r**: Maintainer decision on the pipe fork — do ALL THREE, properly
  on the ASTRA object model: (a) a REAL pipe kobject, (b) temp-file, (c) a POSIX
  layer. Each gets a distinct role, so nothing is duplicated:
  * **(a) Kernel primitive (ASTRA §6.2)**: `OBJ_TYPE_PIPE` (= 6, COUNT→7) — a
    kernel ring buffer with reader/writer OPEN counts.  `sys_object_read`: data
    → return it; empty + writers>0 → -EAGAIN → SYS_READ blocks (as console
    stdin does); empty + writers==0 → 0 (EOF).  `sys_object_write`: append; full
    + readers==0 → -EPIPE.  Reader/writer counts maintained in ONE place per
    lifecycle edge (install/close/destroy/redirect-dup) via a
    `pipe_handle_count(o,rights,±1)` helper, keyed on OBJ_TYPE_PIPE + rights.
    Creation verb `SYS_PIPE` installs a READ handle + a WRITE handle and returns
    both fds (user int[2]).  Anonymous (no registry name) — like an unnamed FILE.
  * **(b) temp-file**: nxshell's `|` when the LHS is a BUILTIN producer (echo —
    100% of main.lua's pipes): materialise its bounded output to a temp file and
    spawn the RHS with fd0←that file, REUSING increment (1)'s redirection; rm
    after.  No streaming machinery for the case that doesn't need it.
  * **(c) POSIX layer**: `pipe(int fd[2])` in libc over `OS1low_pipe`, so
    `spawned | spawned` pipelines and any future POSIX app use the real kobject.
  nxshell `|`: builtin-LHS → path (b); spawned-LHS / multi-stage → path (a).
  IMPLEMENTING (2a) kernel pipe + POSIX pipe() first, then (2b) shell.

- **2026-07-17s**: STANDARDISATION pass (maintainer: "il tuo lavoro è la
  standardizzazione — crea unistd reale ... sia la call os1 che gli standard
  posix come layer di compatibilità") + closed-phase leftovers. Builds clean.
  * **Real `<unistd.h>`**: now the authoritative POSIX entry point, carrying a
    documented POSIX→OS1 mapping table and the KNOWN DEVIATIONS (read/write take
    `char *` and return `long`; `getcwd` returns int; no `sleep()` because
    OS1_sleep is MILLISECONDS).  Names os1.h already declares (read/write/close/
    lseek/chdir/getcwd/sbrk) are documented but NOT re-declared — one
    declaration each, so the headers cannot drift.
  * **DEFECT FOUND + FIXED**: `isatty()` and `getpid()` were DECLARED in
    unistd.h but implemented NOWHERE (the header promised link errors).  Both
    implemented.  `isatty()` now does a REAL capability-type test
    (OS1low_cap_query → OBJ_TYPE_CONSOLE) instead of "fd < 3" — required now
    that increment (1) exists: with `cmd > out` fd 1 is a FILE and with
    `cmd | cmd` a PIPE, and the lua REPL keys off isatty()==0 there.
  * nxshell now calls POSIX `pipe()` via `<unistd.h>` (was a direct
    `OS1low_pipe` shortcut taken only to dodge the missing declaration) — the
    compat layer exercising itself.
  * **PHASE 1 LEFTOVER CLOSED — `ftruncate()`**: composed from primitives, not
    duplicated kernel-side.  shrink to N>0 = read the head back + rewrite from
    offset 0 (offset-0 write REPLACES the file, so the rewrite IS the
    truncation); shrink to 0 = new **OBJ_CTL_TRUNCATE** verb, needed because
    POSIX defines write(fd,buf,0) as a NO-OP and it cannot express "empty this
    file"; extend = zero-fill from the old EOF.  Re-verified that the earlier
    zero-write fix did NOT break `truncate(path,0)`/`fopen("w")`: the PATH-based
    offset-0 zero write still truncates (syscall_dispatch.c), only the fd-based
    one is a no-op (correct POSIX).
  PHASE STATUS: 0,1,2,3 CLOSED (1's only deferred item now done). Phase 4 =
  (i) redirection VERIFIED, (ii) pipes built+compiling, awaiting device test,
  (iii) STILL OPEN: "re-attach a separated process into job control" and
  "root-vs-user process division (formalise the identity split)".

- **2026-07-17t**: CROSS-LAYER AUDIT (maintainer: "cerca se altre parti usano le
  std ... o stub incompleti; ragiona in ordine kernel—lib—posix/libc—lua").
  Device evidence that drove it: `lua -e "print(99)" | lua -e "print(io.read())"`
  → **99** (kernel pipe CORRECT, two live ends), but `echo "print(10)" | lua`
  printed the REPL banner + `>` and waited for a keypress.
  * **ROOT CAUSE (lua layer)**: lua.c's `lua_stdin_is_tty()` ISO-C fallback is
    hardcoded to 1 ("assume stdin is a tty") unless LUA_USE_POSIX is set — which
    we deliberately do not set.  So lua ALWAYS believed stdin was a tty and took
    the REPL branch instead of "execute stdin"; and our own `lua_readline`
    override reads the KEYBOARD mailbox (LUA-TTY-01), so the piped program was
    never consumed.  A textbook "stub used because the real function was
    missing" — isatty() existed only as a DECLARATION.  Fixed: lua_portability.h
    now defines `lua_stdin_is_tty()` = our real `isatty(0)` (LUA-TTY-02).
  * **LUA-TTY-03**: `-i` FORCES the REPL regardless of isatty, so
    `lua -i < file` (main.lua:335) and `echo ... | lua -i` (main.lua:249) still
    reached the keyboard reader.  `os1_lua_readline()` now falls back to
    `fgets(stdin)` whenever `!isatty(0)`.
  * **SYSTEMATIC AUDIT** (nm on every built libc object vs every prototype in
    our core headers) — found the remaining declared-but-UNIMPLEMENTED
    functions, the same class as isatty/getpid: **`_Exit`** (stdlib.h) and
    **`fdopen`** (stdio.h).  Both implemented.  `fdopen` mattered now: it is how
    ported POSIX code wraps a PIPE end in a FILE*.  Supporting it generalised
    `file_fd()` to return the stream's fd field instead of comparing against
    stdin/stdout/stderr — exactly equivalent for existing streams (the console
    structs carry 0/1/2, fopen sets -1) and it makes fdopen'd streams route
    through read()/write().  `fclose()` now closes an fd-backed stream's
    descriptor (POSIX ownership).  ctype's isalnum/isalpha/... were a FALSE
    positive: `static inline` in ctype.h, correctly defined.
  * Audit now returns EMPTY: no function declared in our core headers is
    unimplemented.
  * Maintainer also saw a one-off GRAPHICS glitch that did NOT reproduce; left
    UNDIAGNOSED (no repro) — the log around it shows only demo3d window
    close/focus churn, not a libc path.

- **2026-07-17u**: Phase 4 remaining items resolved.
  * **(a) RE-ATTACH A SEPARATED PROCESS — DONE.** New nxshell builtins
    `attach <pid>` / `disown [%N]`, closing the directive's "separated process
    is NOT killed with its parent, but jobs still tracks it".  The kernel's
    acquisition policy makes the split exact and honest: TRACKING (status, exit
    reporting) needs only a WAIT/READ capability, which any live pid grants — so
    a process this shell never spawned (a dock-launched app re-homed to another
    ancestor) can still be followed by `jobs`; fg/bg additionally need kill
    authority (self/descendant/privileged, process_kill_allowed), so they work
    for our descendants and fail visibly otherwise instead of pretending.
    `attach` names the job through nxexec_lookup_identity() — the SAME Phase 3
    registry identity the bar and dock use, so one process has one name
    everywhere.  Also fixed stale help text that still claimed `bg` was "not
    supported yet" (Phase 2 made it real) and documented the new redirection /
    pipe syntax in `help`.
  * **(b) ROOT-VS-USER IDENTITY SPLIT — DEFERRED, by the maintainer's OWN
    recorded decisions (PIANO-LIBC-ASTRA-2026-07-16 §"decisioni"), not skipped.**
    The division itself ALREADY EXISTS: level_for_path() (/sys/bin → PLVL_ROOT,
    everything else PLVL_USER) as a ceiling+default, the monotonic creator clamp
    in process_create_caps, per-level capability masks, SYS_GET_IDENTITY
    reporting (level<<16)|caps, and nxperm.h presenting it.  What would REMAIN
    is explicitly out of scope or gated:
      - decision 3: "nxauth (prima versione): solo utente root con password di
        preset. **Utenti nominali NON in questo blocco.**"
      - decision 4: per-user VFS partitions = "blocco futuro".
      - R6: nxauth + the kernel exec/kill consolidation require "**doc dedicato
        prima del codice**".
    So writing an identity/users subsystem now would violate the maintainer's
    own doc-before-code rule.  NEXT ACTION for (b) is the dedicated kernel
    exec/kill + nxauth design doc, not code.
  * Confirmed **C1–C4 capability closure is fully CLOSED** (C4: 13/13 PASS on
    amd64 AND aarch64, 0 fault) — no pending item from that block either.

- **2026-07-17v**: BUILD-SYSTEM BUG (why the LUA-TTY-02 fix appeared not to
  work).  Device run showed the pipe now DELIVERING (`10` and `2` executed) but
  lua still printing the REPL banner + `>` prompts, so main.lua:57 asserted:
  expected `'10\n2\n'`, got `'Lua 5.4.6 ...\n> 10\n> 2\n> > \n'`.
  Root cause was NOT the code: `lua_portability.h` is FORCE-INCLUDED into every
  Lua TU (`-include`), but was not listed as a PREREQUISITE of any lua rule, so
  editing it left STALE objects.  `lua_portability.c` recompiled (its own rule
  named it) — which is why the LUA-TTY-03 `fgets(stdin)` fallback DID take
  effect — while `lua.o` did not, keeping upstream's hardcoded
  `lua_stdin_is_tty() 1`.  Proved with `nm -u lua.o`: no `isatty` reference
  before, `U isatty` after.  FIXED: added `LUA_PORT_HDR` and made it a
  prerequisite of the lua pattern rule, `lua.o`, `LUA_PORT_OBJ` and
  `LUA_OS1_OBJ`, so this whole class of silent-stale-object bug cannot recur.
  * Maintainer note: the test suite is launched INTERACTIVELY (double-click in
    nxfilem), so the OUTER lua is legitimately a tty — that is correct, not a
    bug; only the INNER `echo ... | lua` must be non-interactive.  A possible
    nxfilem double-click issue (two instances) was observed but not reproduced;
    left undiagnosed rather than guessed at.

- **2026-07-17w**: `echo "print(10)" | lua` → **`10`** VERIFIED on device (clean,
  no banner/prompts): pipes + LUA-TTY-02/03 confirmed.  Suite advanced 57 → 72.
  New failure at main.lua:72 (`NoRun`: `assert(not os.execute(cmd))` — a command
  that MUST fail) was ANOTHER stale leftover of a closed phase: `nxshell -c` did
  `process_command(); return 0;`, carrying a comment claiming "OS1 does not yet
  provide a real exit-status channel".  That comment was OBSOLETE — Phase 2
  built exactly that channel (exit_code → waitpid → WEXITSTATUS); it was simply
  never wired into the shell, so every command reported success and os.execute
  could never see a failure.  FIXED end-to-end:
  `nxexec_run_foreground_ex(pid, &status)` reaps via
  OS1low_process_wait_status and yields shell-style status (exit code, 128+sig
  when killed, 130 for Ctrl-C); nxshell tracks `g_last_status`, sets 127 on
  command-not-found (POSIX), and `-c` exits with it; `system()` already encoded
  it, so os.execute now observes real failures.  (`nxexec_run_foreground(pid)`
  kept as the status-less wrapper for nxexec.c's hosted terminal.)
  * Self-inflicted bug caught during the edit: one brace-less `else` gained a
    second statement, which would have printed "Unknown command" even on
    SUCCESS in the temp-file pipeline path.  Fixed; other 3 sites verified
    properly braced.
  * NOT bugs (maintainer runs): `lua all.lua` from the shell failing "cannot
    open main.lua" is all.lua:149 doing `dofile('main.lua')` — a RELATIVE path,
    so it needs `cd /home/LUA/luatest` first.  And the OUTER lua being
    interactive when double-clicked from nxfilem is correct (its stdin really is
    the console); only the INNER `echo ... | lua` must be non-interactive.

## Appendix — analysis of the lua nested-spawn hang (historical)
> SUPERSEDED IN PART by 2026-07-17o: the interactive stdin-down-the-tree path
> below is NOT what main.lua needs (it always redirects/pipes input). Kept for
> the eventual interactive case (a hosted REPL reading the real keyboard).
Chain: outer `lua` (foreground job, shell relays its stdin) does
`os.execute` -> `system()` -> spawns `nxshell -c "lua ..."` and BLOCKS waiting
on it -> nxshell -c (windowless, unfocused) spawns the inner `lua` and runs
`nxexec_run_foreground`, which relays stdin by reading ITS OWN stdin
(try_recv). But keyboard IPC only goes to the FOCUSED window's pid
(keyboard.c), and the outer lua is blocked in system()'s wait — so nothing
feeds nxshell -c, nothing feeds the inner lua. Any inner reader (REPL /
io.read) sleeps forever; the outer lua's wait never returns -> `lua all.lua`
stalls. The ctty (`proc->ctty_win`) already carries stdOUT down the tree
(sys_write falls back to ctty_win); stdIN has no equivalent. Phase 4 = an
input path down the ctty/process tree so a windowless descendant reading fd 0
gets the terminal's input. Needs a runtime confirmation of the exact reader.

## Phase 0 — Error visibility (library) — **DONE**
Lowest risk, unblocks diagnosing doom + lua.
- **lib**: one internal seam `__libc_report(ctx, err)` in `lib.c` mapping an
  errno class → notify severity (EIO/EFAULT/ENOMEM/EROFS/ENOSPC → error/red;
  EACCES/EPERM → warn; ENOENT/EAGAIN/normal control flow → silent). Route the
  existing ad-hoc `OS1_notify_warn` calls (open, registry) through it. No spam:
  only unexpected/hard failures notify.
- **lib**: expose it so future portability layers (SDL, lua, doom shims) call
  the same seam — no per-app error plumbing.
- **kernel**: none (reuse `fault_notify_user` + the notify server).
- Exit criteria: a failing file/mmap/alloc in any app raises a visible
  notification with context; doom's load failure becomes observable.

## Phase 1 — Truncation model = standard — **DONE** (ftruncate closed 17s)
Decide + enforce the standard: truncation is an OPEN-time op (O_TRUNC /
`fopen("w")`), not a per-write side effect (POSIX pwrite never truncates).
- **kernel**: keep `open(O_TRUNC)` (already added). Re-evaluate the
  `ext4_write` "offset 0 truncates" behaviour — keep as pragmatic replace, or
  gate it behind a real truncate primitive; document the decision.
- **lib**: `ftruncate()`/`truncate()` over the primitive; `fopen("w")`
  truncates at open only.
- **port**: adapt apps that assumed write-time truncation to the standard API.

## Phase 2 — Kernel process primitives (ASTRA) — **DONE**, device-verified
- **kernel**: per-process `exit_code`; `SYS_WAIT` returns it (payload), so
  `system()` and jobs report real status ("Done (0)"/"Exit 1").
- **kernel**: `STOPPED` process state + resume, so Ctrl-Z/`bg`/`fg` are real
  (today only running/zombie exist).
- **lib**: `waitpid`/`WEXITSTATUS` compat over the payload; `OS1_process_wait`
  NEXS surface.

## Phase 3 — Process identity & serialisation — **DONE** (absorbed old Phase 8)
- **kernel**: widen/clean the `proc->name` field (16 vs 32 mismatch in
  sysstats), add a stable per-process serial; expose identity via the registry
  namespace (`/proc/<pid>` typed object already exists — extend).
- **lib/nxexec**: nxexec assigns the launch identity (display name + icon key)
  and records it, so the bar/dock/jobs all read ONE stable name, not a
  path-basename that collides ("lua", "lua", "lua").

## Phase 4 — TTY / redirection / pipes / job re-attach — **DONE** except (b)→Ph.11
- **kernel/lib**: controlling-terminal model so a hosted (windowless) child —
  including a NESTED spawn — receives stdin (fixes lua SLEEPING). Unify with
  `nxexec_run_foreground()`'s echo/relay (currently separate).
- **model**: integrated-background vs detached on spawn/fork; `jobs` tracks
  separated processes too; "re-attach" a separated process into job control.
- **kernel**: root-vs-user process division (level presets exist per path;
  formalise the identity split).

## Phase 9 — nxexec as THE standard executor — **IN PROGRESS** (shape DECIDED: R6 daemon)
Maintainer directive 2026-07-18: "nxexec deve essere il nostro esecutore
standard e va raffinato; dovrà essere il punto della compatibilità posix ed è
fondamentale per permettere un utilizzo grafico e da terminale SENZA ECCEZIONI".
- **Finding**: there are TWO divergent execution paths today.  nxlauncher (and
  nxfilem) spawn the nxexec BINARY; nxshell only reuses `nxexec.h` HELPERS and
  spawns programs itself.  Everything refined in the shell (redirection, pipes,
  job control, exit status, identity registration) therefore does NOT apply to
  the graphical path, and vice-versa.
- **Symptom already observed**: `progname` is `/bin/lua` when launched from the
  launcher but bare `lua` from the shell, because `nxexec_spawn_search*` spawns
  `do_spawn(out_path, ...)` while leaving `argv[0]` as the BARE name.  main.lua
  rewrites its commands with that progname, so a bare one produces `"lua" ...`
  which must be re-resolved — the "esecuzione da path non diretto" to fix.
- **Work**: consolidate the shell's refined execution logic INTO nxexec; make
  nxshell execute through it; make argv[0] reflect the resolved path; one
  cross-checked path for graphical AND terminal.
- **OPEN DESIGN FORK** (needs maintainer): the recorded R6 vision in
  PIANO-LIBC-ASTRA is "nxexec → **demone privilegiato di esecuzione**" that
  spawns a password panel for extra authority.  So which shape?
  (A) shared LIBRARY (today's nxexec.h, grown into the canonical executor);
  (B) a PROCESS per command (`nxshell` spawns `/sys/bin/nxexec cmd`);
  (C) a privileged DAEMON everyone asks over IPC (the recorded R6 vision).

## Phase 12 — Service standardisation, ALL services (+ `/sys/services`) — TODO (Ph.9 needs it)
Maintainer directive 2026-07-18: *"il lavoro di standardizzazione dei servizi va
fatto per TUTTI i servizi ... consiglio anche di spostarli da /sys/bin a
/sys/services, in questo modo le app utenti usano i servizzi; i servizi vanno
anche divisi dalla libc standard e integrati in maniera modulare"*.

Scope — every system service (nxinit, nxexec, nxntfy_srv, and the rest), not
just nxexec:
1. **Relocation `/sys/bin` → `/sys/services`.**  Separates SERVICES (things apps
   talk to) from BINARIES (things apps run), which is what makes "le app utenti
   usano i servizi" legible in the namespace itself.
   - **DEPENDENCY**: `level_for_path()` currently grants `PLVL_ROOT` to
     `/sys/bin/` specifically.  The preset MUST follow the services to
     `/sys/services/`, or every service silently drops to `PLVL_USER`.  The VFS
     write-ACL protects all of `/sys`, so immutability is unaffected.
   - Also touches: the `/bin`,`/sys/bin` spawn search order (nxexec.h), nxinit's
     supervised-service list, nxlauncher's hidden-entries list, mkdisk layout.
     ASTRA §7.11 already flags an exec-path census as needed for the `.X` format
     work (R5) — do the census ONCE and serve both.
2. **Each service publishes a PORT** under the `OS1nx_<service>` naming standard
   (`OS1NX_PORT_EXEC` is the first).  Clients then address a SERVICE, never a
   pid — the seL4 rule ASTRA §6.5 states.
3. **Per-service capability refinement** (Q5): ASTRA §7.11 records that every
   `/sys/bin/*` still gets the FLAT ROOT preset.  Each service gets an explicit,
   minimal capability set instead.
4. **Services split OUT of the standard libc, integrated modularly**: libc keeps
   the POSIX/OS1 personality only; service CLIENT stubs (the port protocol
   wrappers) live in their own module per service, so an app links only the
   services it uses.  This is the SRL/HAL source-tree split ASTRA §6.4/§7.11 has
   been asking for ("no `kernel/srl`/`kernel/hal` top-level dirs exist yet"),
   applied on the userland side.

Sequencing note: this phase is what makes Phase 9's daemon a *general* pattern
rather than a one-off for nxexec.

## Phase 10 — POSIX/libc completion as a repeatable gate — TODO
Maintainer: "TUTTO quello che riguarda la libreria e posix e il suo
completamento è di tuo interesse ... la maggior parte esiste già a livello
kernel, va standardizzato, studiato e portato a termine.  POSIX vive SOPRA il
nostro sistema come layer di compatibilità."
- Keep the POSIX personality a thin mapping over `OS1_*`/`OS1low_*`; never a
  parallel implementation.
- Make the declared-vs-implemented audit (nm over the libc objects vs the
  prototypes in our headers) a REPEATABLE check, so the isatty/_Exit/fdopen
  class of "declared but nobody implemented it" cannot come back.
- Same for force-included headers as make prerequisites (the lua.o stale-object
  class, 2026-07-17v).

## Phase 11 — Users / capabilities / filesystem — BLOCKED (design doc first); absorbs Ph.4(b)
Maintainer put users/capabilities/filesystem in scope, "da studiare su ASTRA;
la maggior parte esiste già a livello kernel".  Sequenced AFTER the doc that
PIANO-LIBC-ASTRA R6 requires ("studio kill-model/exec in kernel — doc dedicato
PRIMA del codice"), and bounded by its decisions 3 and 4 (nxauth v1 = root with
preset password, named users NOT in this block; per-user VFS = future block).
Absorbs the old Phase 4(b) "root-vs-user process division".

## Phase 5 — fd / object / registry standardisation — **2 of 3 items ALREADY DONE**
- **kernel**: make every `fd` a handle uniformly; expose process/job state
  through the registry namespace so tools read it as files.
- **lib**: the POSIX fd calls are a thin compat over `OS1_object_*`.

## Phase 6 — nxline / nxjobs — model **DONE**, PORTING todo (they are NOT stubs)
- Back `nxjobs` with Phase 2 exit_code/state; `nxline` history via standard
  paths. Standardise both and port to the other userland tools (kilo, lua REPL,
  nxexec host loop) so line-editing/job-control are shared, not re-hand-rolled.

## Phase 7 — doom revert + lua finish — TODO (depends on 9/10)
- Revert doom's old-libc adaptations to the standard API; finish the lua port
  on the now-standard I/O + process model.

## Phase 8 — FOLDED INTO PHASE 3 (was a duplicate; kept for traceability)
- nxexec provides the display name + icon to the bar/dock; stable process
  serialisation end to end.

---

## Testing
No cross-compiler-driven GUI automation here — QEMU is driven by the
maintainer. Agent boots `build/aarch64/disk.img` and requests specific shell
commands / actions per phase. `coccinelle` (`spatch`) available for
mechanical, uniform refactors (e.g. routing error returns through
`__libc_report`).
