> STATUS: agent-generated, **maintainer spot-checked** (2026-06-02) — see REVIEW.md Corrections section.

# Subsystem Analysis 06 — Graphics (`graphics/`)

> Severity/kind tags are defined in [`../TAXONOMY.md`](../TAXONOMY.md).
> Evidence basis: **[verified]** built; **[static]** read-only; **[inferred]** reasoned.

| | |
|---|---|
| **Subsystem** | Graphics |
| **Sources** | `kernel/graphics/graphics.c` (81), `kernel/graphics/region.c` (175), `kernel/graphics/gl.c` (144), `kernel/graphics/font.c` (191), `kernel/graphics/compositor.c` (1229) |
| **Out-of-scope sibling files** | `kernel/graphics/draw2d.c`, `kernel/graphics/draw3d.c` — present but not listed in the review scope; noted where they intersect. |
| **Headers** | `kernel/include/kernel/graphics.h`, `kernel/include/kernel/region.h`, `kernel/include/graphics/gl.h`, `include/api/font.h`, `kernel/include/graphics/default_font.h` |
| **Arch hooks** | `arch_copy_from_user` (aarch64: `arch/aarch64/cpu/syscall.c:35`; amd64: `arch/amd64/mm/uaccess.c:17`), `arch_mb` (barrier in `graphics_swap_buffers`), `gpu_get_primary()` (HAL) |
| **Build** | **[inferred]** Both kernels build with this subsystem present (per project owner). Individual static analysis reveals no missing symbol other than the unimplemented `gl_draw_rect` and `gl_swizzle_bgr` (declared in `gl.h`, absent in `gl.c`; no callers found in kernel `.c` files → no link error). |

---

## 1. Purpose & Role

The graphics subsystem provides five layers:

1. **HAL bridge (`graphics.c`)** — wraps `gpu_get_primary()` to initialise a single `graphics_context` and a static `gl_surface` pointing at the HAL framebuffer. Forwards all draw calls to GL.
2. **2D rasteriser (`gl.c`)** — software pixel primitives operating on `struct gl_surface` (clear, pixel, line, filled-rect, alpha blit). Uses integer/byte arithmetic and Porter-Duff "src over" blending. Not the project's 3D engine — that lives in the out-of-scope `draw3d.c`.
3. **Font renderer (`font.c`)** — per-glyph alpha-masked blit from a pre-rasterised bitmap font embedded via `<graphics/default_font.h>`. Also provides `sys_set_font()`, a syscall allowing userland to replace the active font.
4. **Region manager (`region.c`)** — list-of-rectangles implementation supporting add, subtract, intersect-clip, and destroy. Used by the compositor for occlusion culling.
5. **Compositor (`compositor.c`)** — manages up to 32 on-screen windows (pixel buffers + terminal state), implements a painter's-algorithm/visibility-region render to a pre-allocated backbuffer, and uploads the damage bounding box to the GPU. Also owns: drag-and-drop, mouse hit-testing, PID-based window ownership checks, ANSI-escape terminal emulation, and close-button→`process_terminate` coupling.

---

## 2. Data Flow

```
gpu_get_primary()
      │
      ▼
graphics_init() → g_ctx (720×1280 on both arches)
      │
      │  graphics_get_screen_surface()
      │        │
      ▼        ▼
gl_{clear,draw_pixel,draw_rect_fill,blit}(gl_surface)
                 ▲                ▲
    font.c ──────┘                │
    (gl_draw_char, gl_draw_string)│
                                  │
compositor_init()                 │
      │  compositor_backbuffer ───┘   (720×1280 kmalloc)
      │
      ├── compositor_create_window() → windows[] slot + pixel buffer (kmalloc)
      │         + text_grid + attr_grid (kmalloc)
      │
      ├── compositor_window_write() ─→ ANSI parse → gl_draw_char → expand_damage
      ├── compositor_blit()          ─→ vmm_copy_from_user → windows[].buffer
      ├── compositor_update_mouse()  ─→ drag: mutates windows[].x/y (NO LOCK)
      ├── compositor_handle_click()  ─→ z-order, keyboard_focus_pid, IPC send,
      │                                  process_terminate() (extern kernel fn)
      │
      └── compositor_render_internal()
               │ sort by z_order (bubble)
               │ region occlusion (region_create/subtract/intersect)
               │ blit windows[].buffer → compositor_backbuffer
               │ gradient background
               │ cursor rasterise
               └── gpu->ops->flush(damage rect)
```

---

## 3. What Works

- **[static]** All pixel-writing paths in `gl.c` and `draw_rect_internal` are individually bounds-checked against surface width/height before any buffer write (`gl_draw_pixel:49`, `gl_draw_rect_fill:85-93`, `draw_rect_internal:compositor.c:1116`). A runaway coordinate cannot write outside the window buffer.
- **[static]** `compositor_blit` clips X and Y independently before calling `vmm_copy_from_user` (`compositor.c:1162-1191`), preventing an overlong row from overrunning the window buffer.
- **[static]** `compositor_blit` uses `vmm_copy_from_user` / `arch_copy_from_user` rather than raw `memcpy`, providing address-space validation for the blit data path.
- **[static]** `compositor_create_window` validates dimensions (`w>0, h>0, w<=4096, h<=4096`, `compositor.c:148`) before allocating.
- **[static]** `region_add_rect` rejects zero/negative-dimension rects at entry (`region.c:33`). Redundant coverage but correct.
- **[static]** The `occluded` region in `compositor_render_internal` is nulled at line 914 immediately after its first destroy, making the second `region_destroy(occluded)` at line 1054 a safe no-op (NULL-guarded by `region_destroy`, `region.c:23`).
- **[static]** `gl_blit` correctly implements three-way Porter-Duff: skip α=0, fast-copy α=255, blend otherwise (`gl.c:131-142`). The in-code comment documents the pre-existing alpha-test bug that was already corrected.
- **[static]** `graphics_font_max_width` floors to 8 (`font.c:168`), preventing a zero char-width division in `compositor_create_window` for the column calculation.
- **[static]** The compositor uses `spin_lock_irqsave` + trylock correctly in almost every public API, protecting `windows[]` from the main code paths.

---

## 4. Central Theme (the thing to fix first)

**`sys_set_font` stores an unvalidated, uncopied user pointer into kernel-global state, which is later dereferenced during IRQ-context rendering.**

`sys_set_font` (`font.c:174`) is dispatched at syscall 253 (`syscall_dispatch.c:234`) by passing `arg0` — a raw user-space virtual address — directly to the function with no `copy_from_user`, no address-space validation, and no copy-to-kernel-heap. The function casts it to `struct font_header *`, reads from it, and stores derived interior pointers into `current_font.glyphs` and `current_font.bitmap` (`font.c:186-187`). `gl_draw_char` dereferences those pointers (`font.c:49,50,61`) during every character render, including in `compositor_tick()` which fires from a timer IRQ while an *arbitrary* process's page tables are loaded. Consequences: (a) a process can set-font then exit, leaving dangling kernel pointers to freed/unmapped user memory — guaranteed fault on next text render; (b) a crafted font blob at a known kernel-address can disclose kernel memory via the framebuffer; (c) there is a possible integer overflow in the size check at line 181 if `h->num_chars` is near `SIZE_MAX / sizeof(struct font_glyph_info)`.

This is the subsystem's highest-severity finding, and its fix is mechanical: copy the entire font blob into a `kmalloc`-ed kernel buffer inside `sys_set_font`, validate all offsets against the copied data, and reject the call if metrics produce a zero height.

A second theme — equally important for the stated seL4 extraction goal — is **compositor-to-kernel entanglement**: `compositor_handle_click` calls `extern int process_terminate(int pid)` (`compositor.c:673`) and `kernel_ipc_send` (`compositor.c:655`), reads and writes the scheduler-owned global `keyboard_focus_pid` (`compositor.c:266,270,296,297,636,639`), and accesses the GPU HAL directly. These couplings are the primary technical barrier to extracting the compositor into a userland service.

---

## 5. Findings

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| GFX-FONT-01 | W4 | SECURITY · BUG | `font.c:174-191`, `syscall_dispatch.c:234` | `sys_set_font` stores raw user pointer into kernel globals; dereferenced during IRQ-context rendering. |
| GFX-COMP-01 | W3 | SECURITY · WRONG-DESIGN | `compositor.c:266,270,296,297,636,639` | `keyboard_focus_pid` written from compositor without compositor_lock, races scheduler and keyboard driver. |
| GFX-COMP-02 | W3 | BUG · SECURITY | `compositor.c:698-757` | `compositor_update_mouse` mutates `windows[i].x/y` and damage globals in IRQ context with no lock. |
| GFX-COMP-03 | W3 | WRONG-DESIGN | `compositor.c:655,673,672-673` | Compositor directly calls `process_terminate` and `kernel_ipc_send`; blocks extraction to userland. |
| GFX-COMP-04 | W2 | BAD-IMPL | `compositor.c:318-325` | `compositor_get_buffer` is a public API returning a raw unlocked kernel pointer; no callers currently exist outside compositor.c, but wiring it to a syscall would create a latent UAF on concurrent destroy. |
| GFX-COMP-05 | W2 | BAD-IMPL | `compositor.c:370-378` | `compositor_move_window` mutates window position with no lock; no external callers currently, but the missing lock is a latent race for any future caller. |
| GFX-FONT-02 | W3 | BUG | `font.c:148-149` | `graphics_font_height()` returns `ascent+descent` from font header without checking for zero; `h/char_h` divide-by-zero in `compositor_create_window:204` if a malformed font is loaded. |
| GFX-COMP-06 | W2 | PERF | `compositor.c:810-838` | O(n²) bubble sort + O(n²) top-most promotion runs every frame tick even when window set is static. |
| GFX-COMP-07 | W2 | PERF | `compositor.c:853-879` | Occlusion pass allocates a `region_create()` per window per frame (≤32 kmalloc+kfree pairs per tick). |
| GFX-COMP-08 | W2 | BAD-IMPL | `compositor.c:407-439` | `handle_sgr` concatenates escape digits then parses only a single integer; multi-param sequences (e.g. `\e[1;32m`) silently ignored. |
| GFX-COMP-09 | W2 | WRONG-DESIGN | `compositor.c:59,63-76,1047-1050` | Damage tracking is a bounding-box, not a region; a single pixel update anywhere forces flushing the entire width of the damage bounding box including unmodified columns. |
| GFX-COMP-10 | W2 | BAD-IMPL | `compositor.c:981-993` | Title text is rendered unclipped to the backbuffer after the visibility-region blit; overlaps neighbour windows. |
| GFX-GL-01 | W1 | MISSING | `gl.h:24`, `gl.c` (absent) | `gl_draw_rect` (unfilled outline) and `gl_swizzle_bgr` are declared in `gl.h` but not implemented in `gl.c`; no callers found in kernel `.c` files, so no link error currently. |
| GFX-COMP-11 | W1 | BAD-IMPL | `compositor.c:770,773` | `in_render` atomic guard is redundant: both callers (`compositor_render:1070`, `compositor_tick:1083`) already hold `compositor_lock` with IRQs off; the guard adds confusion without safety benefit. |
| GFX-COMP-12 | W1 | DOC | `compositor.c:246` | Comment says "Mark main shell (PID 2) as protected" but `keyboard_focus_pid` defaults to 7 (`sched/process.c:27`); the "Shell PID 7" comment in `compositor_destroy_window:270` is the live path. |
| GFX-GFX-01 | W0 | DOC | `graphics.c:30-40` | `graphics_get_screen_surface` returns a pointer to a `static` local; thread-safe under a single-core kernel but not under SMP if callers store the pointer across preemption. Worth a comment. |

> **Post-review addendum — GFX-COMP-13 (user-reported, after the 2026-06-02 snapshot).**
> W4 · BUG (SMP deadlock). `compositor_handle_click` held `compositor_lock` across
> `kernel_ipc_send` / `process_terminate` (`compositor.c:655`, pre-fix) — an AB-BA inversion
> vs `process_terminate` (which takes `sched_lock` → `compositor_lock`). On `-smp 4`, closing a
> window then `kill`-ing the resulting process froze the machine with no panic. Filed as issue
> **#100**, **fixed** in commit `6fd1b47` (capture the IPC message + close decision under the
> lock, release it, then make the cross-subsystem calls). The fix resolves **only the freeze**;
> the companion reap-from-IRQ-context symptom remains (tracked via SCHED-03), and the
> design-level coupling itself is **GFX-COMP-03** above (still open). Belongs to the
> SMP-correctness epic #96.

---

## 6. Detailed Entries (top findings)

### GFX-FONT-01 — `sys_set_font` stores a raw user pointer into kernel IRQ-context globals `[static · SECURITY]`

`syscall_dispatch.c:234` dispatches syscall 253 as:
```c
pt_regs_set_return(frame, sys_set_font((void *)arg0, (size_t)arg1));
```
`arg0` is the raw userland virtual address supplied by the calling process. `sys_set_font` (`font.c:174-191`) casts it directly to `struct font_header *` and reads `h->magic`, `h->num_chars`, and `h->bitmap_size` from user memory — with no `vmm_is_user_addr` check, no `copy_from_user`, no address-space lock. It then stores interior pointers derived from `arg0` into the kernel-global `current_font.glyphs` and `current_font.bitmap` (lines 186-187). These are read by `gl_draw_char` (font.c:49,50,61) on every character render, including from `compositor_tick` which fires from the timer IRQ while any process's page tables are active.

Attack / failure scenarios (all [inferred] but mechanically sound given the code):

1. **Process exit after set-font → IRQ fault.** The user process sets font to its own data, then exits. Its page frames are recycled. The next `compositor_tick` dereferences `current_font.bitmap` into unmapped memory → fault in IRQ.
2. **Kernel-address passed as `arg0`.** There is no `vmm_is_user_addr` check. A process can pass a kernel text-segment address; `gl_draw_char` will render kernel bytes as glyph bitmaps visible in the framebuffer → information disclosure.
3. **`size` is caller-supplied and never verified against the process address space.** The check `size < expected` (`font.c:182-183`) compares two caller-controlled values: `arg1` (the claimed size) against `expected` (derived from fields inside the buffer at `arg0`). Neither is independently validated via `vmm_check_range`. A process can claim `size = expected` with a real-looking header, pass the check, then have the buffer mapped smaller — the in-IRQ glyph deref walks off the end of the mapped page.

*Fix direction:* inside `sys_set_font`, validate `arg0` with `vmm_is_user_addr`, compute `expected` safely, `kmalloc(expected)`, `copy_from_user` the whole blob, re-validate all internal offsets against the kernel copy, reject if `ascent + descent == 0`.

---

### GFX-COMP-02 — `compositor_update_mouse` mutates shared state in IRQ context without a lock `[static · BUG·SECURITY]`

`compositor_update_mouse` (`compositor.c:698-757`) is called from the mouse driver (IRQ context). It reads and writes `windows[i].x`, `windows[i].y` during drag (730-744), the `damage_x1/x2/y1/y2` globals (749-754 via `expand_damage`), and `compositor_dirty` (756) — all without holding `compositor_lock`. The compositor render path (`compositor_render_internal`) holds `compositor_lock` while traversing `windows[]`. An IRQ mid-render can therefore produce a torn window position (half the render sees old X, half sees new) or tear the damage bounding box. The `expand_damage` function (`compositor.c:67-77`) is not atomic; six non-atomic reads/writes to the four damage coordinates can be interleaved with the flush path at `compositor.c:1032-1050`.

*Fix direction:* take `compositor_lock` (via `spin_lock_irqsave`) at the top of `compositor_update_mouse`, as every other public compositor function does. The existing trylock-in-tick pattern already tolerates the lock being busy.

---

### GFX-COMP-03 — Compositor directly invokes scheduler and IPC from within the input-event handler `[static · WRONG-DESIGN]`

`compositor_handle_click` (`compositor.c:591-692`) embeds:
- `extern int process_terminate(int pid);` declared and called at lines 672-673. The lock is correctly released at line 667 before the call (because `process_terminate` → `compositor_destroy_windows_by_pid` re-acquires it, and spinlocks are non-recursive). The WRONG-DESIGN is that the compositor *knows about* and directly *calls* `process_terminate` at all.
- `kernel_ipc_send(keyboard_focus_pid, &msg)` at line 655 while holding `compositor_lock`.
- Direct read/write of `keyboard_focus_pid` (owned by `kernel/sched/process.c`) at lines 636,639 without holding the scheduler lock.

For a seL4-style extraction, the compositor must not call `process_terminate`, send IPC, or touch scheduler state directly. In a service model, the compositor would post a "close requested" event to an event queue, and a privileged process manager would act on it.

---

### GFX-COMP-01 — `keyboard_focus_pid` written without consistent locking `[static · SECURITY]`

`keyboard_focus_pid` is declared in `kernel/sched/process.c:27` and referenced `extern` in `kernel/include/kernel/sched.h:148`. The compositor writes it at lines 266, 270, 296, 297, 636, 639 while holding `compositor_lock`, but the keyboard driver (`drivers/keyboard/keyboard.c:192,196,221`) reads it without that lock. The scheduler (`syscall_dispatch.c:167`) writes it from syscall context. These are three distinct lock domains mutating the same non-atomic `int`, a classic SMP data race. On a two-core SMP boot, a concurrent keyboard IRQ and compositor click handler can corrupt the focus PID silently.

---

### GFX-COMP-04 — `compositor_get_buffer` latent UAF: raw unlocked pointer in a public API `[static · BAD-IMPL]`

`compositor_get_buffer` (`compositor.c:318-325`) searches `windows[]` and returns `windows[i].buffer` without holding `compositor_lock`. No callers currently exist outside compositor.c itself, so there is no live UAF today. However, it is exported in `kernel/include/kernel/graphics.h` as a public API. Any future caller — or a syscall wiring — that stores the returned pointer and later writes through it after a concurrent `compositor_destroy_window` (which calls `kfree(windows[i].buffer)` under the lock) produces a use-after-free. The correct API is to accept a callback or provide a copy-out function, not to return a raw unlocked pointer to an internally-owned buffer.

---

### GFX-FONT-02 — Font height not validated; zero height causes divide-by-zero `[inferred]`

`graphics_font_height()` (`font.c:148-149`) returns `current_font.header.ascent + current_font.header.descent`. Both are `uint16_t` in `font_header`. `sys_set_font` (`font.c:174-191`) does not validate that `ascent + descent > 0`. If a malformed font with `ascent=0, descent=0` is loaded, `compositor_create_window` (`compositor.c:202-204`) computes `windows[slot].grid_rows = h / char_h` with `char_h == 0` → divide-by-zero. The assumption: a user process calls `sys_set_font` with a crafted header before any window is created. The `graphics_font_max_width` fallback to 8 (`font.c:168`) does not apply to height. [inferred — requires malformed font via sys_set_font]

---

## 7. Refactor Direction (toward the declared goals)

| Goal | Graphics subsystem implication |
|---|---|
| **seL4-style compositor-as-userland-service** | Remove `process_terminate`, `kernel_ipc_send`, `keyboard_focus_pid` from compositor.c (GFX-COMP-03/01). Replace with a message-passing windowing protocol: compositor posts events to a privileged event broker. Map per-window pixel buffers as shared memory (seL4 dataport / grant table). GPU flush remains in kernel (or a thin driver service). |
| **seL4-style font-manager-as-userland-service** | `sys_set_font` must copy the blob into kernel heap and validate it (GFX-FONT-01). Long-term: font management moves to a font-server process; `gl_draw_char` is called in that process's address space; the compositor receives pre-rendered glyph pixmaps via shared buffers. |
| **Plan 9 "everything is a file"** | Each window's pixel buffer becomes a file (e.g. `/dev/win/100/fb`) opened by its owning process. The compositor reads via shared mapping rather than `compositor_get_buffer`. `compositor_window_write` becomes a `write(2)` to `/dev/win/100/text`. Input events emerge from `/dev/win/100/events`. The damage-rectangle flush becomes the kernel's responsibility when the app calls `fsync` on the fb file. |
| **SMP correctness** | Lock `compositor_update_mouse` (GFX-COMP-02); make `keyboard_focus_pid` atomic or guard with a single dedicated spinlock (GFX-COMP-01); remove `in_render` redundancy (GFX-COMP-11). |
| **Performance** | Replace bubble sort with an insertion sort on window creation/z-change (GFX-COMP-06); pre-allocate a pool of regions (GFX-COMP-07); upgrade damage tracking to a true region list (GFX-COMP-09). |

**Suggested extraction sequencing:**
1. Fix GFX-FONT-01 (copy-in + validate) — mechanical, unblocks safe font loading.
2. Fix GFX-COMP-02 (lock mouse handler) — one-line spinlock add.
3. Make `keyboard_focus_pid` atomic or move to compositor-owned state protected by `compositor_lock`.
4. Introduce a "window event" ring buffer accessible from both compositor and input drivers, decoupling GFX-COMP-03.
5. Map per-window buffers as shared memory; remove `compositor_get_buffer` raw-pointer interface.
6. Promote compositor to ring-3 with a thin kernel "present" syscall for GPU flush.

**Extraction feasibility:** The font manager is moderately straightforward to extract — it is self-contained, has no scheduler coupling, and its only kernel dependency is the render surface; the primary work is the copy-in fix plus moving `gl_draw_char` into a font-server process with shared-buffer output. The compositor is significantly harder: it currently calls `process_terminate`, owns `keyboard_focus_pid`, sends IPC, and touches the GPU HAL directly; extraction requires decoupling all four of those entanglements and introducing a shared-memory surface protocol, representing a substantial windowing-protocol design effort comparable to a minimal Wayland-equivalent.

---

## 8. Verification Notes

- Build: **[inferred]** Both arches build (per project owner); `gl_draw_rect` / `gl_swizzle_bgr` declared in `gl.h` but not implemented in `gl.c`; no callers found in kernel C files → no link error.
- **[static]** Graphics initialises at 720×1280 via HAL on both arches: compositor hard-codes `bb_width=720, bb_height=1280` at `compositor.c:112-113`, matching the project owner's stated runtime fact.
- **[inferred / owner-provided]** A "Shell PID 7" window is created at 640×480 at runtime. In-source evidence: `keyboard_focus_pid = 7` (`sched/process.c:27`) and the `protected = (pid == 2)` comment discrepancy in GFX-COMP-12.
- **[static]** The 3D engine (`draw3d.c`) and high-level 2D helpers (`draw2d.c`) are out of scope for this review pass. `draw3d.c` uses `vec4_t`/`mat4_t` (floats, not fixed-point as stated in the project context) and a fixed-point sin/cos approximation for rotation; this should be noted for any 3D review.
- All findings in §5 with `[inferred]` tags state their preconditions inline and should be challenged against the actual scheduler / page-table state before promotion to a bug-fix ticket.
