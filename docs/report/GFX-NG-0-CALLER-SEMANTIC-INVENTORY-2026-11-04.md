# GFX-NG-0 — caller and semantic inventory of the legacy graphics ABI

**Date:** 2026-11-04  
**Status:** partial inventory, source-verified; no ABI numbers or kernel behavior changed.  
**Purpose:** finish the caller-by-caller portion of GFX-NG-0 before defining buffer/surface descriptors or starting GFX-NG-1.

This document supplements `docs/architecture/GRAPHICS-USERLAND-MIGRATION.md`. It records the actual compatibility surface and callers found in the current tree. It is not permission to move the legacy compositor or change the v1 ABI.

## 1. Current compatibility path

The application-facing names in `include/api/os1.h` and the compatibility names in `user/sys/lib/lib.c` are not direct implementations of a future graphics service:

- `OS1_window_create()` -> `os1_video_window_create()` -> `_sys_create_window()` -> `SYS_CREATE_WINDOW` -> `compositor_create_window()`.
- `OS1_window_draw()` -> `_sys_window_draw()` -> `SYS_WINDOW_DRAW` -> `compositor_draw_rect()`.
- `OS1_window_blit()` -> `os1_video_present_argb8888()` -> `_sys_window_blit()` -> `SYS_WINDOW_BLIT` -> `compositor_blit()`. The portability adapter validates dimensions/stride and packs strided input, but the legacy syscall itself still passes a user pixel pointer to the compositor path.
- `OS1_window_resize()` -> `os1_video_window_resize()` -> `_sys_window_resize()` -> `SYS_WINDOW_RESIZE` -> `compositor_resize_window()`.
- `OS1_window_destroy()` -> `_sys_destroy_window()` -> `SYS_DESTROY_WINDOW` -> `compositor_destroy_window()`.
- `OS1_window_write()` -> `_sys_window_write()` -> `SYS_WINDOW_WRITE` -> `window_text_write()` -> `compositor_window_write()` -> terminal/font rasterization.
- `OS1_gfx_flush()/OS1_gfx_render()` -> `os1_video_render()` -> `_sys_compositor_render()` -> `SYS_COMPOSITOR_RENDER`. Only PID 1 performs USB polling, input drain and rendering; other CAP_WINDOW callers return as damage/presentation hints.
- Bare names (`create_window`, `window_draw`, `window_blit`, `destroy_window`, `flush`, etc.) are compatibility shims, not separate ABI operations.

The same graphics wrappers are duplicated in `user/sys/lib/lib.c` and `user/sys/lib/lib-2.c`; any later API migration must establish which library variant is built for each target and keep both coherent.

## 2. Caller inventory observed in source

| Operation | Observed callers / consumers | Semantic dependency |
|---|---|---|
| Create window | `nxbar`, `nxexec`, `nxfilem`, `nximage`, `nxlauncher`, `nxntfy_srv`, `nxpower`, `nxreg`, `nxsettings`, `nxshell`, `nxui`; capability, IPC, FD, sandbox, stress, input and write tests; demo3d portability adapter; Lua binding | Returns legacy integer window ID. Creation requires CAP_WINDOW, rejects a dying process, copies a bounded title, and binds ownership to current PID. |
| Destroy window | `nxfilem`, `nximage`, `nxbar`, `nxntfy_srv`, `nxpower`, `nxui`; input/stress tests | Owner or machine may destroy. Process teardown and close-button paths also destroy internally. Must become capability revocation plus compositor policy request without losing legacy ID mapping. |
| Draw rectangle | `counter`, IPC demos, `nxshell`, `font_lib` | Kernel raster operation on a window; caller ownership/capability behavior must be preserved by compatibility broker. |
| Blit pixels | `nxbar`, `nxfilem`, `nximage`, `nxlauncher`, `nxntfy_srv`, `nxpower`, `nxsettings`, `nxui`, `demo3d` | Userland owns an ARGB framebuffer and submits a region. This is the main migration seam to mapped buffer capability + bounded damage commit. |
| Resize window | `nxfilem`, `nximage`, `nxlauncher`, `nxshell` | Logical surface resize; current caller often reallocates its own framebuffer separately. Requires generation change and safe in-flight writer/presenter lifetime. |
| Window flags | `nxbar`, `nxexec`, `nxntfy_srv`, `nxui` | Top-most, hidden, passive/click-through flags are window policy, not surface metadata. Owner/machine authorization. |
| Focus | `input_test`, `nxexec`, `nxfilem`, `nxshell` | Legacy API is PID-oriented (`set_focus(get_pid())`); NG compositor must be window-centric and resolve a process primary window in userland. |
| Flush/render | `nxinit` is the real frame pump; `nxenvinit`, `nxshell`, tests and applications call compatibility flush/hints | Frame scheduling belongs to compositord. Preserve current single-owner behavior during transition; do not let every app trigger concurrent render/input polling. |
| Display query | `nxbar`, `nxfilem`, `nximage`, `nxinfo`, `nxlauncher`, `nxntfy_srv`, `nxpower`, `nxres`, `nxsettings`, `nxshell`, `nxui`, portability adapter | Current desktop size is packed into a scalar. NG requires a versioned display/mode query from displayd. |
| Display mode / zoom / style / background | `nxres` CLI and shared `nxres.h` helpers used by settings/desktop services | Global display/compositor policy; currently kernel calls GPU mode and compositor setters and mirrors style/theme/background into registry. These operations migrate to displayd/compositord + registry service, not surface controls. |
| Display poll | PID 1 / `nxinit` supervisor loop | Host mode events are polled in process context; must become displayd's device-event responsibility. |
| Font install | `nxfont` via `set_font()` | Global kernel font pointer/state; move font ownership/rasterization to userland font service/library. Current raw-pointer lifetime contract is unsafe as a long-term service ABI. |
| Window enumeration | `nxui` dock and window inspection utilities | Read-only snapshot of legacy window list. NG equivalent is a compositord-owned query, not kernel scene graph enumeration. |
| Window-of-PID / terminal grid | `nxexec` and terminal applications | PID-to-window lookup and font-derived grid are legacy convenience queries; replace with authenticated process/window service query and terminal metrics. |
| Terminal text write | `nxshell`/stdout routing and legacy `OS1_window_write` veneer | Calls terminal parser and font rasterizer synchronously in kernel under compositor lifetime lock; blocked by GFX-LEGACY-TERMINAL-01 preflight. |

## 3. Security, ownership and synchronization semantics to preserve

The syscall dispatcher currently enforces different policies per operation; they must not be flattened into a single generic “graphics permission”:

- Create: CAP_WINDOW, not dying, title copied into per-CPU syscall buffer, owner=current PID.
- Resize: CAP_WINDOW plus owner or machine.
- Destroy: owner or machine; internal process-exit/close teardown is a separate kernel path.
- Set flags: owner or machine.
- Style/theme/background, zoom, mode and font: CAP_WINDOW; mode changes also invoke GPU mode-setting and compositor resize; display poll is machine-only.
- Window write: CAP_WINDOW; bounded copy into a kernel bounce buffer before terminal output.
- Window blit: legacy compositor validates ownership; successful blit marks `current_process->self_rendered`.
- Compositor render: CAP_WINDOW; only PID 1 performs the actual input/USB/render pump.

The future broker must authenticate the service caller and transfer surface/buffer capabilities explicitly. Do not preserve raw PID authority or expose kernel pointers. A window object in the legacy ABI is not the NG surface object.

The current handle substrate is reference-counted but **does not carry a generation field**: `struct handle_entry` is `{ obj, rights }`, and `handle_install_locked()` reuses the first free integer slot. `struct kobject` currently stores only the legacy window ID and owner PID for `OBJ_TYPE_WINDOW`; there are no buffer/surface payloads or generation counters. Therefore the NG surface generation must be explicit in the typed surface object/commit descriptor and validated independently of the process-local handle index. Do not claim that the existing handle slot itself prevents stale-generation commits.

## 4. Important coupling and migration blockers

1. The existing `os1_video_platform.c` is already the NexsOS-specific seam for SDL/Mesa/D3D portability. Evolve it; do not patch third-party trees or create a parallel video abstraction.
2. The adapter's `os1_video_present_argb8888_strided()` checks stride and count and packs non-contiguous input, but the kernel still consumes a user pointer synchronously. A safe shared-buffer design must define map rights, pinning, generation, commit damage and process-death revocation.
3. `SYS_COMPOSITOR_RENDER` currently combines USB HID polling, input drain and compositor rendering for PID 1. These are separate service responsibilities in the target topology.
4. `SYS_SET_DISPLAY_MODE`, `SYS_DISPLAY_POLL`, style/theme/background and zoom are desktop/display policy. They must not be placed in the surface/buffer object API.
5. Terminal text rendering remains a correctness gate. The intermittent AArch64 SSP incidents in `GFX-LEGACY-TERMINAL-CONCURRENCY-2026-11-03.md` and `GFX-INPUT-SSP-FAULT-2026-11-04.md` are still OPEN; passing test-all runs do not close them.

## 5. G0 status and next required audit

This caller map is a source-derived first pass, not a claim that every call site and semantic edge is closed. Still to verify before G0 exit:

- all syscall wrapper declarations/argument widths/return conventions in both `lib.c` and `lib-2.c`, plus the two architecture syscall stubs;
- `SYS_WINDOW_ENUM` structure/version and exact snapshot consistency;
- stdout/TTY routing into `window_text_write()`, including terminal close/focus and resize interactions;
- all compositor-internal callers from IRQ, scheduler, process teardown, input and GPU paths;
- complete style/theme/zoom/font persistence and notification behavior;
- the proposed buffer/surface/commit descriptors against the existing object manager's generation, reference, rights, transfer and revocation rules.

**No new object type, syscall number, ABI struct, source move or legacy behavior change is authorized by this inventory.** GFX-NG-1 must wait until the terminal concurrency preflight and the remaining G0 audit items above are resolved.

## 6. Legacy surface storage and lifetime facts

The current `struct window` in `kernel/graphics/compositor.c` combines several distinct domains:

- scene/window policy: legacy ID, owner PID, x/y, logical width/height, draw width/height, z-order, visibility/minimized/protected/top-most/passive flags, title and corner state;
- pixel storage: a raw `uint32_t *buffer` allocated with `kmalloc(width * height * 4)`, no first-class object, mapping record or capability;
- terminal state: embedded `struct terminal` (cell grids, cursor, parser and focus state);
- presentation metadata: alpha/background and damage tracking.

Observed lifecycle:

- Create allocates and initializes the pixel buffer and terminal grids while `compositor_lock` is held, then publishes the window record.
- Destroy frees the pixel buffer and terminal grids and clears the record while holding `compositor_lock`.
- Resize allocates a replacement while holding `compositor_lock`, swaps the live pointer and geometry under that lock, unlocks, then frees the old buffer. It subsequently notifies the owner with `IPC_TYPE_RESIZE`.
- `compositor_get_buffer()` explicitly documents that its returned pointer is unsafe after unlock unless the caller independently owns the window lifetime.
- `compositor_blit()` performs owner validation and copies the user pixel data into the kernel buffer while holding `compositor_lock`; the syscall is synchronous and has no shared-buffer lease.
- `gfx_surface_t` is currently only a typedef of `struct gl_surface`; `surface.c` still exposes clear/fill/composite pixel operations. It is not the planned capability-backed NG surface manager.

This confirms the required separation is not simply moving `compositor.c`: scene policy and terminal state must leave the kernel, while a new kernel-side buffer/surface object must own validated backing pages, map/pin counts, rights, generation and in-flight commit lifetime. The current lock is simultaneously a scene-table lock and a lifetime guard; the NG design must not reproduce that coupling.
