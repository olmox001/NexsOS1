# 2026-07-11 — SDL2 input: mouse motion delivery and full keyboard map

New increment record (documentation is append-only). Priority set by the
maintainer: validate SDL input before opening the OpenGL/D3D9 fronts.

## Mouse motion, without new ABI

The compositor delivered mouse events to the focused app ONLY on button
press/release (`compositor_handle_click`); apps never saw pure motion. Per
the maintainer's directive ("non cambiare l'API esistente, ottimizzala"),
motion now rides the EXACT same `IPC_TYPE_MOUSE` message from
`compositor_update_mouse`:

- `data1 = 0` (no button) marks a motion-only event; every existing consumer
  already treats a zero button as not-a-click, and the SDL2 adapter emits
  `SDL_MOUSEMOTION` for it. No new syscall, message type or field.
- Coordinates are window-relative LOGICAL coords, mapped through the
  draw-rect scaling exactly like the click path.
- Delivery only while the cursor is inside the focused window's content
  area, never during drag/resize, rate-limited to >= 8 ms (~125 Hz) via
  `mono_ns()` so the unbounded per-process IPC queue cannot be flooded from
  mouse-IRQ context. `kernel_ipc_send` runs after the compositor unlock,
  mirroring the click path's deadlock-avoidance contract.

## Keyboard map completed

The overlay's evdev→SDL scancode table grew from 9 keys to the full main
block: letters, digits, punctuation, modifiers (shift/ctrl/alt), F1–F12,
navigation cluster (home/end/pgup/pgdn/insert/delete) and arrows. Unmapped
codes stay UNKNOWN; printable input continues to arrive via the UTF-8 text
channel (`SDL_TEXTINPUT`).

## sdltest as the input harness

The test window now visualises every input class:

- black crosshair follows `SDL_MOUSEMOTION`;
- left click turns the 3 px border green, right click red
  (`SDL_MOUSEBUTTONDOWN`, position logged on serial);
- arrow keys move the magenta square (`SDL_KEYDOWN` + scancode names logged);
- typed text logged via `SDL_TEXTINPUT`; ESC quits.

## Validation

`make` / `make ARCH=amd64` green. Headless QEMU boots on both architectures:
7 kernel tests passed, 0 failed, no panic, full service startup — the
motion-delivery kernel change does not disturb the boot path. Interactive
checklist for the maintainer's next `make run` (both arches): launch
`sdltest`, move the mouse inside the window (crosshair tracks), click
left/right (border green/red), arrows (square moves), type letters (serial
shows scancode names and text), ESC exits cleanly; verify dock/launcher and
window drag still behave with the new motion traffic.
