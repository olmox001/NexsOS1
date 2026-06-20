# NEXS-DIR-01 — OS1_/POSIX naming split + object handles

## Problem
The base API (`os1.h`) and the POSIX/libc surface are not lexically
distinguishable. `sleep()` meant the proprietary millisecond sleep while POSIX
`sleep()` is seconds — a silent unit clash. Names like `set_focus`, `window_of_pid`
also leak the wrong object (see DIR-02).

## Direction
Two clearly separated layers, distinguishable by name:

* **OS1 base API** — every proprietary, non-POSIX primitive is prefixed `OS1_`.
  It is the stable surface apps link against (the "Application Model").
* **POSIX/libc** — real `sleep`/`nanosleep`/`read`/`write`/… built ON TOP of the
  OS1 primitives. The bare, unprefixed names belong here.

### Pilot (DONE)
`sleep(int ms)` → **`OS1_sleep(int ms)`** across `os1.h`, `lib.c`, and all callers.
This frees the bare name `sleep` for a future POSIX `sleep(unsigned seconds)` and
proves the mechanical rename + build-green loop on both arches.

### Next task — complete the rename
Mass-rename the remaining OS1 base-API verbs to `OS1_` (one mechanical pass,
build-green on both arches, no behavioural change):
`yield, spawn, spawn_args, spawn_caps, spawn_level, kill_process, wait,
create_window, destroy_window, window_draw, window_blit, set_window_flags,
compositor_render, set_focus, flush, notify, registry_read, registry_write,
get_time, get_pid, print, printf_win, …`
Provide thin POSIX/libc shims with the bare names where a real POSIX equivalent
exists, so ported software keeps compiling.

## Object handles (everything is an object)
Replace bare `int` ids with typed, uniform handles so the API talks about objects,
not integers:

```
OS1_window_t   OS1_surface_t   OS1_process_t
OS1_file_t     OS1_socket_t    OS1_service_t   (a generic OS1_handle_t underneath)
```

GUI/proc/fs/input families become consistent (see DIR-04):
`OS1_window_create/_present/_resize`, `OS1_proc_spawn/_wait/_info`,
`OS1_fs_read/_write/_list/_mkdir`, `OS1_input_wait/_poll`.

## Acceptance
* `grep -rE "\bsleep\(" user include` returns only POSIX-layer references. ✅ (pilot)
* Base API verbs are `OS1_`-prefixed; POSIX shims keep bare names; both arches build.

## Status (2026-06-20)

**DONE** (ASTRA §7.1, header `include/api/object.h`): the "everything is an object"
half of this direction landed as a real, unforgeable-handle capability layer —
not a rename, but the actual object surface this doc called for. Shipped:

* `OS1low_handle_create/_duplicate/_close`, `OS1low_cap_query/_grant`,
  `OS1_object_read/_write/_wait/_ctl` (syscalls 235..243) — a uniform object I/O
  surface over typed kernel objects.
* Object types `OBJ_TYPE_FILE/PROCESS/REGKEY/WINDOW` with namespaces
  `OS1_NS_FS/PROC/REG/WIN`; separable/attenuable rights `OS1_RIGHT_*`. This is the
  generic `OS1_handle_t`-underneath the typed handles in this doc were sketching.

**Remaining (the next structural step, DIR-01/#164)**: the full **call-surface
rename/unification** is still pending — mass-prefix the remaining base-API verbs to
`OS1_`, add POSIX bare-name shims, and fold the legacy syscalls/verbs onto the
`OS1_`/`OS1low_` + capability model so the whole surface is consistent (read the
ASTRA §6.1 ABI tables first so the final names match and the work isn't done twice).

## F4 execution map (2026-06-20) — the family-by-family rename/capability pass

The concrete, audited mapping that drives the remaining work (ASTRA §7.6). It
refines the §6.1 ABI tables against the **actual** symbols in `include/api/os1.h`
today. Convention (from §6 header): **`OS1low_`** = direct kernel entry (stable,
minimal); **`OS1_`** = high-level libos1/SRL surface (versionable). The legacy
POSIX bare names (`open/close/read/write/lseek/exit`) stay as the personality
layer and are **not** renamed.

**Method, one family per micro-phase** (build-green both arches + boot 0-panic +
capability regression `captest`/`capkill`, then commit+push):
1. add the canonical name in `os1.h`, implement in `lib.c` — it wraps the existing
   `_sys_*` stub, so **no new syscall and no behaviour change**;
2. rewrite the bare verb as a one-line **compat shim** calling the canonical
   (zero userland breakage — every existing caller keeps compiling);
3. callers migrate opportunistically, never in a flag-day.

The `M4.5` column marks where the verb later *also* gains a capability/object
path (the ambient-PID path stays as the convenience shim).

### Process — `OS1low_process_*`  **[pilot]**
| bare today | canonical | M4.5 capability path |
|---|---|---|
| `spawn`, `spawn_args` | `OS1low_process_spawn(path,argc,argv)` | — |
| `spawn_caps`, `spawn_level` | `OS1low_process_spawn_caps(path,level,caps)` | — |
| `kill_process` | `OS1low_process_kill(pid)` | `OBJ_CTL_KILL` (done) |
| `wait` | `OS1low_process_wait(pid)` | `OS1_object_wait` on PROCESS |
| `yield` | `OS1low_process_yield()` | — |
| `get_pid` | `OS1low_process_self()` | — |
| `exit` | `OS1low_process_exit(status)` | — (bare `exit` also kept, POSIX) |
| `get_procs` | `OS1_process_enum(buf,max)` | — (high-level introspection) |

### Time — mostly done
`OS1_sleep`, `os1_mono_ns`, `os1_cpu_ns` already prefixed. `get_time` →
`OS1_time_now()`. POSIX `nanosleep`/`clock_gettime` stay (personality).

### IPC — `OS1low_ipc_*`
`send`→`OS1low_ipc_send`, `recv`→`OS1low_ipc_recv`, `try_recv`→
`OS1low_ipc_try_recv` (M4.5: PORT objects, `OBJ_TYPE_PORT` reserved).
`notify`→`OS1_notify_post` (high-level SRL).

### Window / graphics — `OS1_window_*` / `OS1_gfx_*`
`create_window`/`destroy_window`/`window_draw`/`window_blit`/`window_write`/
`window_of_pid`/`window_grid`/`set_window_flags`→`OS1_window_*`;
`set_focus`→`OS1_window_set_focus` (unify with `OBJ_CTL_FOCUS`, closes
OBJ-WIN-FOCUS); `draw`/`flush`/`compositor_render`→`OS1_gfx_draw/_flush/_render`.
(enum/minimize/restore/focus/close already `OS1_window_*`.)

### Display — `OS1_display_*` (today `_sys_*` only, no bare wrapper)
`display_info`→`OS1_display_info`, `set_display_mode`→`OS1_display_set_mode`,
`display_poll`→`OS1_display_poll`, `window_resize`→`OS1_window_resize`,
`set_style`→`OS1_display_set_style`, `set_zoom`→`OS1_display_set_zoom`.

### Registry — `OS1_registry_*` (M4.5 path = REGKEY, done)
`registry_read`→`OS1_registry_get`, `registry_write`→`OS1_registry_set`,
`registry_enum`→`OS1_registry_enum`. `set_font`→`OS1_display_set_font`.

### Filesystem — `OS1_fs_*` (M4.5 path = `OBJ_TYPE_FILE`, `OS1_NS_FS` exists)
`file_write`→`OS1_fs_write`, `file_read`→`OS1_fs_read`, `list_dir`→
`OS1_fs_list`, `chdir`→`OS1_fs_chdir`, `getcwd`→`OS1_fs_getcwd`.

### Memory — `OS1low_vm_*`
`sbrk`→`OS1low_vm_sbrk` (M4.5: `OS1low_vm_map/_unmap/_protect`).

### Order
**process (pilot)** → time → ipc → memory → registry → display → filesystem →
window/graphics (largest, last). Per family, M4.5 capability routing follows the
M4 rename only where the table marks a path.

### Status — M4 DONE (2026-06-20)
The full M4 rename pass is **complete and pushed** on `comprehensive-review`; the
whole libos1 call surface now has OS1_/OS1low_ canonical names with bare-name compat
shims, build-green on both arches, boot 0-panic, captest+capkill clean throughout:

| family | commit | family | commit |
|---|---|---|---|
| (execution map) | `5bb569e` | memory + filesystem | `cac28f4` |
| process (pilot) | `ed0f1ba` | display (+ callers migrated) | `a4398f9` |
| time + ipc + registry | `3302c11` | window / graphics | `0a1bf80` |

### Status — M4.5 easy wins DONE (2026-06-20)
The low-risk capability/object routing (real behaviour, not just names) is shipped for
the two families whose objects already existed:

| step | commit | what changed |
|---|---|---|
| process `wait` → capability | `5711cc0` | `OS1low_process_wait` acquires a WAIT-only PROCESS handle + `OS1_object_wait`; kernel separated wait-right from kill-right (seL4) so a non-destructive handle no longer needs kill authority. kill/IPC-send acquisition stays gated (capkill/capipc still pass). |
| file read → capability | `95565da` | `OS1_fs_read` (size>0) routes through `handle_create(FS,READ)` + new `OBJ_CTL_SEEK` + `object_read`; size-probe stays ambient. `OBJ_CTL_SEEK` completes the FILE capability for random access. |

(process `kill` → `OBJ_CTL_KILL` and registry → `OBJ_TYPE_REGKEY` already shipped pre-F4.)

**Remaining M4.5 = the larger sub-projects** (deferred, deliberately not "easy wins"):
- **file write → capability** — blocked on `handle_create(FS)` requiring an existing
  file (vfs_open); needs O_CREAT support so routing write doesn't break file creation
  (`NOTE(M4.5-FS-WRITE)`, ASTRA §6.8 `open(O_CREAT)` → `handle_create`).
- **IPC → `OBJ_TYPE_PORT`** — Mach-style first-class ports; the type is reserved but
  not implemented. A real new kernel subsystem (closer to Phase C).
- **memory → `OS1low_vm_map/_unmap/_protect`** — depends on the B2 VM model maturing.
