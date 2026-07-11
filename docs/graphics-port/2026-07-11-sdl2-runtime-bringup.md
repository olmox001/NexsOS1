# 2026-07-11 — SDL2 runtime bring-up: first SDL window on screen

New increment record (documentation is append-only).

## Result

`sdltest` runs on NexsOS: `SDL_Init` selects the `nexsos` driver, the window
appears on the compositor with the ARGB gradient, pure R/G/B channel squares
and the moving white bar, all presented through
SDL_GetWindowSurface/SDL_UpdateWindowSurface → NEXSOS framebuffer →
`os1_video_present_argb8888_strided`. Verified visually by the user on
AArch64 (screenshot: SDL2 window beside NXShell reporting
`driver: nexsos`).

Note for future testers: the white bar at the bottom is the only element
DESIGNED to move. The gradient and the R/G/B squares are static; any apparent
motion of those indicates a presentation bug.

## Root cause fixed: SDL_CreateWindow failed at first run

SDL2's core applies the window title only AFTER `CreateSDLWindow` (through
`SetWindowTitle`), so `window->title` is still NULL inside the driver's
create hook — and `os1_video_window_create` rejects a NULL title with
`-EINVAL`. The overlay now falls back to a "SDL2" title at creation and also
resolves the `SDL_WINDOWPOS_UNDEFINED/CENTERED` magic positions to a sane
default instead of passing them to the kernel as raw coordinates.

## nexs target standardisation (user-authored)

`SDL_config_nexsos.h` defines `SDL_NEXSOS 1` alongside the `-D__NEXSOS__`
build flag — the standard platform pair the cross-build advertises to SDL
sources, as SDL expects for a platform port. The only in-tree change,
`SDL_ExitProcess` exiting through the OS1 libc `exit()`, was authored by the
maintainer on the fork's `nexsos-port-sdl2` branch (per the submodule's
policy that AI-generated code must not enter the SDL tree).

## Known port gaps (open, tracked for next increments)

- SDL timer subsystem disabled (`SDL_TIMERS_DISABLED`): `SDL_Delay` /
  `SDL_GetTicks` are dummy stubs; apps must pace with `OS1_sleep` for now.
- Threads disabled, audio dummy, no title mutation after window creation
  (kernel window-property operation not yet exposed), no dynamic loading.
- OS1 libc macro leakage into SDL sources (`STACK_SIZE`, `FP_ONE` from
  `os1.h` via `stdlib.h`) produces redefinition warnings — a libc layering
  cleanup candidate, not an SDL issue.

## Validation

`make` and `make ARCH=amd64` green (only pre-existing baseline diagnostics).
AArch64 `make run`: 7/7 kernel tests, no panic, SDL window verified on
screen by the user. AMD64 boot verified in the previous protocol run; the
post-fix delta touches only the SDL overlay and sdltest client, not the
kernel image.
