# 2026-07-11 — SDL2 driver overlay and published submodule forks

New increment record; the pre-existing `README.md`, `STATE.md` and
`VALIDATION.md` are unchanged.

## Submodule sources moved to published forks

The three graphics submodules now follow the same scheme as
`base-nexs`/`kilo`/`doom`: a fork on the `olmox001` account with a published
port branch, referenced from `.gitmodules`.

| Path | Fork | Branch | Pinned commit |
| --- | --- | --- | --- |
| `user/sys/lib/sdl` | `olmox001/SDL` (fork of `libsdl-org/SDL`) | `nexsos-port-sdl2` | `0e802c12c` |
| `user/sys/lib/opengl` | `olmox001/mesa` (fork of `chaotic-cx/mesa-mirror`) | `nexsos-port-opengl` | `b16af1f337c` |
| `user/sys/lib/direct3d` | `olmox001/wine` (fork of `wine-mirror/wine`) | `nexsos-port-d3d9` | `6eb2e4c32cc` |

Mesa has no official GitHub mirror any more, so the fork base is the actively
synced `chaotic-cx/mesa-mirror`; Wine uses the official `wine-mirror/wine`.
All three port branches are published and point exactly at the pinned gitlink
commits, so a recursive clone works from the forks alone. Each local submodule
keeps an `upstream` remote at the original source for future syncs.

## SDL2 `nexsos` video driver overlay

`user/sys/lib/portability/sdl2/` supplies the SDL2 video driver as a
NexsOS-owned overlay (video, framebuffer and event sources plus `overlay.mk`);
the SDL submodule remains unpatched. The driver depends only on
`os1_video_platform.h`: software ARGB8888 windows, pitch-safe partial
presentation through `os1_video_present_argb8888_strided`, focused
keyboard/mouse/resize event pumping and resize requests. OpenGL contexts,
relative pointer mode, clipboard, title mutation and hardware acceleration are
deliberately not advertised.

Fix applied in this increment: `SDL_nexsosvideo.c` included
`../../sdl/src/SDL_hints.h`, a path that does not exist (the public header
lives in `include/`); it now includes `"SDL_hints.h"` like every SDL source.
After the fix, all three overlay sources compile cleanly against the SDL 2.32
submodule headers with `-DSDL_VIDEO_DRIVER_NEXSOS`. Registering
`NEXSOS_bootstrap` in SDL's bootstrap table and the SDL cross-build itself
belong to the `nexsos-port-sdl2` branch integration, not to this overlay.

## Validation

Commands: `make run`; `make run ARCH=amd64` (protocol from `VALIDATION.md`).

Result: both QEMU boots completed normally and reached the interactive
desktop.

- AArch64 VirtIO-MMIO: kernel unit tests 6 passed, 0 failed; VirtIO GPU,
  input and block discovered; host scanout 1280x800; four CPUs online; init,
  launcher and NXShell started.
- AMD64 VirtIO-PCI: kernel unit tests 6 passed, 0 failed; same discovery and
  service startup; NXShell exercised interactively in the QEMU window
  (`ls`, `cd sys`) with correct output on the visible desktop.
- No panic or fault in either serial log.
- The overlay sources are not compiled into the NexsOS image yet (they build
  only inside the future SDL cross-build), so the booted kernels exercise the
  foundation/surface increments plus the submodule re-pointing.
