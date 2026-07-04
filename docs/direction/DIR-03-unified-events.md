# NEXS-DIR-03 — One event model: `event_wait()`

## Problem
Waiting is fragmented across three unrelated primitives:
* input → `read(0)`
* IPC → `try_recv()`
* timer → `OS1_sleep()` / nanosleep

An app that wants to react to a key, a message, and a timeout must juggle three
mechanisms (and today often busy-polls, which is exactly what produced the
notify_srv re-render and the yield-spin throttle that TIMER-UAF-01 exercised).

## Direction
Collapse all blocking into a single event primitive:

```c
OS1_event ev;
OS1_event_wait(&ev);   // blocks until ONE event is ready, ~0% CPU idle

enum {
  EVENT_KEY, EVENT_MOUSE, EVENT_IPC,
  EVENT_TIMER, EVENT_WINDOW, EVENT_PROCESS,
};
```

A single event loop replaces input/IPC/timer/window/process polling. This is the
userland counterpart of the kernel's `io_poll` bring-up contract and the recv +
real-blocking-sleep work already landed; it generalises a future
**recv-with-timeout** (issue #135) into one call.

Benefits:
* One idiom for every app and service (notify_srv, shell, editors, browser).
* No busy-poll loops by construction — removes whole classes of the
  yield-spin / re-render defects.
* Natural fit with the compositor (`EVENT_WINDOW`: resize/expose/close) and the
  service model (`EVENT_IPC`, `EVENT_PROCESS`).

## Migration
1. Implement `OS1_event_wait` over the existing blocking IPC + the per-process
   timer (sleep deadline as `EVENT_TIMER`) + input fd readiness.
2. Port notify_srv and the shell to the single loop as the reference adopters.
3. Deprecate direct `read(0)`/`try_recv()`/sleep-poll patterns in app code.

## Acceptance
* notify_srv and shell run on a single `OS1_event_wait` loop, no `try_recv`+sleep poll.
* A timeout is expressible as `EVENT_TIMER`, removing the visible-branch poll in notify_srv.

## Status (2026-06-20)

**DONE — input base** (ASTRA §7.5, header `include/api/input.h`): windowed apps now
read keyboard, mouse and resize through **one** API, `input_poll_event(input_event_t *)`.
The single `input_event_t` union carries all three (`INPUT_TYPE_KEYBOARD/MOUSE/RESIZE`),
decoded from the underlying IPC transport types `IPC_TYPE_INPUT` / `IPC_TYPE_MOUSE` /
`IPC_TYPE_RESIZE`. Mouse buttons are the shared evdev codes `MOUSE_BTN_LEFT/RIGHT/MIDDLE`
and key states `KEY_PRESSED/RELEASED/REPEAT`, centralised in `input.h` so every app
matches the same constants. The compositor delivers events to the **focused** window.

This is the unified *input* leg of `OS1_event_wait`: one poll covers key+mouse+resize
instead of three primitives. The full blocking `OS1_event_wait` that also folds in IPC,
timer, window and process readiness (~0% idle, no busy-poll) is still the target.

**Remaining**: per-window mouse delivery **beyond the focused window**; a system-driven
**desktop-resize broadcast** to apps (tracked with DIR-07); and the unified blocking
`OS1_event_wait` loop with the `EVENT_*` set above (notify_srv/nxshell as adopters).

## Status (2026-07-02)

**No change** — verified `grep -rn OS1_event_wait include/ user/ kernel/` returns
nothing; the full blocking event loop is still unimplemented. `nxshell`/
`nxntfy_srv` (renamed from `notify_srv`, see DIR-05/ASTRA §7.7) still run their
own poll/sleep loops, not a shared event primitive. Per-window mouse delivery
beyond focus and the desktop-resize broadcast remain open, matching DIR-07's
status. This direction is unchanged since 2026-06-20.
