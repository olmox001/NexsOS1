# 2026-07-11 — increment 4: D3D9 presentation backend + demo3d as client

New increment record (documentation is append-only). This is increment 4 of
the plan in `STATE.md`: "Add a D3D9 adapter backend that maps its CPU
presentation surface to the same header; keep device/context dispatch
independent of the backend." Increment 5 (GPU-context object design) is
delivered in the same batch as a pure design document,
`2026-07-11-gpu-context-object-design.md` — no ABI number, no implementation,
as the plan requires.

## What was added

`user/sys/lib/portability/d3d9/os1_d3d9_present.{h,c}`: a swapchain with a
CPU ARGB8888 backbuffer and D3D9 Lock/Unlock/Present/Reset semantics over
`os1_video_platform.h`.

- Backend-independent dispatch: every public entry point forwards through a
  `struct os1_d3d9_present_ops` table selected at creation from the
  platform's advertised backend. The software table is the only one today;
  `OS1_VIDEO_BACKEND_GPU_CONTEXT` intentionally yields `-ENOSYS` until the
  ASTRA GPU-context object exists.
- D3D9 rules enforced at the dispatch layer, not per backend: Present and
  Reset fail while the backbuffer is locked; double lock is `-EBUSY`;
  out-of-range dirty rects are rejected.
- Presentation maps to `os1_video_present_argb8888_strided` (pitch-safe
  partial presents) + `os1_video_render`.

This is presentation glue only. The D3D9 API layer itself (device, state,
shaders) is future work sourced from the Wine submodule's `nexsos-port-d3d9`
branch; the Wine tree remains unpatched.

## demo3d rewired as the reference client

`user/bin/demo3d.c` now renders its cube through the exact
lock → rasterize → unlock → Present cycle the D3D9 personality will drive,
instead of calling `window_blit` + `compositor_render` directly. The
rasterizer is unchanged; it draws into the locked backbuffer (tight pitch
verified at lock time). The demo prints the active present backend at startup.

## Build integration

`os1_d3d9_present.o` joined `USER_LIB_O` in the Makefile, so the adapter
compiles into the userland library for every system ELF on both
architectures.

## Validation

Commands: `make run`; `make run ARCH=amd64` (protocol from `VALIDATION.md`),
launching `demo3d` from NXShell during each run.

- AArch64 VirtIO-MMIO: kernel unit tests 7 passed, 0 failed; `demo3d`
  started through the swapchain — serial shows
  `Real Solid GL Engine Init … (present backend 1)` (1 =
  `OS1_VIDEO_BACKEND_SOFTWARE`); two concurrent instances ran without
  errors; no lock/present failure, no panic.
- AMD64 VirtIO-PCI: 7 passed, 0 failed; `demo3d` running through the
  swapchain with the same backend line; no failures in the serial log.

Visual outcome to confirm: the rotating shaded cube must look and animate
exactly as before the rewiring (same window, same colours, ~60 FPS pacing).
