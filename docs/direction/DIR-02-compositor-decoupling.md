# NEXS-DIR-02 — Compositor ↔ process/scheduler decoupling; compositor in the HAL

Extends #83/#67/#69 (decouple scheduler ↔ compositor).

## Problem
The compositor is coupled to process management in both directions:

* **Apps speak in PIDs about windows.** `set_focus(get_pid())` couples the process
  manager to the compositor; `window_of_pid(pid)` leaks the PID↔window association.
* **Apps drive the compositor.** `window_blit()` + `compositor_render()` makes the
  application responsible for compositor frame production.
* **The scheduler reaches into the compositor.** `schedule()` calls
  `compositor_get_focus_pid()` for the focus boost (SCHED-01,
  `kernel/sched/process.c`) — a kernel→compositor dependency.

## Direction

### 1. Window-centric API (objects, not PIDs)
Everything about focus / minimise / fullscreen / z-order / drag speaks only of
windows:
```
window_focus(win)   window_activate(win)          // not set_focus(get_pid())
process_get_primary_window(pid)  window_get_owner(win)   // explicit relation
```

### 2. The compositor is not driven by apps
Apps only declare "this surface is ready":
```
window_present(win)   /  window_commit(win)
```
The compositor owns its **own scheduler, frame pacing, dirty regions, and vsync**,
independent of app render loops — enabling 60/120 Hz, static windows that don't
redraw, and compositor-driven animations. (Wayland-like.)

### 3. Invert the scheduler↔compositor dependency
The focus boost must not be a kernel→compositor call. A userland policy/compositor
service adjusts a focused window-owner's priority via a capability
(`CAP_SCHED_HINT`), so the kernel scheduler has no compositor dependency.

### 4. Compositor as a HAL component
```
Kernel : scheduler · mm · ipc · vfs
HAL    : gpu · input · compositor · core-restart
Userland: shell · browser · editor · terminals
```
Placing the compositor in the HAL lets a **core restart preserve the framebuffer
and graphics state** across a kernel-core recovery (see DIR-05), and matches the
"compositor as a fundamental component" thesis.

## Acceptance
* No `kernel/` code calls `compositor_get_focus_pid()` in the schedule path.
* App-facing API exposes `window_*` verbs; no `set_focus(get_pid())` /
  `window_of_pid()` in userland.
* Compositor frame production is timer/vsync-driven, not app-`compositor_render()`-driven.

## Status (2026-06-18, GFX-DYN-01 pass)
* **Done** — scheduler→compositor inversion (SCHED-01 #83): `schedule()` reads
  the scheduler-owned `keyboard_focus_pid` hint; the compositor only pushes it
  down via `sched_set_focus_pid()` (#67).  `compositor_get_focus_pid()` was
  **removed** (dead).  Frame production is timer-driven (`compositor_tick`).
  The PID↔window relation is the single explicit seam
  (`compositor_get_window_by_pid` / `compositor_primary_window_of_pid`).
* **Done** — the terminal emulator is a self-contained engine (`term.c`/`term.h`),
  no longer tangled in the compositor (#123 part 1).
* **Remaining** — full window-handle ABI (apps still address windows by PID-
  derived ids; `window_present/commit` verbs); compositor leaving the kernel as a
  HAL service.  Tracked with the desktop/mobile compositor transform.
* See **DIR-07** for the surface model, resize and Policy/Style/Theme that build
  on this seam.

## Status (2026-07-02)

* **Done** — `compositor_get_focus_pid()` cleanup: removed from the canonical
  compositor. Historical alternate compositor sources were deleted on
  2026-11-10 after terminal parsing was confirmed in `kernel/graphics/term.c`
  and Material/shadow policy in the userland graphics/theme stack.
* **Done** — `compositor_update_mouse` (`kernel/graphics/compositor.c:1356-1466`)
  was the last unlocked window-list mutator; it now takes `compositor_lock`,
  closing a drag/resize torn-write race (commit `492e5ec`).
* **Still open, unchanged** — window-centric API (`window_focus`/`window_activate`
  instead of PID-derived addressing), `window_present`/`window_commit`, and
  compositor-as-HAL-component. No `kernel/hal`-level compositor placement exists
  in the source tree as of this date.


## Architecture decision update — 2026-11-03 (supersedes HAL placement)

The old §4 diagram ("compositor in the HAL") is **superseded** for NEXS-NG.
The compositor is a ring-3 supervised userland service, not a kernel/HAL
component. The scheduler has no compositor dependency.

The kernel retains a deliberately narrow surface/buffer/control manager:
capability-owned backing storage, validated surface metadata, safe mappings,
rights, lifetime/pinning, synchronization, commit validation and controlled
presentation hand-off. It performs no composition or pixel rasterization.
Window geometry, z-order, focus, damage policy, frame pacing, vsync policy,
decorations, cursor composition and animation belong to `compositord`.

This is a stronger boundary than the original DIR-02 "compositor as HAL
component" thesis. The legacy compositor remains in the current kernel only
for compatibility until the parallel NG service stack passes its migration
gates. See `docs/architecture/GRAPHICS-USERLAND-MIGRATION.md`.
