# 2026-07-11 — design: modular input architecture and the nxinput daemon

Design record requested by the maintainer (documentation is append-only).
No code accompanies this document: like the GPU-context design, the split is
recorded BEFORE implementation so each increment lands against an agreed
shape. Direction set by the maintainer: separate device handling from input
logic; runtime-modular device layouts and keyboard schemes; abstract the
duplicated per-arch IRQ plumbing behind a simple ABI per ASTRA; move
high-level input policy into a userland `nxinput` daemon launched by init.

## Today's stack (what we build on)

- Devices: PS/2, VirtIO-input (MMIO on AArch64, PCI on amd64), USB2/USB3
  stubs — each feeds `input_report`, which currently dispatches
  SYNCHRONOUSLY in IRQ context (`input_dispatch`: EV_KEY → layout+IPC,
  pointer → compositor). The DIR-02/DIR-03 comments already anticipate an
  input-server thread; the ring (`input_ring`) exists.
- Keyboard layouts are partially implemented in-kernel
  (`keyboard_layout_t`, US/IT tables, KBD-LAYOUT-01) but selection is
  compile-time-ish and per-device mapping does not exist.
- Delivery to apps is one fixed policy: translated `IPC_TYPE_INPUT` /
  `IPC_TYPE_MOUSE` / `IPC_TYPE_RESIZE` messages to the focused pid.

## Target split (ASTRA: mechanism in kernel, policy in userland)

1. **Device tier (kernel, per-bus drivers).** PS/2, VirtIO, USB2/USB3
   drivers do ONLY: IRQ handling, hardware protocol, normalisation to one
   record — `(source_device_id, type, code, value)` in evdev vocabulary.
   They stop calling subsystem sinks (compositor/keyboard tables) directly;
   they push into the existing input ring. The per-arch IRQ differences
   (GIC vs APIC wiring) stay inside each driver's probe path — the record
   format is the ONE arch-independent seam, so nothing above the ring knows
   about IRQs at all.
2. **Routing tier (kernel, thin).** The ring consumer forwards raw records:
   - WM-reserved events (pointer buttons/motion for drag/resize/focus,
     titlebar clicks) to the compositor — the kernel keeps only what the
     scene service itself needs to stay coherent;
   - everything else to the subscribed input service (nxinput) through the
     existing IPC transport. If nxinput is not running, a built-in fallback
     keeps today's behaviour (translated events to the focused pid), so the
     system stays usable during boot and in recovery.
3. **Policy tier (userland: nxinput daemon).** Launched by init like the
   notification service (the project-standard daemon methodology:
   `/sys/bin`, IPC service loop, capability-gated). Owns:
   - keyboard schemes/layouts (moves the `keyboard_layout_t` tables out of
     the kernel; runtime-selectable per device via nxsettings/nxreg);
   - per-device input profiles (mouse acceleration, tablet absolute
     mapping, future gamepads via USB);
   - high-level transformation (dead keys, compose, key repeat policy,
     UTF-8 production) and delivery to the focused app in the SAME
     `IPC_TYPE_INPUT`/`IPC_TYPE_MOUSE` message shapes apps consume today —
     the app-facing ABI does not change (maintainer constraint: optimise
     the existing API, do not replace it).

## Increments (in the programme's controlled style)

1. Ring-only dispatch: `input_dispatch` becomes the ring consumer running
   out of IRQ context (the DIR-02 thread), same behaviour, both arches
   validated with `make run`.
2. Device records gain `source_device_id`; PS/2, VirtIO and USB report
   through the same normaliser.
3. nxinput skeleton: init-launched daemon subscribing to the routing tier;
   keyboard translation moves to it behind the unchanged app ABI; kernel
   fallback retained.
4. Layout/scheme registry: nxinput reads layouts from the filesystem
   (connecting the partially-implemented layout tables), nxsettings gains
   the selector UI.
5. Only then: gamepad/joystick classes for SDL, and the SDL threads
   decision (needed for SDL_AddTimer) evaluated against real OS1 threading
   primitives.

Constraint reminders: no ambient-authority input syscall; the daemon
subscribes through capability-checked IPC like every service; a feature is
not "done" until `make run` passes interactively on both architectures.
