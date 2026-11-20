# Session 3 — verified state and ownership (2026-11-19)

**Scope:** local `SPERIMENTAL-LINUX`. The three primary component
repositories are NX-CORE (`kernel/`), NX-SDK (`system/`) and
NX-USERKIT (`user/USERKIT/`). The umbrella NexsOS1 repository owns
integration, system images and pinned gitlinks. A Coreutils external
submodule and its Gnulib child also have local changes: commit their
source and gitlink from the deepest submodule outward. An umbrella
`git add` cannot save uncommitted submodule source.

## Implemented and exercised

- Physical display modes come from the device provider. The logical
  desktop has Auto (hardware-linked) and Manual base dimensions,
  independent of zoom. No application-owned fixed 640x480 fallback.
- The `surface/layout-v1` object resource is generated from
  `kernel/contracts/surface-v1.json`, mirrored in NX-SDK and checked
  for wire compatibility (ABI r6, SDK API r8, USERKIT r3).
- Generic top/bottom anchors, reservations and Z priorities are
  keyed to existing owner-controlled surfaces, not executable names.
  Owner/privilege checks deny registration of other PIDs' surfaces.
  Ordinary windows are bounded to the available workarea while
  preserving the original source/presentation request.
- `nxbar` and `nxui` stay in NexsOS1 `sys/bin`, not USERKIT. Both
  retain the same surface/window identity across resizes, and
  publish replacement buffers only after successful allocation and
  layout update. The top strip's reservation is independent of
  menu/notification popup height.
- USERKIT `nxcompositor` still renders generic window snapshots.
  `nxmoused` does not treat overlay corners as user-resizable grips.
  The kernel's window compatibility path remains transitional.
- The shared real UTMP record and freeable zero-length allocation
  fix Gnulib runtime regressions; some Coreutils/POSIX facilities
  remain incomplete.

## Remaining ownership boundaries

The kernel's transitional window/graphics functions cannot be
removed until HAL display, emergency terminal, NXPAX/fault reporting,
device and surface consumers have been verified and migrated.
No new syscalls were introduced in this iteration. This is a
diagnostic milestone, **not** a full Power12 qualification.

See [VERIFICATION.md](VERIFICATION.md) and [ISSUES.md](ISSUES.md).
