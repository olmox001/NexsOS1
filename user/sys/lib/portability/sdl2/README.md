# SDL2 NexsOS overlay

This overlay supplies the SDL2 `nexsos` video driver without editing the SDL
submodule. Its build manifest is `overlay.mk`; the eventual SDL cross-build
must compile those three sources with `SDL_VIDEO_DRIVER_NEXSOS` and link them
with `lib.c`/`os1_video_platform.c`.

Implemented scope: software ARGB8888 windows, pitch-safe partial framebuffer
presentation, focused keyboard/mouse/resize event pumping and resize requests.
OpenGL contexts, relative pointer mode, clipboard, title mutation and hardware
acceleration are deliberately not advertised yet.
