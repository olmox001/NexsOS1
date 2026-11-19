# Surface layout plugins — object resource contract

## Ownership and scope

The compositor is an independent USERKIT service. It renders generic
window snapshots, does not identify executables and does not depend on NexsOS1
apps. The kernel's transitional window storage remains responsible only for
capability-checked window metadata, placement bounds and surface ownership.
NexsOS1-specific `nxbar` and `nxui` remain in
`user/NexsOS1-main/sys/bin`: they **register** overlays, rather than becoming
dependencies of USERKIT or adding a second renderer.

This is a capability-backed **surface layout plugin protocol**, not a
dynamically linked executable plugin. It uses the existing object interface,
never new syscalls. The canonical wire schema is
`kernel/contracts/surface-v1.json`; the generator writes the matching
`kernel/include/abi/surface.h` and `system/include/contract/surface.h`.

## Protocol

`OS1_RESOURCE_SURFACE_LAYOUT` (`surface/layout-v1`) supports exact-sized
`os1_surface_layout_v1` reads and writes.

- **Write:** `OS1_surface_layout_apply(id, anchor, layer, order, margin,
  reserved, width, height)` configures an *existing*, caller-owned surface.
  Requires ROOT/MACHINE privilege, write rights and the exact owning PID; unlike
  `CAP_WINDOW_MANAGE`, this does not authorize changes to other processes'
  windows or broaden the service's startup capability grant. It updates
  the existing window, preserving its ID and replacing its source pixel buffer
  only through the existing checked resize path.
- **Read:** `OS1_surface_layout_query(id, &state)` returns the resolved
  position, presented size, and global workarea. `id=0` queries the workarea
  without requiring any registered plugin.
- **Anchor:** TOP places the surface at its top margin; BOTTOM tracks the
  desktop's logical lower edge. Horizontal placement is centered. A change in
  hardware mode or logical workspace (Auto/Manual/zoom) resolves the anchor
  immediately, even before the app redraws or polls the new dimensions.
- **Reservation:** `reserved` is the persistent strip depth; `margin` is
  added to the claimed edge. Menus, popups and notifications may extend past
  that strip without shrinking the workarea. Reserved regions are derived
  exclusively from visible, registered surfaces. Disposing of the surface
  disposes of the registration.
- **Z:** OVERLAY is chromeless and sorted above normal applications;
  `order` is a relative overlay priority (bounded by the kernel window cap).
  Focus changes must not silently replace this priority. Two independent
  plugins may select their priorities without hardcoding executable names into
  the compositor.

The generic kernel boundary neither owns the plugin's pixels nor draws chrome.
Only a surface's **presented** application extent is clipped to the current
workarea; its original logical pixels and requested dimensions are retained,
so resizing back to a larger desktop restores their previous extent.

## Current registrations

- `nxbar`: top edge, `BAR_MARGIN_TOP`, reserve only `BAR_H`, overlay
  order 20. The notification/menu dropdown grows the same window, without
  changing the permanent reserved strip or recreating a new window ID.
- `nxui`: bottom edge, `DOCK_MARGIN_BOTTOM`, reserve `DOCK_H`, overlay
  order 10. The dock resizes the same window, with an atomic app-buffer swap
  after a successful layout update; if allocation fails, its previous
  framebuffer and geometry remain valid.

These priorities and dimensions are owned by NexsOS1 userland design, not
the compositor. Other projects may register a different set of components.

## Testing and remaining migration boundaries

`gfxcontroltest` exercises registration, workarea release, resolved anchors,
Z ordering, permissions, and reflow after a manual logical-mode switch through
the real resource provider in QEMU. Validate screenshot plus nxadb tests after
changing physical modes and zoom on **both** amd64 and aarch64.

The window creation API and the transitional backing pixel store still use
existing legacy compatibility paths. This contract does not purport to have
completed full object-only window creation or eliminated remaining legacy
syscalls. Do not introduce a parallel surface manager to work around that
migration. Do not replace the NXPaX fault route or suppress diagnostics.

## Verified on real QEMU guests (2026-11-19)

- **AMD64/GRUB BIOS:** the rebuilt kernel reached the shell; all 36 kernel
  tests passed, and `gfxcontroltest` passed 12/12. The full guest suite
  passed 19/20: Coreutils remained the sole failed group. Transcript:
  `build/ui-layout-verified-amd64-uart.log`. Build transcript:
  `.git/ui-layout-verified-amd64-run.log`.
- **AArch64/GRUB UEFI:** all 37 kernel tests passed; `gfxcontroltest`
  passed 12/12, including oversize window bounds, foreign PID denial,
  workarea/reflow, Z and plugin lifecycle. The guest suite passed 19/20,
  again with Coreutils the sole failed group. Transcript:
  `build/ui-layout-verified-aarch64-uart.log`; build:
  `.git/ui-layout-verified-aarch64-run.log`.
- **Native `nxadb` AArch64 interactive:** checked `nxres modes`, queried
  auto/manual logical desktop state, applied zoom 300% on 1280×800 hardware
  (logical 426×266), changed the physical output to 2048×1152 at zoom
  100% (logical 2048×1152), then zoom 150% (logical 1365×768). Inspected
  QMP screenshots with the dock and bar anchored at both screen extremes,
  including after all changes. Restored 1280×800 Auto/100% and verified
  physical and logical dimensions again. Captures:
  `build/runs/aarch64/iso-uefi/ui-layout-zoom-300.png`,
  `ui-layout-highres-100.png`, `ui-layout-highres-150.png`,
  `ui-layout-restored.png`.
- **Host checks:** `make -o power12 check ARCH=amd64` and
  `ARCH=aarch64` both passed, including real NXPAX stress with one
  million and two million concurrent-writer events per architecture.
  See `.git/ui-layout-host-{amd64,aarch64}-recheck.log`.
- **Power12:** the changed-code gate has 0 blocking findings across
  67 scoped files (47 report-only findings). The global audit still reports
  3 build policy, 386 identifier/limit and 5,171 AST findings. These are
  *diagnostic*, not globally Power12-qualified builds. No persistent rule
  exclusions, source assertions, fake tests or new syscalls were added.

**Remaining independent problems:** `coreutilstest` fails `nice`
(`execvp` is still a libc `ENOTSUP` stub) and `nohup` (no real
`/dev/null` route for its stdin redirection). Neither should be "fixed"
by false success or simulated POSIX semantics. At 300% zoom, legacy shell
text still exhibits visible pixel/reflow degradation despite correct
surface anchors; this needs a separate terminal/scale/render audit. Native
`nxadb` interaction was successful after the boot suite had released the
terminal; sending commands while the test runner held focus timed out, so
the prompt/focus handoff is not yet fully robust.
