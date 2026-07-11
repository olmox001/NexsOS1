# 2026-07-11 — SDL2 window resize: adopt-and-reacquire (nxlauncher pattern)

New increment record (documentation is append-only).

## Symptom

Grip-resizing the sdltest window ended the app:
`present failed: Window surface is invalid, please call
SDL_GetWindowSurface() to get a new surface`. The compositor's resize event
reached SDL (the driver pumps `SDL_WINDOWEVENT_RESIZED`) and SDL correctly
invalidated the window surface — but nothing re-acquired it, and the OS1
logical surface was never re-adopted at the new size.

## Fix: the nxlauncher/nxsettings resize contract, mapped to SDL

Native OS1 apps handle `INPUT_TYPE_RESIZE` by (1) `OS1_window_resize` to
adopt the new logical size (kernel no-ops when unchanged, so grip-release
echoes cannot loop), (2) rebuilding their pixel buffer, (3) redrawing. The
same sequence now happens through SDL's own machinery:

- **Driver** (`SDL_nexsosframebuffer.c`): `NEXSOS_CreateWindowFramebuffer`
  — the hook SDL calls when the app re-acquires the window surface — first
  adopts `window->w/h` as the OS1 logical size via
  `os1_video_window_resize`, then allocates the matching framebuffer. First
  acquisition passes through the same path (kernel no-op guard).
- **Client** (`sdltest.c`): on `SDL_WINDOWEVENT_SIZE_CHANGED` it re-acquires
  the surface, tracks the drawable size dynamically (gradient, border, bar
  and markers all scale), clamps the markers into a shrunken window, and a
  failed present retries one surface re-acquire (a resize can land between
  poll and present) instead of exiting.

## Validation

`make` / `make ARCH=amd64` green; parallel headless QEMU boots on both
architectures: 7/7 kernel tests, no panic. Interactive checklist for the
maintainer: grip-resize the sdltest window repeatedly (grow and shrink,
including during mouse motion) — the gradient must rescale to the new size,
serial logs `resized to WxH`, and the app must keep running; input
(crosshair, clicks, arrows) must still work after several resizes.
