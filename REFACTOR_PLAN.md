# OS1 Refactor Plan — Kernel Stabilization & Architecture

**Date**: 2026-05-10  
**Branch**: dev-research  
**Author**: Ken Thompson mode — full liberty to restructure for correctness and simplicity

---

## Principles

1. **Stability before features** — every phase must pass `make run` before the next begins
2. **No mixed commits** — one commit per phase, stability fixes separate from refactors
3. **Verifiable exit criteria** — each phase has a concrete, observable outcome
4. **Lock discipline** — `sched_lock → compositor_lock` always in that order, never reversed
5. **No speculative abstractions** — only implement what the running system needs
6. **POSIX ABI v compatible** — syscall numbers and calling convention preserved

---

## Phase 0 — Panic & Diagnostics Foundation ✅ IN PROGRESS

**Goal**: Make crashes debuggable. One panic = one clean trace, no cascades.

**Problems**:
- `panic()` halts only the panicking CPU; other 3 CPUs continue running
- Serial output from 4 CPUs interleaved → crash dumps unreadable
- No CPU ID in log lines

**Changes**:
- `kernel/lib/printk.c`: Add `volatile int panic_flag` global; in `panic()` set flag + send IPI to all CPUs (SGI0); add `[C%d]` CPU prefix to every `vprintk` line
- `kernel/drivers/gic/gic.c` + `gic.h`: Add `gic_send_ipi_all()` using `gic_send_sgi(0, target_list_all_except_self)`
- `kernel/drivers/timer/timer.c`: At top of `timer_handler()`, check `panic_flag`; if set → halt this CPU (disable timer, WFE loop)

**Exit criteria**: After a panic, `[System halted]` appears and system stops. No cascading `[Init] Notification Server died` messages after halt.

---

## Phase 1 — Critical Crash Fixes

**Goal**: Fix all identified crashes. System must boot to shell, close button must not crash.

### 1a — Close Button (process_terminate premature free)

**Root cause**: `process_terminate(pid)` (called from `compositor_handle_click`) immediately frees the process kernel stack, page tables, and `proc` struct via `pmm_free_page(proc)`. If the process is currently scheduled on another CPU, that CPU accesses freed memory → EC=0x25 data abort.

**Fix**: 
- In `process_terminate`, when `current_process != proc`: mark PROC_DEAD but **do not free** kernel_stack, page_table, or proc struct
- The process is already removed from the runqueue and from `process_pool[]`
- Add a `deferred_free_list` (static array of process pointers) and push to it
- CPU 0 timer handler: after `schedule()`, drain `deferred_free_list` and free dead processes that are no longer running (`proc->on_cpu == -1`)

**Additional**: `compositor_handle_click` accesses `windows[]` without lock — add lock around the hit-detection loop.

**Exit criteria**: Clicking the red close button terminates the process and removes its window. No crash.

### 1b — schedule() crash at 0x4000abb0 (EC=0x25, `str x19, [x24, #16]`)

**Root cause** (suspected): After Phase 0 and 1a are applied, this crash may resolve. If not, diagnose from clean logs.

### 1c — ELR=0x0 cascade (PID 2 idle task)

After Phase 0 eliminates the cascade, this may be a spurious secondary crash. Diagnose from clean single-trace logs.

**Exit criteria**: `make run` boots to shell, no panics in first 30s.

---

## Phase 2 — Render Loop: Vsync + Damage Optimization

**Goal**: Screen updates at 30Hz without waiting for input. No wasted CPU cycles.

**Current state**:
- `compositor_tick()` is called every tick (up to 100Hz) but only renders if `compositor_dirty`
- `COMPOSITOR_TARGET_FPS = 60` but interval calculation has no vsync alignment
- `compositor_render_internal()` calls `memcpy(fb_va, backbuffer, bb_w * bb_h * 4)` on every render even for 1-pixel change

**Changes**:
- `kernel/drivers/gpu/virtio_gpu.c`: 
  - **CRITICAL**: Swap flush order — `TRANSFER_TO_HOST_2D` must precede `RESOURCE_FLUSH`.
  - Fix command buffer race conditions (if any) using a per-device lock.
- `kernel/graphics/compositor.c`: 
  - Implement `compositor_damage_rect(x, y, w, h)` and call it from all drawing syscalls (`blit`, `draw_rect`).
  - Harden `compositor_blit` with `copy_from_user` to prevent kernel panics on invalid user pointers.
  - Optimize `compositor_render_internal` to only `memcpy` dirty rectangle rows to the hardware framebuffer.

**Exit criteria**: Screen refreshes at ~30fps during idle (visible with moving cursor). No regressions on shell/demo3d. CPU utilization lower than before.

---

## Phase 3 — HAL Split (arch independence, prepares amd64)

**Goal**: Zero architecture-specific code outside `kernel/arch/aarch64/`. All `mrs/msr`, assembly instructions, MMIO addresses behind typed C interfaces.

**Current state**: Some `mrs`/`msr` macros scattered in drivers. GIC addresses hardcoded in `gic.h`. Timer registers in timer.c.

**Changes**:
- `kernel/arch/aarch64/include/arch/arch.h`: Already has most macros. Audit and complete.
- `kernel/include/kernel/arch.h`: Define the HAL contract (function signatures only, no implementation)
- Move GICD/GICC base addresses to `kernel/arch/aarch64/include/arch/platform.h`
- `kernel/drivers/gic/gic.c`: Replace `0x08000000` hardcodes with `GICD_BASE` from platform.h (already done in gic.h — verify)
- No amd64 implementation yet — just clean interface

**Exit criteria**: `grep -r "mrs\|msr\|0x0800\|0x0801" kernel/drivers/` returns nothing outside arch/. Build passes.

---

## Phase 4 — VFS Layer

**Goal**: `open/read/write/close` via a `struct vfs_ops` interface. Ext4 becomes a plugin.

**Current state**: `ext4.c` has direct function calls `ext4_read_file(path, buf, size, offset)`. No inode caching, no path resolution cache.

**Changes**:
- `kernel/fs/vfs.h` + `kernel/fs/vfs.c`: Define `struct vfs_node`, `struct vfs_ops { open, read, write, close, stat, readdir }`, path resolution
- `kernel/fs/ext4.c`: Wrap existing functions with `vfs_ops` interface
- `kernel/arch/aarch64/cpu/syscall.c`: `SYS_FILE_READ/WRITE` now go through `vfs_read/vfs_write`
- User `init.c`: Parse `/init.cfg` via the new VFS (existing TODO)

**Exit criteria**: `make run`, shell `cat /init.cfg` works. Existing file tests pass.

---

## Phase 5 — Process Manager Completeness (seL4 inspiration)

**Goal**: Robust capability-based process lifecycle. No memory leaks on process death.

**Current gaps**:
- No signals (SIGTERM/SIGKILL)
- No pipes
- Wait queue cleanup on process death incomplete
- IPC queue not drained on process death (memory leak)
- No process group / session support

**Changes**:
- `kernel/sched/process.c`: 
  - `process_terminate`: Drain IPC queue on death (free all `ipc_node`)
  - Add `process_signal(pid, sig)` stub (SIGTERM = terminate, SIGKILL = force)
  - Audit all `kmalloc`/`pmm_alloc` in process lifecycle for matching frees
- `kernel/include/kernel/sched.h`: Add `PROC_PERM_*` capability flags (already started)
- Linker script: Verify stack sizes — kernel stack 128KB, user stack 64KB as declared

**Exit criteria**: Run 10 processes via shell, kill them all, run again. No memory exhaustion. `make run` stable for 60s.

---

## Phase 6 — Graphics Split: Kernel Driver vs User-Space Compositor

**Goal**: Kernel retains only GPU HAL + framebuffer DMA. Compositor logic moves to user-space privileged process.

**Why last**: This is the riskiest change. All other phases must be stable first.

**Architecture**:
```
Kernel:
  drivers/gpu/virtio_gpu.c  — VirtIO GPU HAL (DMA, flush, resource mgmt)
  drivers/gpu/gpu_core.c    — GPU device abstraction
  kernel syscalls:
    SYS_GPU_MAP_FB           — Map framebuffer page into compositor process
    SYS_GPU_FLUSH            — Flush region to display
    SYS_GPU_GET_INFO         — Get display dimensions

User (privileged PID 2):
  user/bin/compositor.c     — Full compositor, window manager
  user/lib/compositor_lib.c — Shared API for apps
```

**Migration path**:
- Keep `kernel/graphics/compositor.c` as-is during migration
- Build new `user/bin/compositor.c` that replicates functionality using the GPU syscalls
- Once new compositor is stable, remove kernel-side one
- Update all SYS_CREATE_WINDOW etc. to be IPC messages to compositor PID

**Exit criteria**: `make run` works with user-space compositor. No regressions on windows, close button, drag.

---

## POSIX Completeness (ongoing, not a separate phase)

- `read/write/open/close` via VFS (Phase 4)
- `fork` — not planned (ELF spawn is sufficient)
- `exec` — consider implementing as `spawn` replacement
- `waitpid` — partially done (`process_wait`)
- `getpid/getppid` — done
- Signal delivery — Phase 5

---

## Linker Script Audit (Phase 0 sub-task)

Current `kernel/arch/aarch64/kernel.ld` — review:
- Kernel stack: defined in `start.S` as `.space 128*1024` — verify
- User stack: allocated per-process in `process_create` as `STACK_SIZE = 128KB` — check
- Heap: kmalloc uses 32MB PMM region — adequate for current load

---

## Progress Tracker

| Phase | Status | Commit |
|-------|--------|--------|
| 0     | IN PROGRESS | — |
| 1a    | PENDING | — |
| 1b    | PENDING | — |
| 1c    | PENDING | — |
| 2     | PENDING | — |
| 3     | PENDING | — |
| 4     | PENDING | — |
| 5     | PENDING | — |
| 6     | PENDING | — |
