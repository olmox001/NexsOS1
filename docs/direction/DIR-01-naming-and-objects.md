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
