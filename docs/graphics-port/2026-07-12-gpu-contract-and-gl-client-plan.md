# 2026-07-12 — surface-speaking GPU contract; GL client and linking plan

New increment record (documentation is append-only). Maintainer directives
executed in order: fork push resolved, video-driver optimisation in the
gfx_chrome style, Mesa sourcing decision, GL client selection, dynamic
linking evaluation registered before the D3D9 personality.

## Video drivers: one driver, now one vocabulary

Survey result first: the VirtIO-GPU driver is ALREADY single-source across
transports — one `virtio_gpu.c` drives VirtIO-MMIO (AArch64) and VirtIO-PCI
(amd64) through the transport-agnostic virtio register layer, and S-STAB
already made `present` atomic under the driver lock. What was missing is the
same thing the compositor was missing before gfx_chrome: a shared semantic
vocabulary.

- New contract wrapper `gpu_present_surface(const gl_surface *src,
  const gfx_rect *damage)` in gpu_core: the core presents a VALIDATED
  `gfx_surface` + damage rect — the exact unit SDL/OpenGL/D3D9 speak all the
  way down through the syscalls — instead of raw pointer+ints.
- The S-STAB `gfx_surface_verify()` gate now runs AT the core/driver seam:
  a geometry/allocation desync panics loudly at the source rather than
  becoming a silent OOB copy inside a driver (the UTM-panic class, now
  structurally guarded on every present).
- The compositor no longer reaches into `dev->ops->present` directly —
  restoring gpu_core's own "no driver internals in the core" rule. Driver
  ops signatures are unchanged (existing API optimised, not replaced).

## Mesa sourcing decision (maintainer question answered)

Work with the INTEGRATED submodule (`user/sys/lib/opengl`, fork
`olmox001/mesa`, branch `nexsos-port-opengl`, now pinned at `mesa-25.0.7`).
No new gitmodule: the fork + port-branch model already covers OSMesa AND
Gallium Nine (the D3D9-over-gallium candidate) in the same tree, one pin to
maintain, one upstream to sync.

## GL client: Craft (the Minecraft clone)

Selected client for the OpenGL milestone: **Craft** by Michael Fogleman
(github.com/fogleman/Craft) — the canonical complete Minecraft clone:
**MIT licence** (compatible, as required), plain C, OpenGL with simple
shaders, small enough to port honestly. Port plan, in order after the OSMesa
cross-build lands:

1. fork to `olmox001/Craft`, branch `nexsos-port` (the kilo/doom model);
2. dependency strategy: GLFW → thin shim over the SDL2 nexsos driver /
   os1_gl_platform seam (window+input+swap only); GLEW → dropped (static GL
   links symbols directly); sqlite3 (world storage) → vendored, its VFS over
   the OS1 libc; curl (multiplayer) → disabled for the first profile;
3. rendering lands on OSMesa + softpipe through `os1_gl_platform`.

## Dynamic linking (registered for evaluation BEFORE the D3D9 personality)

Motivation confirmed: licence pressure. Wine's d3d9 stack is LGPL —
statically linking it into every ELF is exactly what LGPL makes painful.
Two mitigations, to be decided in a dedicated ASTRA design increment:

- **Gallium Nine** (in the pinned Mesa tree, MIT) as the D3D9 personality
  source removes most of the licence pressure outright;
- a NexsOS dynamic-linking design (shared objects + loader as an ASTRA
  object/VM exercise: mappable library objects with capability-checked
  handles) remains valuable regardless — for LGPL compliance headroom, disk
  footprint, and library updates — and gets its own design document before
  any D3D9 personality code lands.

## Validation

`make` / `make ARCH=amd64` and `make run` on both architectures per
protocol; the contract change is behaviour-preserving (same present path,
plus the verify gate).
