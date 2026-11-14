> 2026-11-12 scope correction: this is a historical function inventory. [K]
> records the former retention decision, not approval to keep window/desktop
> policy in the kernel. The current maintainer requirement and cutover gates
> are in GRAPHICS-USERLAND-MIGRATION.md. Original line numbers are provenance,
> not current source locations; missing functions/files must be reconciled
> against the live build manifest before moving or deleting code.

# Graphics and compositor kernel function migration catalog

**Purpose:** function-by-function source-of-truth for the graphics transition. A box is checked only after its implementation is in userland (or the kernel primitive is intentionally classified as retained) and the relevant behavior has a passing regression test. This is an inventory, not a completion claim.

**Target boundary:** window/process ownership and terminal/window lifecycle remain kernel mechanisms as explicitly requested; rendering, raster, chrome, terminal parsing/raster, input policy, graphics libraries and GPU policy move to supervised userland. Kernel keeps surface/capability lifetime, safe mapping/commit/present handoff, registry and hardware resource/DMA/IRQ enforcement.

**NASA/JPL Power of Ten review for every moved function:** (1) fixed bounds for loops/queues; (2) no recursion; (3) static bounds/invariants for arrays and surface spans; (4) validate all external data at entry; (5) keep helpers short and one responsibility; (6) check every allocation/IPC/surface error; (7) constrain shared state and locks; (8) avoid hidden global state; (9) test assertions/visual behavior on both architectures; (10) keep the migration catalog and exact source mapping current.

**Status:** `[ ]` pending; `[M]` moved/reimplemented in userland and exercised; `[K]` deliberately retained kernel-side under the target boundary; `[~]` partial/compatibility path remains and does not count as done.

## Kernel functions by source

The 182 rows and line numbers below preserve the original inventory. Current extraction finds 159 functions in `kernel/graphics/*.c` and `kernel/drivers/gpu/*.c`: 23 original functions were removed. Removed functions remain cataloged; `[~]` explicitly records missing parity instead of counting deletion as completion. Input drivers and syscall callers require their own additional inventory. `[K]` is an ownership classification, not proof of a passing lifecycle test.

### `kernel/drivers/gpu/gpu_core.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [~] | `gpu_get_display_info` | 68 | Still implemented in the kernel; split provider protocol/implementation into a userland device service while retaining capability/MMIO/IRQ/DMA resource enforcement. Not a completed retained primitive. |
| [~] | `gpu_poll_events` | 75 | Still implemented in the kernel; split provider protocol/implementation into a userland device service while retaining capability/MMIO/IRQ/DMA resource enforcement. Not a completed retained primitive. |
| [~] | `gpu_present_surface` | 82 | Still implemented in the kernel; split provider protocol/implementation into a userland device service while retaining capability/MMIO/IRQ/DMA resource enforcement. Not a completed retained primitive. |
| [~] | `gpu_register` | 11 | Still implemented in the kernel; split provider protocol/implementation into a userland device service while retaining capability/MMIO/IRQ/DMA resource enforcement. Not a completed retained primitive. |
| [~] | `gpu_set_mode` | 61 | Still implemented in the kernel; split provider protocol/implementation into a userland device service while retaining capability/MMIO/IRQ/DMA resource enforcement. Not a completed retained primitive. |
| [ ] | `gpu_get_primary` | 56 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `gpu_unregister` | 31 | Still implemented in the kernel; split provider protocol/implementation into a userland device service while retaining capability/MMIO/IRQ/DMA resource enforcement. Not a completed retained primitive. |

### `kernel/drivers/gpu/virtio_gpu.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [ ] | `vgpu_attach_backing` | 87 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_create_2d` | 70 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_flush` | 220 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_get_display_info` | 447 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_poll_events` | 464 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_present` | 269 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_query_display_info` | 135 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_release_resources` | 509 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_reset_device` | 497 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_set_mode` | 356 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_set_scanout` | 104 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_unref` | 123 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_xfer_flush_locked` | 171 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `virtio_gpu_send` | 555 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_get_framebuffer` | 339 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `vgpu_destroy` | 535 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `virtio_gpu_init` | 661 | Still implemented in the kernel; split provider protocol/implementation into a userland device service while retaining capability/MMIO/IRQ/DMA resource enforcement. Not a completed retained primitive. |

### `kernel/graphics/chrome.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [~] | `gfx_chrome_button_hit` | 314 | Shared userland hit-test exists and is used by `nxmoused`; remove the dead kernel renderer/input copy after its remaining callers are retired. |
| [~] | `gfx_rrect_contains` | 60 | Userland equivalent is exercised by `make test-gfx-chrome`; kernel implementation still backs legacy painters. |
| [ ] | `chrome_clipped` | 108 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `chrome_isqrt` | 38 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `chrome_sdf_dist` | 138 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `gfx_chrome_button_pixel` | 329 | Shared userland painter is used by `nxcompositor` and host-tested; remove kernel implementation after renderer retirement. |
| [ ] | `gfx_chrome_content_separator` | 272 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `gfx_chrome_titlebar_tint` | 255 | Userland tint helper is used by `nxcompositor`; kernel copy remains in the legacy renderer. |
| [ ] | `gfx_chrome_border` | 397 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `gfx_chrome_button_geometry` | 288 | Shared userland geometry is used by both painting and input; remove kernel copy when legacy renderer and click path are retired. |
| [ ] | `gfx_chrome_shadow_fast` | 154 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gfx_chrome_shadow_margins` | 84 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gfx_chrome_shadow_premium` | 183 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gfx_chrome_shadow_solid` | 118 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |

### `kernel/graphics/compositor.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [M] | `compositor_paint_background` | 2535 | Equivalent rendering helper in `user/sys/bin/nxcompositor.c`; background/cursor path exercised in AMD64 and AArch64 screenshots. |
| [K] | `compositor_create_window` | 1063 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_focus_window` | 1291 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_get_window_by_pid` | 1666 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_minimize_window` | 1241 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_resize_window` | 1768 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_restore_window` | 1266 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_set_zoom` | 914 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_window_grid` | 1509 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_window_info` | 1348 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_window_owner` | 1204 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_window_snapshot` | 1401 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `sys_window_enum` | 1368 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [~] | `region_pool_retire` | 561 | Removed with the unused kernel renderer; userland currently redraws full frames. Bounded damage/occlusion regions still need a userland port; removal is not migration completion. |
| [~] | `__click_try_start_resize_locked` | 2085 | Removed; `nxmoused.c::button_event/route_event` implement bottom-right resizing only. Original edge/corner and protection behavior must be ported/tested. |
| [~] | `compositor_present_frame` | 2638 | Kernel helper removed; `nxcompositor.c::render_frame` submits OS1_display_present. Kernel copy/GPU implementation remains; capability-backed presentation is pending. |
| [K] | `compositor_reserved_bottom_locked` | 794 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_reserved_top_locked` | 771 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [ ] | `window_region_has_alpha` | 429 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `region_pool_acquire` | 547 | Removed with the unused kernel renderer; userland currently redraws full frames. Bounded damage/occlusion regions still need a userland port; removal is not migration completion. |
| [~] | `__click_hit_test_locked` | 2036 | Removed; `nxmoused.c::hit_test` consumes kernel snapshots. Full clipping/policy parity remains unverified. |
| [ ] | `__clear_other_carets_locked` | 1866 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `__click_dispatch_outside_lock` | 2220 | Removed; `nxmoused.c::button_event/send_mouse` dispatch in userland. Error/backpressure handling remains incomplete. |
| [~] | `__click_prepare_dispatch_locked` | 2142 | Removed; `nxmoused.c::button_event` prepares userland actions. Full original behavior remains a regression gate. |
| [~] | `__click_raise_and_focus_locked` | 2063 | Removed; `nxmoused.c::button_event` requests OS1_window_focus; kernel retains authorization/window policy. |
| [ ] | `__fill_window_info_locked` | 1318 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `__focus_topmost_locked` | 1182 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `__free_retired_window_resources` | 1539 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `__mouse_capture_motion_locked` | 2406 | Removed; `nxmoused.c::route_event/send_mouse` sends motion. Motion currently follows hit testing rather than complete captured-target semantics; port before closing. |
| [~] | `__mouse_update_drag_locked` | 2327 | Removed; `nxmoused.c::route_event` calls OS1_window_move. Full constraints/zoom/protection parity remains unverified. |
| [~] | `__mouse_update_position_locked` | 2296 | Removed; `nxmoused.c::route_event/clamp_pointer` handle relative/absolute coordinates. Full protocol ranges/hotplug remain unverified. |
| [~] | `__mouse_update_resize_locked` | 2360 | Removed; `nxmoused.c::route_event` calls OS1_window_resize. Original edge/corner, zoom and release behavior remains incomplete. |
| [ ] | `__raise_to_front_locked` | 1229 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `compositor_bind_backbuffer_surface` | 563 | Kernel desktop backbuffer helper removed; `nxcompositor.c::render_frame` owns temporary userland buffers. Persistent mapped surface ownership is pending. |
| [~] | `compositor_handle_click_release` | 1948 | Kernel implementation removed; `nxmoused.c::button_event` sends captured releases. Full capture/resize release parity remains unverified. |
| [M] | `compositor_paint_cursor` | 2594 | Equivalent rendering helper in `user/sys/bin/nxcompositor.c`; background/cursor path exercised in AMD64 and AArch64 screenshots. |
| [~] | `compositor_render_internal` | 2702 | Kernel implementation removed; `nxcompositor.c::render_frame` performs ring-3 composition. Occlusion, damage, clipping, complete shadows/chrome and legacy parity remain incomplete. |
| [ ] | `draw_rect_internal` | 3235 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `expand_damage` | 384 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `expand_window_content_damage` | 396 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [~] | `gfx_notify_client` | 1756 | Removed with the unused kernel pointer path; `nxmoused.c::send_mouse` sends IPC. Dropped-event accounting and recovery are not yet equivalent. |
| [~] | `region_pool_frame_reset` | 536 | Removed with the unused kernel renderer; userland currently redraws full frames. Bounded damage/occlusion regions still need a userland port; removal is not migration completion. |
| [K] | `compositor_get_buffer` | 1649 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [ ] | `compositor_blit` | 3306 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [K] | `compositor_destroy_window` | 1553 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_destroy_windows_by_pid` | 1588 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [ ] | `compositor_draw_rect` | 3295 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [K] | `compositor_focus_changed` | 1887 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_full_damage` | 715 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_get_size` | 728 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [~] | `compositor_handle_click` | 2256 | Kernel implementation removed; `nxmoused.c::button_event` handles clicks, close/minimize and capture. Complete original click/resize/error semantics still require parity tests. |
| [K] | `compositor_init` | 658 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_move_window` | 1707 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [~] | `compositor_render` | 3162 | Kernel entry removed; `nxcompositor.c::render_frame/main` own rendering. Full renderer parity remains open; SYS_COMPOSITOR_RENDER is a compatibility no-op. |
| [K] | `compositor_resize` | 823 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_set_native_mode` | 902 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `compositor_set_window_flags` | 3438 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [~] | `compositor_tick` | 3196 | Kernel entry removed; `nxcompositor.c::main` owns frame pacing. No timer latency improvement has been demonstrated. |
| [~] | `compositor_update_mouse` | 2452 | Kernel implementation removed; `nxmoused.c::route_event` aggregates motion and publishes the pointer. Device matrix, capture and resize parity remain open. |
| [ ] | `compositor_window_write` | 1900 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |

### `kernel/graphics/compositor_style.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [ ] | `compositor_background_active` | 186 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `compositor_style_active` | 178 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `compositor_theme_active` | 182 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `compositor_set_background` | 211 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `compositor_set_style` | 195 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `compositor_set_theme` | 203 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [K] | `compositor_titlebar_height` | 190 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |

### `kernel/graphics/draw2d.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [ ] | `swap_int` | 102 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_blend` | 205 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_circle` | 27 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_gradient_h` | 221 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_line` | 14 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_rounded_rect` | 161 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_triangle` | 92 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_fill_circle` | 59 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_fill_triangle` | 111 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |

### `kernel/graphics/draw3d.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [ ] | `mat4_identity` | 61 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `mat4_mul` | 147 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `mat4_perspective` | 181 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `mat4_rotate_y` | 112 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `mat4_scale` | 85 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `mat4_translate` | 74 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `project_to_screen` | 209 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `mat4_mul_vec` | 165 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `render3d_clear_zbuffer` | 47 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `render3d_cube` | 283 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `render3d_init` | 24 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `render3d_triangle` | 241 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |

### `kernel/graphics/font.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [ ] | `graphics_char_width` | 232 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_font_ascent` | 387 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_font_height` | 364 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_font_max_width` | 398 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_string_width` | 323 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `sys_set_font` | 414 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_draw_char_clipped` | 135 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_draw_char` | 211 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_draw_string` | 258 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_draw_string_clipped` | 284 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |

### `kernel/graphics/gl.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [ ] | `clip` | 49 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `blend_over` | 72 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_blit` | 268 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_clear` | 94 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_draw_line` | 140 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_draw_pixel` | 120 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_draw_rect` | 168 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_draw_rect_fill` | 199 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gl_swizzle_bgr` | 221 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |

### `kernel/graphics/graphics.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [K] | `graphics_present_argb8888` | 79 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [K] | `graphics_screen_surface` | 55 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [ ] | `graphics_clear` | 142 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_char` | 221 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_pixel` | 109 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_rect` | 125 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_draw_string` | 239 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_init` | 35 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `graphics_swap_buffers` | 159 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [K] | `panic_screen` | 175 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |

### `kernel/graphics/region.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [ ] | `region_scratch_reset` | 104 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `region_copy_into` | 111 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `region_create` | 55 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `region_add_rect` | 149 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `region_clear` | 327 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `region_destroy` | 79 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `region_intersect_rect` | 295 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `region_subtract` | 223 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |

### `kernel/graphics/surface.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [K] | `gfx_surface_valid` | 5 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |
| [ ] | `gfx_surface_clear` | 34 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gfx_surface_composite_over` | 47 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `gfx_surface_fill` | 39 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [K] | `gfx_surface_verify` | 14 | Retained kernel boundary: window/process lifecycle, surface validation/present, or hardware resource control. |

### `kernel/graphics/term.c`

| Status | Original kernel function | Original line | Userland destination / retained reason |
|---|---|---:|---|
| [ ] | `term_init` | 316 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_resize` | 365 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `csi_params` | 120 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_xterm256` | 37 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `handle_csi` | 204 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `handle_sgr` | 59 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_clear_cell` | 138 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_draw_cursor` | 174 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_erase_caret` | 154 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_fill_rect` | 21 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_clear_caret` | 502 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_free` | 354 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_set_focus` | 494 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |
| [ ] | `term_write` | 396 | Not yet transferred; port/reuse the original implementation and preserve its existing behavior. |

## Required gates before closing the catalog

- [ ] Every `[ ]` and `[~]` function has a specific userland destination or a documented `[K]` reason; no kernel rendering symbol remains linked except the emergency/present handoff path.
- [ ] Window state, terminal state, focus, process ownership, resize, damage, occlusion, clipping, alpha/scaling, chrome, both titlebar buttons, original cursor, error paths and restart behavior match the original behavior.
- [ ] GPU protocol/command submission and acceleration policy execute in userland; kernel only mediates capabilities, DMA pinning/cache maintenance, MMIO/IRQ resources and validated presentation.
- [ ] No duplicate implementation remains after each source block is transferred; temporary original source backup remains checksum-verifiable until the transition gates close.
- [ ] Build, KTEST, original graphics/window tests, mouse/keyboard/touch interaction, 160-sample timer diagnostic and 30-second health soak pass on AMD64 and AArch64.
- [ ] Measure compositor lock/IRQ-off time and frame latency before/after. If timer/latency does not improve, profile and fix the remaining kernel render path before checking the performance gate.

## Current evidence (2026-11-08)

- See [current handoff and test evidence](../report/GFX-USERLAND-HANDOFF-2026-11-08.md). The transition is **incomplete**.
- `nxcompositor` composes in ring 3; the unused kernel frame renderer and pointer handlers have been removed. Kernel window storage, terminal rendering, graphics libraries and GPU implementation remain.
- The original 12×16 cursor bitmap is retained. Shared userland button geometry/paint/hit helpers cover close + minimize; the yellow button has a central horizontal bar. Chrome ASan/UBSan host tests pass; full window decoration parity is still open.
- Fresh GRUB images booted on AMD64 and AArch64 with the new window-manager log marker; KTEST passed 11/11 and 12/12, respectively, plus window focus, 30-second health soak and screenshots. QMP mouse minimize/dock restore and subsequent keyboard commands succeeded on both.
- Original graphics and input sources remain in `/tmp/nexsos-gfx-input-backup-20261108`: all 15 SHA-256 manifest entries verified. This manifest does not include GPU driver files; use their repository history. Keep the backup and history until the parity gates close.
