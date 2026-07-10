# Graphics port state and decisions

## 2026-07-11 — foundation

- Selected D3D9 as the first Direct3D compatibility profile.
- Parent worktree remains on `comprehensive-review-sdl`; no NexsOS parent
  branch is created for this programme.
- Registered immutable upstream source submodules:
  - `user/sys/lib/sdl` — SDL, upstream `SDL2` branch;
  - `user/sys/lib/opengl` — Mesa, upstream `main` branch;
  - `user/sys/lib/direct3d` — Wine, upstream `master` branch, used as the
    D3D9 compatibility source.
- Created local port branches without changing upstream source:
  `nexsos-port-sdl2`, `nexsos-port-opengl`, `nexsos-port-d3d9`.
- Added `os1_video_platform`: a single userland adapter for window creation,
  resize, ARGB8888 presentation, translated input events and render
  notification. `lib.c` now routes its window create/destroy/resize/blit/render
  wrappers through it.
- Added `gfx_surface`, an internal kernel semantic facade around validated
  ARGB8888 surfaces. It does not add an application-visible syscall.
- Completed the two declared-but-missing internal raster primitives:
  `gl_draw_rect` and `gl_swizzle_bgr`.

## Non-negotiable constraints

- Do not patch Mesa, SDL or Wine for NexsOS integration. All OS glue belongs
  under `user/sys/lib/portability` and build glue belongs in NexsOS.
- No ambient-authority GPU syscall. Future GPU context, surface and fence
  operations are object-manager handles with capability checks per ASTRA 6.1
  and 6.2.
- A feature is not marked ported until it builds and is manually exercised on
  both AArch64 VirtIO-MMIO and amd64 VirtIO-PCI.

## Next controlled increments

1. Add unit tests for `gfx_surface` clipping, blending and BGR swizzle.
2. Extract compositor chrome geometry/shadow calculation into allocation-free
   surface primitives, keeping style/theme data-only.
3. Implement SDL2's NexsOS video/event driver against the portability header.
4. Add a D3D9 adapter backend that maps its CPU presentation surface to the
   same header; keep device/context dispatch independent of the backend.
5. Design the capability-owned GPU-context object before adding any new ABI
   number or hardware-accelerated path.
