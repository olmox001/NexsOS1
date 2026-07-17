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
- **doom loops on savegame LOAD** (write now works via real `rename()`). Two
  hypotheses ruled out: header-last write (doom writes sequentially + renames)
  and the 16 MiB syscall cap (savegame is ~176 KB). Root cause needs runtime
  visibility → **blocked on Phase 0**; likely a doom-side hack vs standard I/O
  (Phase 7) once visible.
- **lua test children stay SLEEPING** → nested spawn (lua → `nxshell -c` →
  lua) has no controlling terminal / stdin relay, so the inner lua blocks on
  input forever and the parent's `wait()` never returns → **Phase 4**.
- `nxline.h` / `nxjobs.h` are STUBS → **Phase 6**.

---

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

## Phase 0 — Error visibility (library) — START HERE
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

## Phase 1 — Truncation model = standard (kernel + lib + port)
Decide + enforce the standard: truncation is an OPEN-time op (O_TRUNC /
`fopen("w")`), not a per-write side effect (POSIX pwrite never truncates).
- **kernel**: keep `open(O_TRUNC)` (already added). Re-evaluate the
  `ext4_write` "offset 0 truncates" behaviour — keep as pragmatic replace, or
  gate it behind a real truncate primitive; document the decision.
- **lib**: `ftruncate()`/`truncate()` over the primitive; `fopen("w")`
  truncates at open only.
- **port**: adapt apps that assumed write-time truncation to the standard API.

## Phase 2 — Kernel process primitives (ASTRA)
- **kernel**: per-process `exit_code`; `SYS_WAIT` returns it (payload), so
  `system()` and jobs report real status ("Done (0)"/"Exit 1").
- **kernel**: `STOPPED` process state + resume, so Ctrl-Z/`bg`/`fg` are real
  (today only running/zombie exist).
- **lib**: `waitpid`/`WEXITSTATUS` compat over the payload; `OS1_process_wait`
  NEXS surface.

## Phase 3 — Process identity & serialisation
- **kernel**: widen/clean the `proc->name` field (16 vs 32 mismatch in
  sysstats), add a stable per-process serial; expose identity via the registry
  namespace (`/proc/<pid>` typed object already exists — extend).
- **lib/nxexec**: nxexec assigns the launch identity (display name + icon key)
  and records it, so the bar/dock/jobs all read ONE stable name, not a
  path-basename that collides ("lua", "lua", "lua").

## Phase 4 — TTY / input routing + bg/fg abstraction
- **kernel/lib**: controlling-terminal model so a hosted (windowless) child —
  including a NESTED spawn — receives stdin (fixes lua SLEEPING). Unify with
  `nxexec_run_foreground()`'s echo/relay (currently separate).
- **model**: integrated-background vs detached on spawn/fork; `jobs` tracks
  separated processes too; "re-attach" a separated process into job control.
- **kernel**: root-vs-user process division (level presets exist per path;
  formalise the identity split).

## Phase 5 — fd / object / registry standardisation (ASTRA §6.2)
- **kernel**: make every `fd` a handle uniformly; expose process/job state
  through the registry namespace so tools read it as files.
- **lib**: the POSIX fd calls are a thin compat over `OS1_object_*`.

## Phase 6 — nxline / nxjobs real model + port to userland
- Back `nxjobs` with Phase 2 exit_code/state; `nxline` history via standard
  paths. Standardise both and port to the other userland tools (kilo, lua REPL,
  nxexec host loop) so line-editing/job-control are shared, not re-hand-rolled.

## Phase 7 — doom revert + library conformance + lua finish
- Revert doom's old-libc adaptations to the standard API; finish the lua port
  on the now-standard I/O + process model.

## Phase 8 — naming → bar / icons via nxexec
- nxexec provides the display name + icon to the bar/dock; stable process
  serialisation end to end.

---

## Testing
No cross-compiler-driven GUI automation here — QEMU is driven by the
maintainer. Agent boots `build/aarch64/disk.img` and requests specific shell
commands / actions per phase. `coccinelle` (`spatch`) available for
mechanical, uniform refactors (e.g. routing error returns through
`__libc_report`).
