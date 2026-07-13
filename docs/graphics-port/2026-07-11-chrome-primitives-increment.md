# 2026-07-11 — increment 2: compositor chrome extracted into gfx_chrome

New increment record (documentation is append-only: existing files unchanged).
This is increment 2 of the plan in `STATE.md`: "Extract compositor chrome
geometry/shadow calculation into allocation-free surface primitives, keeping
style/theme data-only."

## What moved where

New kernel-internal library `kernel/graphics/chrome.c` +
`kernel/include/kernel/gfx_chrome.h`, beside `gfx_surface`. Every function is
allocation-free and takes plain data (rects, radii, ARGB colours, style
scalars); `compositor_style_t`/`compositor_theme_t` stay data-only in the
compositor, which maps them to arguments. The raster painters draw on a
`gfx_surface_t` honouring an exclusive clip rect — the same surface/clip
semantics the userland SDL2/OpenGL presentation model uses, per the programme
goal of one drawing methodology shared across kernel chrome and userland
personalities.

Extracted from `compositor_render_internal` (code moved verbatim, only
parameterised):

- `gfx_rrect_contains` — the rounded-rect membership test (was the static
  `rrect_inside`).
- `gfx_chrome_shadow_margins` — per-shadow-type damage-footprint margins.
- `gfx_chrome_shadow_solid` / `gfx_chrome_shadow_fast` /
  `gfx_chrome_shadow_premium` — the three shadow painters (solid backing,
  single-pass SDF, contact+diffuse SDF with rim light).
- `gfx_chrome_titlebar_tint`, `gfx_chrome_content_separator` — per-pixel
  titlebar highlight/gradient and under-titlebar inner shadow.
- `gfx_chrome_button_geometry`, `gfx_chrome_button_hit`,
  `gfx_chrome_button_pixel` — titlebar button geometry, hit-test and painting
  (circle / square / rounded-square shapes, left/right side).
- `gfx_chrome_border` — the 1px rounded window outline.

`compositor.c` shrank by ~300 lines and now calls the primitives.

## Deliberate behaviour-preserving optimisations

1. Button geometry is computed once per window; previously it was recomputed
   for every titlebar pixel.
2. `compositor_handle_click` now uses the same `gfx_chrome_button_geometry` +
   `gfx_chrome_button_hit` the renderer paints with. The two sites had
   hand-mirrored geometry ("geometry mirrors the hit-test" comment); they can
   no longer drift.

Self-correction applied during the increment: the first version of the
hit-test used a local `int button` that shadowed the `button` parameter of
`compositor_handle_click`; renamed to `hit_button` before validation.

## Test

`test_gfx_chrome_contract` (kernel unit test #7) covers the rounded-rect
predicate, per-type shadow margins, button geometry + hit-test agreement and
clip honouring of the solid shadow painter on caller-owned memory.

## Validation

Commands: `make run`; `make run ARCH=amd64` (protocol from `VALIDATION.md`).

- AArch64 VirtIO-MMIO: 7 passed, 0 failed; scanout 1280x800; four CPUs
  online; init, launcher and NXShell started; no panic in the serial log.
- AMD64 VirtIO-PCI: 7 passed, 0 failed; same startup; NXShell used
  interactively during the run (`nxwins`).
- Build diagnostics unchanged: only the pre-existing baseline warnings
  (`ksyms.S`, amd64 `.note.PVH`, RWX LOAD segment); no new compiler warnings.

Visual outcome: chrome must look identical to before (shadow types 0/1/2
across the style presets, button shapes/sides, borders, rounded corners).
User visual feedback requested before expanding the porting surface to the
next increment (D3D9 presentation backend, GPU-context object design).
