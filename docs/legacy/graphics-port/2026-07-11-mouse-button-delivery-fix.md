# 2026-07-11 — mouse buttons: release delivery and right/middle unblocked

New increment record (documentation is append-only).

## Two kernel-level defects (not regressions — latent since the beginning)

1. **Button RELEASE was never delivered to apps.** `compositor_handle_click`
   used the button-up edge only to end drag/resize and returned. Any
   button-state consumer (SDL tracks held buttons) saw the first press and
   then treated every later press as "still held" — the observed
   first-click-only behaviour.
2. **Right and middle buttons were dropped in the input driver.**
   `input_dispatch` forwarded only `BTN_LEFT` to the compositor; `BTN_RIGHT`
   / `BTN_MIDDLE` hit a "no consumer yet" branch and vanished. Right-click
   could never work in any app.

## Fixes (same IPC message, no new ABI)

- The release path now also sends the focused app the standard
  `IPC_TYPE_MOUSE` message with `data2 = 0` and clamped window-relative
  logical coordinates (the pointer may be released outside the window).
- `input_dispatch` forwards all three pointer buttons to
  `compositor_handle_click`.
- Click-logic improvements in the compositor, as requested:
  - WM actions are LEFT-button only: drag start, grip resize, titlebar
    buttons, and drag/resize end on release. A right/middle release can no
    longer cancel an ongoing left-drag.
  - App delivery of presses is CONTENT-area only: chrome presses (titlebar,
    buttons, drag) belong to the window manager and no longer leak into the
    app's input stream with negative rel_y.
  - Right/middle presses still focus and raise the clicked window.

`sdltest` now logs both `DOWN` and `UP` with coordinates, so press/release
ordering is visible on serial.

## Validation (make run, both architectures, interactive)

- AArch64: 7/7 kernel tests; serial shows repeated
  `mouse button 1 DOWN/UP` pairs and `mouse button 3 DOWN/UP` pairs
  (maintainer clicking live in the QEMU window) — repeatable left clicks and
  working right clicks.
- AMD64: identical behaviour, 7/7, no panic.
