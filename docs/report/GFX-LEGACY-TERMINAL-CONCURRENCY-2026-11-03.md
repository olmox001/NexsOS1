# GFX-LEGACY-TERMINAL-01 — terminal output, compositor lock and intermittent SSP fault

**Date:** 2026-11-03  
**Status:** finding confirmed; root cause of the stack-canary mismatch not yet proven; corrective design is now an explicit prerequisite of the graphics migration.  
**Scope:** legacy kernel graphics path only. This report does not change the approved NEXS-NG architecture and does not authorize moving or deleting the legacy compositor.

## 1. Observed failure

A clean AArch64 make test-all run reached NXShell and passed all 11 KTEST cases. The nxwins window-list command completed successfully; the test runner then began typing the follow-up focus command, but the serial log contains only the initial “/s” before the kernel panic. The reported NXWINS-FOCUS-FAIL is therefore a consequence of the interrupted command, not evidence that the focus operation itself failed. The run ended with:

- NXWINS-FOCUS-FAIL(1) and KERNEL-FAULT-DURING-SOAK;
- KERNEL PANIC / SSP: Stack Smash at 0xffff0000400af0b8;
- current process PID 8 (NXShell), boot phase K3-userland;
- the return site symbolized with the rebuilt AArch64 ELF as compositor_window_write, kernel/graphics/compositor.c:1902, at compositor_window_write+0x218, the stack-protector epilogue immediately after the spin_unlock_irqrestore() path;
- KTEST had already completed 11 PASSED, 0 FAILED before the runtime failure.

The failure serial is preserved at docs/report/logs/GFX-SSP-failure-aarch64-serial-2026-11-03.log; its QEMU manifest is docs/report/logs/GFX-SSP-failure-aarch64-manifest-2026-11-03.json and its pre-fault 1280x800 screenshot is docs/report/logs/GFX-SSP-failure-aarch64-screen-2026-11-03.png. The exception dump records EC=0x22, ELR/FAR=0x8001eb1b, SP=0xffff00004477e8a0, current PID 8. The same clean AArch64 test had passed in a preceding run, and later diagnostic runs completed repeated 30-second NXShell/window/focus soaks with KTEST=11/11, screenshots at 1280x800 and faults=0. Therefore the failure is intermittent. A passing run does not close it.

The SSP failure site identifies the function whose saved canary check failed; it does not by itself identify which write (if any) corrupted the stack. The captured exception context is not sufficient to attribute the mismatch to a specific renderer, IRQ, scheduler or test-runner operation. Do not label the cause as proven until the diagnostic canary snapshot or a reproducible debugger trace identifies it.

## 2. Verified legacy call path

The source currently implements this synchronous path:

1. kernel/core/syscall_dispatch.c::window_text_write() validates the request length (maximum SYSCALL_MAX_IO_BYTES), allocates a kernel bounce buffer, copies user bytes with arch_copy_from_user(), NUL-terminates it, mirrors it to UART and calls compositor_window_write() for a positive window ID.
2. kernel/graphics/compositor.c::compositor_window_write() acquires the global compositor_lock with spin_lock_irqsave(), finds the window, constructs a stack-local gl_surface, checks its geometry/capacity, sets terminal focus and calls term_write() before releasing the compositor lock.
3. kernel/graphics/term.c::term_write() parses ANSI/VT input, updates terminal cursor/grid state, scrolls pixels and cell grids, and calls glyph rendering/caret helpers synchronously.
4. kernel/graphics/font.c::gl_draw_char_clipped() takes font_lock and rasterizes glyph pixels into the surface while the caller still owns compositor_lock.

The surface capacity check validates the descriptor's geometry against its declared capacity; it does not independently prove the allocation's actual lifetime or pointer provenance. The compositor lock currently supplies lifetime protection for the window, terminal state and pixel buffer, but its scope includes CPU rendering work.

## 3. Existing documentation and architectural decision

This is not a new decision to remove the compositor or replace it with a different design. The approved direction already exists:

- docs/report/TIMER-SMP-DIAGNOSTIC-2026-11-03.md measured compositor_render() holds of roughly 34.7–72.3 ms on AMD64 and 40.7–130.0 ms on AArch64, and explicitly rejects simply replacing spin_lock_irqsave() with spin_lock() because IRQ mutators share the same lock.
- docs/PENDING-WORK.md §14 already records the required snapshot/ownership and IRQ-mutation work.
- docs/direction/DIR-02-compositor-decoupling.md and DIR-07-display-surface-resize.md preserve the legacy compatibility path while describing the NEXS-NG boundary.
- docs/architecture/GRAPHICS-USERLAND-MIGRATION.md makes the approved target explicit: compositor, terminal rendering, fonts and rasterization in ring-3; kernel retains capability-protected buffer/surface lifetime, validation, mapping, synchronization and presentation control.
- The integrated NEXS-NG plan requires lifetime/ownership audits and hostile concurrency tests before the buffer/surface and compositor migration gates.

The missing piece is an explicit legacy preflight item tying the intermittent SSP failure and terminal-output critical section to those already-approved phases. This report supplies that item; it does not change the target architecture.

## 4. What is established vs. still unknown

**Established:**

- terminal rendering runs while compositor_lock is held with local IRQs masked in compositor_window_write();
- the same global lock protects window lifetime and is also used by compositor rendering and input/focus mutation paths;
- glyph rasterization takes a nested font_lock and performs per-pixel CPU work;
- independent timing diagnostics already show long compositor lock holds on both architectures;
- at least one AArch64 runtime produced an SSP canary mismatch at the compositor_window_write() epilogue; at least one subsequent AArch64 full test passed.

**Not established:**

- that the long critical section itself overwrote the canary;
- that the terminal pixel buffer, cell grids, glyph bitmap, stack, IRQ frame or scheduler context is the corrupting writer;
- that nxwins or QMP focus handling caused the fault rather than merely being the last observed interaction;
- that AMD64 is immune (the recorded fault is AArch64; both architectures still require regression coverage).

### Timer/preemption hypothesis to discriminate

The AArch64 return sequence leaves a narrow interval after compositor_window_write() releases compositor_lock with spin_unlock_irqrestore() and before the compiler's SSP epilogue checks the canary. kernel/core/timer.c::kernel_timer_tick() calls schedule(regs) on every CPU timer IRQ, including ticks that interrupt EL1 kernel code; exception.S saves the 816-byte IRQ frame on the interrupted kernel stack and restore_context() resumes or switches tasks. A pending timer IRQ can therefore preempt this exact interval. This is a concrete competing hypothesis for the intermittent failure, not proof that the scheduler or IRQ frame overwrote the canary. The scheduler already contains pending_reenqueue protections for task-stack lifetime; those invariants must be audited against EL1-preempted frames rather than bypassed.

A temporary diagnostic was added to stack_protector.c. The first build used an incorrect call-site offset (+0x228 rather than the verified +0x218), so those repeat runs did not capture the saved canary. The source filter has since been corrected to the symbol-relative +0x218 and now validates the caller FP against the current process kernel-stack bounds before reading the saved canary; a corrected AArch64 build and runtime test passed, but did not trigger the failure. The corrected five-run repeat is recorded in docs/report/logs/GFX-SSP-corrected-diagnostic-repeat-2026-11-03.log. Earlier three-run and five-run non-reproduction samples are in docs/report/logs/GFX-SSP-diagnostic-aarch64-repeat-2026-11-03.log and docs/report/logs/GFX-SSP-diagnostic-aarch64-extended-2026-11-03.log. None of these passing runs provides a saved-canary comparison because the fault did not recur.

## 5. Corrective work — GFX-LEGACY-TERMINAL-01

Do not “fix” this by removing irqsave globally, dropping compositor_lock around term_write() without an ownership protocol, or moving the compositor files. Those changes would trade the observed failure for deadlocks, use-after-free or torn window state.

Required implementation sequence:

1. **Close the evidence loop:** retain the full serial/QEMU logs and reproduce with the diagnostic canary snapshot or debugger. Compare the saved canary with the global guard; inspect the caller frame and exact stack bounds; correlate with CPU/IRQ and scheduler activity.
2. **Audit all terminal memory spans:** prove term.cols/rows match the allocated text/attribute grids after create/resize; prove all scroll, caret and glyph paths stay within the actual pixel allocation; validate active-font glyph metadata and bitmap spans with overflow-safe arithmetic.
3. **Define stable window/surface ownership:** introduce a pin/reference or generation protocol so a writer can retain the terminal state and backing buffer while the global window-table lock is released. Resize/destroy must wait for or revoke in-flight writers safely.
4. **Separate state serialization from rasterization:** protect terminal parser/grid state with an explicit per-terminal serialization mechanism; capture bounded render work/surface generation under short locks; rasterize outside the global compositor lock only while the backing surface is pinned. Keep IRQ-originated mutations deferred to a safe process/work context or protected by a strictly bounded non-rendering critical section.
5. **Preserve legacy ABI:** SYS_WINDOW_WRITE and current userland behavior remain compatible until the NEXS-NG broker/service passes its migration gates.
6. **Add regression tests:** concurrent writers; long ANSI output and line wrapping; focus changes during writes; font replacement; resize/destroy while a writer is in flight; simultaneous compositor tick/present; SMP=1 and SMP=4. Assert canaries, surface bounds, terminal grid bounds, no faults/deadlocks and correct output.
7. **Verify both architectures:** clean rebuild, boot and full test on AMD64 and AArch64, then repeated soak/stress. Preserve complete serial, QEMU stdout/stderr, manifest and before/after screenshots.

## 6. Phase integration and exit criteria

This is a legacy correctness prerequisite / GFX-NG-0 preflight, not a new replacement architecture. It must close before GFX-NG-1 relies on buffer/surface lifetime semantics and before the legacy rendering path is reduced by the v1 compatibility broker.

Exit criteria:

- stack-canary mismatch is either reproduced with a proven writer and fixed, or a documented alternate root cause is established;
- actual allocation bounds and window lifetime are protected independently of the global compositor scene lock;
- terminal rasterization no longer runs under the global window-table lock with IRQs masked for an unbounded per-character/per-pixel duration;
- concurrency tests and full clean AMD64/AArch64 runtime gates pass with zero kernel faults;
- PENDING-WORK.md, the graphics migration plan and the diagnostic report agree on status and remaining work.

Until these criteria are met, classify the SSP incident as open / intermittent, not fixed. Keep the legacy implementation intact as the compatibility reference.


## Separate incident: input-time SSP / null instruction abort — 2026-11-04

A second AArch64 SSP incident was captured during NXShell character entry. The screen proves the typed cat /reg/system.version command was incomplete and never executed, so this is not evidence of a VFS read fault. The SSP return address maps to sys_registry+0x3b4; the dump reports PID 32002 (idle CPU2 in the same run's process table), ELR/FAR zero and ESR 0x86000004. This is separate from the earlier compositor_window_write() incident. It broadens the investigation to kernel stack/context lifetime, EL1 timer preemption and input dispatch; it does not establish registry corruption. Full evidence and instrumentation gates: docs/report/GFX-INPUT-SSP-FAULT-2026-11-04.md.
