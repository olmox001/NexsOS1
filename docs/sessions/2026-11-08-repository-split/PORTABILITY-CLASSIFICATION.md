# Portability layer ownership and support inventory

Date: 2026-11-08
Status: first code/documentation audit; feature-level conformance tests still required.

## NexsOS-owned portability code

| Path | Purpose and evidence | Target owner | Current support classification |
|---|---|---|---|
| portability/os1_video_platform.{c,h} | Common software-video adapter; queries display info, creates/resizes windows, blits ARGB8888 and polls input through existing OS1 syscalls | NX-SDK graphics extension | SOFTWARE_ONLY for the declared software framebuffer path; runtime feature matrix still NOT_RUN |
| portability/d3d9/os1_d3d9_present.{c,h} | D3D9-style swapchain/presentation seam over os1_video_platform; not a D3D9 implementation | NX-SDK graphics extension | SOFTWARE_ONLY for CPU backbuffer presentation; full D3D9 REQUIRES Wine compatibility implementation |
| portability/opengl/os1_gl_platform.{c,h} | Software color-buffer/window presentation seam; code rejects non-software backend and does not create a real GL context | NX-SDK graphics extension | SOFTWARE_ONLY for framebuffer surface; PARTIAL for OpenGL context/API compatibility |
| portability/sdl2/* | Four NexsOS SDL2 driver sources plus config/overlay; README says upstream SDL remains unpatched | NX-SDK SDL portability adapter; SDL2 source remains external | PARTIAL overall; software ARGB8888 window/framebuffer/event path is implemented; remaining SDL features must be individually classified and tested |
| portability/gnulib/* | NexsOS glue/config and locale/time compatibility around upstream Gnulib | NX-SDK optional compatibility extension; upstream Gnulib remains external | CONDITIONAL on C/POSIX locale where stated; timezone helpers are currently no-ops; do not claim full locale support |
| portability/lua/* and user/sys/lib/os1LUA_lib.c | Lua time/tmpnam/REPL integration and OS1 Lua bindings | NX-SDK language-runtime extension; upstream Lua remains external | PARTIAL pending tests for tmpnam, clock units, redirected stdin and all exposed OS1 operations |
| portability/coreutils/* | Coreutils-specific config, glue, headers and patches | NX-VIOLA application port/build integration | Not SDK core. Extract a function into SDK only if it is generic, independently specified and reused outside Coreutils |

## Mixed build ownership to separate

- portability/gnulib/overlay.mk compiles both upstream Gnulib sources and Coreutils-specific gl sources. Split the manifest: upstream Gnulib remains an external dependency; generic NexsOS Gnulib adapter belongs to SDK; Coreutils gl sources and app link rules remain with NX-VIOLA.
- portability/coreutils/overlay.mk is app-specific and currently passes -Wno-error. Do not move the entire overlay into SDK and do not carry warning demotions into a stable SDK build.
- SDL2, Gnulib, Lua, Wine/D3D9 and Mesa/OpenGL upstream source trees remain external. Only NexsOS-owned adapters/configuration/patches move to NX-SDK. Do not patch upstream submodules as part of extraction.

## Explicit gaps and inconsistencies to resolve

1. SDL2 README says title mutation is not advertised, while SDL_nexsosvideo.c registers NEXSOS_SetWindowTitle. Verify actual behavior and update the spec/test or remove the false claim.
2. SDL2 README explicitly excludes OpenGL contexts, relative pointer mode, clipboard and hardware acceleration. These must be represented as PARTIAL/UNSUPPORTED/REQUIRES capabilities, not silently treated as full SDL2 support.
3. os1_video_platform.c defines OS1_VIDEO_ABI_VERSION privately. Move the identity into the public contract header/manifest and test client/provider mismatch behavior.
4. D3D9 presentation uses a function-pointer operations table. Audit it against the project's NASA/JPL Rule 9 constraints; keep only with a documented, bounded justification or replace with explicit dispatch.
5. Gnulib locale_compat.c returns C locale and implements set_tz/revert_tz as no-ops. This is conditional behavior, not full locale/timezone support.
6. Lua tmpnam uses a static counter and /home path; verify collision, wrap, creation semantics and multi-process behavior. clock() returns os1_cpu_ns(); verify CLOCKS_PER_SEC and the C clock() contract.
7. Coreutils glue includes app-facing helpers such as iopoll and uname; keep app-specific unless a generic SDK contract and independent consumer justify extraction.

No portability feature is FULL merely because a source file exists or an adapter links. Each item needs a requirement ID, exact profile, test IDs and a result in the generated compatibility matrix.
