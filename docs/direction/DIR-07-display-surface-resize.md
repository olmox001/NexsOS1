# NEXS-DIR-07 — Dynamic display, surface model & resize; compositor look

Implements GFX-DYN-01 (#121) and the terminal-extraction half of #123; resolves
DRV-GPU-02 (#54), DRV-VIRTIO-06 (#49) and DRV-GPU-01 (#53).  Extends DIR-02
(compositor in the HAL) and the ASTRA provider model.

## Problem

Framebuffer, video driver and compositor were hard-coded to **720×1280** — a
portrait mode forced onto every display, no runtime resize, no HiDPI.  The
virtio-gpu driver ignored `GET_DISPLAY_INFO`, `vgpu_set_mode` was a stub, and the
terminal emulator lived tangled inside the 1700-line compositor.

## The layered surface model

The compositor is the single owner of pixels reaching the display.  Resolution
is **not** a property an application sees: an app owns a *logical surface* and the
compositor decides the visible size, scale and position.

```
   App                      draws into its own logical framebuffer
    │
   App framebuffer          (logical size, e.g. 640×480) — owned by the app
    │
   Surface                  compositor's view of the app buffer: on-screen
    │                        draw size (draw_w/draw_h) + position + scale + clip
   Scene graph              z-order, occlusion, damage
    │
   Compositor FB            the desktop-virtual surface (backbuffer)
    │                        may differ from the physical scanout (HiDPI/zoom)
   Scanout FB               the GPU's scanout resource (virtio-gpu)
    │
   Display
```

Two independent scalings fall out of this:

1. **App surface → window draw size.**  When a window's `draw_w/draw_h` differ
   from its logical `width/height`, the compositor nearest-samples the logical
   buffer into the draw rect.  So on resize the app can keep drawing at the old
   size (instant, fluid) and reallocate lazily on the resize event.
2. **Compositor FB → scanout.**  The desktop-virtual size is independent of the
   physical resolution (groundwork for HiDPI, global zoom, instant mode change,
   rotation, multi-monitor).  Today they are kept equal by `compositor_resize`.

### Framebuffer levels

| Level | Owner | NEXS |
|---|---|---|
| Scanout FB | GPU | virtio-gpu resource + DMA backing (`virtio_gpu.c`) |
| Compositor FB | compositor | `compositor_backbuffer` (capacity-preallocated) |
| App FB | app | per-window `buffer` (`compositor_create_window`) |
| Temp buffers | compositor | (future: blur/shadow/animation caches) |

## Resize flows

**Desktop / global resize** (resolution change):
```
nxres W H  ─SYS_SET_DISPLAY_MODE→  gpu_set_mode(W,H)         (virtio-gpu: new
                                     │                         resource+scanout)
                                     └→ compositor_resize(W,H) (retarget backbuffer,
                                                                clamp windows, repaint)
```
**Host-driven** (QEMU window resized): virtio-gpu raises a display-change event;
`init`'s supervisor loop calls `SYS_DISPLAY_POLL` (process context) which applies
the same `gpu_set_mode` + `compositor_resize`.  The heavy work never runs in the
timer-IRQ compositor tick.

**Window resize**: `SYS_WINDOW_RESIZE` → `compositor_resize_window` reallocates
the logical surface, reflows the terminal (`term_resize`), and sends the owner an
`IPC_TYPE_RESIZE` event (decoded as `INPUT_TYPE_RESIZE` by `input_poll_event`).

## Policy / Style / Theme (separation of concerns)

The look is split into three orthogonal axes (`kernel/.../compositor_style.h`):

```
Policy = behaviour   floating · tiled · mobile     (window management)
Style  = form        titlebar? borders? rounded? shadows? sizes
Theme  = colours     desktop · titlebar · text · accent palette
```

Style and Theme never touch the scene graph, surfaces, input or buffers — only
*how* existing windows render.  Presets: Classic / Material / Glass / Minimal /
Retro (style) and Light / Dark (theme).  `SYS_SET_STYLE` (and `nxres style|theme
<name>`) switch them live.  Default = Classic + Light (the previous look).

> **Desktop vs mobile** (future): the two policies map to the two test targets —
> amd64 = desktop, aarch64 = mobile — and build on `compositor.c.new` once the
> compositor is stabilised.  Tiling/mobile policies are hooks today.

## ASTRA mapping

- **One provider, two transports.**  A single `virtio_gpu.c` drives virtio-gpu on
  VirtIO-MMIO (aarch64) and VirtIO-PCI (amd64) because it speaks only the HAL
  transport contract (`virtio_read_reg/write_reg/notify`, `virtio_setup_queue`,
  `arch_virtio_get_device`).  No ISA code, no per-arch GPU driver.
- **Contract, not magic numbers.**  The GPU exposes capabilities behind
  `struct gpu_ops` (`get_display_info`, `set_mode`, `poll_events`); the core
  consumes them via `gpu_*` wrappers.  The compositor sizes itself from
  `gpu_get_primary()` — zero hard-coded resolutions in the core.
- **Compositor in the HAL (DIR-02).**  Frame production is timer-driven
  (`compositor_tick`), the scheduler→compositor dependency stays inverted, and
  the terminal is a self-contained engine (`term.c`) — graphics can later leave
  the kernel as a service.

## Per-transport note

`poll_events` reads the virtio-gpu config events register through the transport
(`virtio_read_reg` at `VIRTIO_MMIO_CONFIG`); this resolves on VirtIO-MMIO
(aarch64 host-driven auto-resize works).  On VirtIO-PCI the modern transport does
not map device-config through that path, so amd64 uses the explicit `set_mode`
path (`nxres`) for now.  Wiring PCI device-config reads is a follow-up.

## Acceptance / verification

- Boot at the host's `xres/yres` without recompiling — verified aarch64
  1920×1080 and amd64/PCI 1600×900 (`Graphics: Initialized via HAL (WxH)`).
- Runtime resize from userland — verified: `nxres 800 600` →
  `VirtIO-GPU: mode set to 800x600 (resource 2)` + `Compositor: resized to
  800x600`; QMP screendump is 800×600 with desktop + windows re-rendered.
- Live theme switch — verified: `nxres theme dark` repaints desktop + chrome.
- Terminal still renders after extraction (shell prompt, ANSI colours, caret).
- Built and booted on **both** arches (aarch64 `aarch64-none-elf-gcc` 7.2.0,
  amd64 `x86_64-elf-gcc`).

## Follow-ups (not in this pass)

- Interactive edge-grip window resize (drag) using the surface-scaling path.
- Compositor FB ≠ scanout (real HiDPI/zoom) final scale stage in flush.
- Border/shadow/rounded rasterisation for Material/Glass/Retro styles.
- Font alpha/scaling (#121.4) and stb_image PNG (#121.5).
- Modern terminal protocol on top of `term.c` (#123 problem 2).
- PCI device-config event reads for amd64 host-driven auto-resize.
