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
