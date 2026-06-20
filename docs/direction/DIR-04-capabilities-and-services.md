# NEXS-DIR-04 — Capabilities not privileges; service-family syscalls; no `fork()`

Extends #79 (4-level privilege model + fine-grained capabilities).

## 1. Capabilities are a property of the process, not the user
Every authority-bearing call checks a capability the process holds:
```
proc_kill(pid)   -> requires CAP_PROC_KILL
proc_spawn(...)  -> CAP_PROC_SPAWN
fs_write(...)    -> CAP_FS_WRITE
notify(...)      -> CAP_NOTIFY
net_*(...)       -> CAP_NET
```
This is consistent with the existing capability work and replaces ambient
"the user is allowed" privilege with "this process was granted this capability."
(Closes the unauthenticated-`srv.notify_pid` class, USR-SEC-01.)

## 2. Services instead of isolated syscalls (consistent families)
Group the flat syscall list into coherent, predictably named families:
```
proc_spawn / proc_wait / proc_info / proc_kill
fs_read / fs_write / fs_list / fs_mkdir
window_create / window_draw / window_present / window_resize
input_wait / input_poll
```
Naming consistency is small in isolation but decisive once the ecosystem grows.
(Pairs with the `OS1_` prefixing in DIR-01.)

## 3. No `fork()` — ever
`spawn()` (optionally `spawn_args()` / `spawn_flags()`) is sufficient and is the
right choice. `fork()` would drag in copy-on-write, `exec`, `dup2`, pipes,
signals, and process groups — the whole UNIX complexity tax NEXS exists to avoid.

## 4. Application Model vs Kernel ABI
Separate two surfaces:
* **Application Model** (stable userlib): `window_create()`, `event_wait()`,
  `fs_read()`, `proc_spawn()`. Apps almost never touch raw syscalls.
* **Kernel ABI** (internal, low-level, minimal, "almost invisible"): may evolve
  freely without breaking applications, because apps depend on the userlib.

## Thesis
Replace UNIX's *everything is a process* with **everything is a service and
everything communicates by events** (see DIR-03):
```
processes → events → services → compositor
```
not
```
processes → syscalls → kernel
```

## Acceptance
* Authority calls are capability-checked, not privilege/uid-checked.
* Syscalls are organised into the `proc_*/fs_*/window_*/input_*` families.
* No `fork()` primitive exists or is planned; `spawn*` is the only process-creation path.

## Status (2026-06-20)

**DONE** (ASTRA §7.1/§7.2/§7.4):

* **Real capability layer** (§1 of this doc). Authority is now an unforgeable handle to
  a kernel object with separable/attenuable rights, not an ambient PID/level identity —
  `OS1low_handle_create/_duplicate/_close`, `OS1low_cap_query/_grant`,
  `OS1_object_read/_write/_wait/_ctl` over `OBJ_TYPE_FILE/PROCESS/REGKEY/WINDOW`
  (`include/api/object.h`). Acquisition is ambient-gated (`CAP_*`), use is
  capability-based; a granted handle delegates exactly its (only-shrinkable) rights.
* **Per-path capability presets** (§4 Application-vs-Kernel separation in practice): a
  binary's VFS location sets its privilege preset — `/sys/bin` = ROOT, `/bin` = USER —
  under the monotonic creator-clamp (no escalation); `/sys/bin` is write-protected.
* **Stratified SRL services**: every system CLI is a reusable helper + thin frontend,
  **secure-by-caller** because the helper only wraps kernel-gated syscalls (adds no
  ambient checks). Examples: `nxres` (display/style/theme), `nxproc` (process mgmt —
  `user/sys/bin/nxproc.h` helper consumed by the CLI and by the shell's `ps`/`top`).
* **No `fork()`** (§3) holds: `spawn*`/`spawn_caps` remain the only process-creation path.

**Remaining**: per-service capability **refinement** (today `/sys/bin` services all start
at the ROOT preset, not yet a reduced per-service mask); the explicit **SRL/HAL
source-tree split** (B5); the consistent `proc_*/fs_*/window_*/input_*` family naming
(§2, pairs with the DIR-01 call-surface refactor); and the planned `nxinfo`/`nxperms`
services on the same stratified pattern.
