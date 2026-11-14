# Live graphics ownership census — 2026-11-12

Generated from current C translation units through the same Clang source parser used by Power12. This inventory supersedes historical line numbers; it does not certify equivalence or completed migration. Current maintainer scope: normal graphics and all window/desktop policy in userland, kernel restricted to surface/buffer/capability mechanisms and emergency output.

## Existing resource and SDK seams

| Kernel provider | SDK consumer | Current limitation |
| --- | --- | --- |
| resources/buffer.c | extensions/services/buffer_client.c; gfx_buffer.c | Page-backed resource and process mappings exist; complete typed surface/view/commit model remains open |
| resources/display.c | surface_client.c; nxcompositor | Present still copies into allocated staging and a legacy GPU path; not a completed surface commit lease |
| resources/window_compat.c | surface_client.c; kernel WINDOW compatibility | Calls kernel compositor state; must disappear with window policy cutover |
| resources/input.c; drivers/input/core.c | nxkeyboardd; nxmoused | Raw events separated from interpretation; capability event routing and loss/capture semantics still need qualification |

## Current function definitions

### `kernel/graphics/chrome.c`

Existing destination/seam: `system/lib/gfx/chrome.c; system/lib/gfx/decoration.c`. Existing SDK primitives are reused; verify geometry and hit-test parity.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `chrome_isqrt` | 38 | Open: caller/contract/lifetime equivalence review |
| `gfx_rrect_contains` | 60 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_shadow_margins` | 84 | Open: caller/contract/lifetime equivalence review |
| `chrome_clipped` | 108 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_shadow_solid` | 118 | Open: caller/contract/lifetime equivalence review |
| `chrome_sdf_dist` | 138 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_shadow_fast` | 154 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_shadow_premium` | 183 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_titlebar_tint` | 255 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_content_separator` | 272 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_button_geometry` | 288 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_button_hit` | 314 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_button_pixel` | 329 | Open: caller/contract/lifetime equivalence review |
| `gfx_chrome_border` | 397 | Open: caller/contract/lifetime equivalence review |

### `kernel/graphics/compositor.c`

Existing destination/seam: `user/sys/services/nxcompositor.c; user/sys/services/nxmoused.c`. Window/scene/focus/stacking policy migrates to userland. Only surface/capability mechanisms remain in kernel.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `expand_damage` | 388 | Open: caller/contract/lifetime equivalence review |
| `expand_window_content_damage` | 401 | Open: caller/contract/lifetime equivalence review |
| `window_region_has_alpha` | 434 | Open: caller/contract/lifetime equivalence review |
| `compositor_init` | 511 | Open: caller/contract/lifetime equivalence review |
| `compositor_full_damage` | 537 | Open: caller/contract/lifetime equivalence review |
| `compositor_get_size` | 551 | Open: caller/contract/lifetime equivalence review |
| `compositor_reserved_top_locked` | 594 | Open: caller/contract/lifetime equivalence review |
| `compositor_reserved_bottom_locked` | 617 | Open: caller/contract/lifetime equivalence review |
| `compositor_resize` | 646 | Open: caller/contract/lifetime equivalence review |
| `compositor_set_native_mode` | 682 | Open: caller/contract/lifetime equivalence review |
| `compositor_set_zoom` | 694 | Open: caller/contract/lifetime equivalence review |
| `compositor_create_window` | 729 | Open: caller/contract/lifetime equivalence review |
| `__focus_topmost_locked` | 848 | Open: caller/contract/lifetime equivalence review |
| `compositor_window_owner` | 870 | Open: caller/contract/lifetime equivalence review |
| `__raise_to_front_locked` | 895 | Open: caller/contract/lifetime equivalence review |
| `compositor_minimize_window` | 907 | Open: caller/contract/lifetime equivalence review |
| `compositor_restore_window` | 932 | Open: caller/contract/lifetime equivalence review |
| `compositor_focus_window` | 957 | Open: caller/contract/lifetime equivalence review |
| `__fill_window_info_locked` | 984 | Open: caller/contract/lifetime equivalence review |
| `compositor_window_info` | 1016 | Open: caller/contract/lifetime equivalence review |
| `sys_window_enum` | 1036 | Open: caller/contract/lifetime equivalence review |
| `compositor_window_snapshot` | 1069 | Open: caller/contract/lifetime equivalence review |
| `compositor_window_grid` | 1179 | Open: caller/contract/lifetime equivalence review |
| `__free_retired_window_resources` | 1209 | Open: caller/contract/lifetime equivalence review |
| `compositor_destroy_window` | 1223 | Open: caller/contract/lifetime equivalence review |
| `compositor_destroy_windows_by_pid` | 1258 | Open: caller/contract/lifetime equivalence review |
| `compositor_get_buffer` | 1319 | Open: caller/contract/lifetime equivalence review |
| `compositor_get_window_by_pid` | 1336 | Open: caller/contract/lifetime equivalence review |
| `compositor_move_window` | 1377 | Open: caller/contract/lifetime equivalence review |
| `compositor_set_window_draw_size` | 1413 | Open: caller/contract/lifetime equivalence review |
| `compositor_resize_window` | 1438 | Open: caller/contract/lifetime equivalence review |
| `__clear_other_carets_locked` | 1522 | Open: caller/contract/lifetime equivalence review |
| `compositor_focus_changed` | 1543 | Open: caller/contract/lifetime equivalence review |
| `compositor_window_write` | 1556 | Open: caller/contract/lifetime equivalence review |
| `draw_rect_internal` | 1625 | Open: caller/contract/lifetime equivalence review |
| `compositor_draw_rect` | 1685 | Open: caller/contract/lifetime equivalence review |
| `compositor_blit` | 1696 | Open: caller/contract/lifetime equivalence review |
| `compositor_set_window_flags` | 1828 | Open: caller/contract/lifetime equivalence review |

### `kernel/graphics/compositor_style.c`

Existing destination/seam: `system/lib/gfx/theme.c; user/sys/services/nxcompositor.c`. Separate reusable palette algorithms from authoritative cross-process service state.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `compositor_style_active` | 182 | Open: caller/contract/lifetime equivalence review |
| `compositor_theme_active` | 186 | Open: caller/contract/lifetime equivalence review |
| `compositor_background_active` | 190 | Open: caller/contract/lifetime equivalence review |
| `compositor_titlebar_height` | 194 | Open: caller/contract/lifetime equivalence review |
| `compositor_set_style` | 199 | Open: caller/contract/lifetime equivalence review |
| `compositor_set_theme` | 207 | Open: caller/contract/lifetime equivalence review |
| `compositor_set_background` | 215 | Open: caller/contract/lifetime equivalence review |

### `kernel/graphics/font.c`

Existing destination/seam: `system/lib/font_lib.c; pending nxfontdd/SDK extension`. Normal font parsing, metrics and rasterization leave kernel; emergency-only path isolated.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `gl_draw_char_clipped` | 135 | Open: caller/contract/lifetime equivalence review |
| `gl_draw_char` | 211 | Open: caller/contract/lifetime equivalence review |
| `graphics_char_width` | 232 | Open: caller/contract/lifetime equivalence review |
| `gl_draw_string` | 258 | Open: caller/contract/lifetime equivalence review |
| `gl_draw_string_clipped` | 284 | Open: caller/contract/lifetime equivalence review |
| `graphics_string_width` | 323 | Open: caller/contract/lifetime equivalence review |
| `graphics_font_height` | 364 | Open: caller/contract/lifetime equivalence review |
| `graphics_font_ascent` | 387 | Open: caller/contract/lifetime equivalence review |
| `graphics_font_max_width` | 398 | Open: caller/contract/lifetime equivalence review |
| `sys_set_font` | 414 | Open: caller/contract/lifetime equivalence review |

### `kernel/graphics/gl.c`

Existing destination/seam: `system/lib/gfx/raster.c; system/lib/gfx/alpha.c`. Pixel operations and blending leave kernel; map each actual caller before removing symbols.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `clip` | 49 | Open: caller/contract/lifetime equivalence review |
| `blend_over` | 72 | Open: caller/contract/lifetime equivalence review |
| `gl_clear` | 94 | Open: caller/contract/lifetime equivalence review |
| `gl_draw_pixel` | 120 | Open: caller/contract/lifetime equivalence review |
| `gl_draw_line` | 140 | Open: caller/contract/lifetime equivalence review |
| `gl_draw_rect` | 168 | Open: caller/contract/lifetime equivalence review |
| `gl_draw_rect_fill` | 199 | Open: caller/contract/lifetime equivalence review |
| `gl_swizzle_bgr` | 221 | Open: caller/contract/lifetime equivalence review |
| `gl_blit` | 268 | Open: caller/contract/lifetime equivalence review |

### `kernel/graphics/graphics.c`

Existing destination/seam: `display service/provider seam; emergency kernel console`. Split scanout/resource mechanism from normal rendering and display policy.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `graphics_init` | 35 | Open: caller/contract/lifetime equivalence review |
| `graphics_screen_surface` | 55 | Open: caller/contract/lifetime equivalence review |
| `graphics_present_argb8888` | 79 | Open: caller/contract/lifetime equivalence review |
| `graphics_draw_pixel` | 109 | Open: caller/contract/lifetime equivalence review |
| `graphics_draw_rect` | 125 | Open: caller/contract/lifetime equivalence review |
| `graphics_clear` | 142 | Open: caller/contract/lifetime equivalence review |
| `graphics_swap_buffers` | 159 | Open: caller/contract/lifetime equivalence review |
| `panic_screen` | 175 | Open: caller/contract/lifetime equivalence review |
| `graphics_draw_char` | 221 | Open: caller/contract/lifetime equivalence review |
| `graphics_draw_string` | 239 | Open: caller/contract/lifetime equivalence review |

### `kernel/graphics/region.c`

Existing destination/seam: `user/sys/services/nxcompositor.c and SDK graphics library`. Port bounded damage/occlusion behavior; current full-frame composition is not equivalent.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `region_create` | 55 | Open: caller/contract/lifetime equivalence review |
| `region_destroy` | 79 | Open: caller/contract/lifetime equivalence review |
| `region_scratch_reset` | 104 | Open: caller/contract/lifetime equivalence review |
| `region_copy_into` | 111 | Open: caller/contract/lifetime equivalence review |
| `region_add_rect` | 149 | Open: caller/contract/lifetime equivalence review |
| `region_subtract` | 223 | Open: caller/contract/lifetime equivalence review |
| `region_intersect_rect` | 295 | Open: caller/contract/lifetime equivalence review |
| `region_clear` | 327 | Open: caller/contract/lifetime equivalence review |

### `kernel/graphics/surface.c`

Existing destination/seam: `kernel/resources for validated metadata; system/lib/gfx for pixel work`. File location is not authority: retain only bounds/lifetime/mapping/capability control.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `gfx_surface_valid` | 5 | Open: caller/contract/lifetime equivalence review |
| `gfx_surface_verify` | 14 | Open: caller/contract/lifetime equivalence review |
| `gfx_surface_clear` | 34 | Open: caller/contract/lifetime equivalence review |
| `gfx_surface_fill` | 39 | Open: caller/contract/lifetime equivalence review |
| `gfx_surface_composite_over` | 47 | Open: caller/contract/lifetime equivalence review |

### `kernel/graphics/term.c`

Existing destination/seam: `system/lib/gfx/tty_model.c; user/sys/services/nxttyd.c`. Existing userland VT and TTY service are destinations; remove legacy kernel text route after caller cutover.

| Current function | Current line | Migration status |
| --- | ---: | --- |
| `term_fill_rect` | 21 | Open: caller/contract/lifetime equivalence review |
| `term_xterm256` | 37 | Open: caller/contract/lifetime equivalence review |
| `handle_sgr` | 59 | Open: caller/contract/lifetime equivalence review |
| `csi_params` | 126 | Open: caller/contract/lifetime equivalence review |
| `term_clear_cell` | 144 | Open: caller/contract/lifetime equivalence review |
| `term_erase_caret` | 161 | Open: caller/contract/lifetime equivalence review |
| `term_draw_cursor` | 181 | Open: caller/contract/lifetime equivalence review |
| `handle_csi` | 211 | Open: caller/contract/lifetime equivalence review |
| `term_init` | 323 | Open: caller/contract/lifetime equivalence review |
| `term_free` | 362 | Open: caller/contract/lifetime equivalence review |
| `term_resize` | 373 | Open: caller/contract/lifetime equivalence review |
| `term_write` | 404 | Open: caller/contract/lifetime equivalence review |
| `term_set_focus` | 504 | Open: caller/contract/lifetime equivalence review |
| `term_clear_caret` | 512 | Open: caller/contract/lifetime equivalence review |

## Gate

115 current function definitions inventoried. A legacy symbol is retired only after all active callers use the verified SDK/service or kernel mechanism replacement. Pure algorithms already present in the SDK are reused; state ownership, authorization and lifecycle are reviewed separately. Build success, name similarity and relocation alone do not demonstrate equivalence.
