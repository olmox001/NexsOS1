# NEXS-DIR-05 — C-line backtrace, kernel recovery mode, on-screen panic

The in-kernel trace debugger already works well: the fault dump + fp-chain
backtrace (`kernel/lib/backtrace.c`, `kernel/lib/fault_print.c`, `kernel/include/
kernel/fault.h`) located TIMER-UAF-01 precisely (`kernel_timer_tick+0x130`,
`pic_chip_end+0x33`). The direction is to push it from "symbol+offset" toward a
real recoverable debugger for the architecture NEXS has.

## 1. Exact C source lines, not just symbol+offset
The ELFs are built with DWARF (`-g`). Resolve each backtrace frame to
`file:line` (addr2line-style: walk `.debug_line`) so a panic shows the incriminated
C statements directly, not just `func+0xNN`. Can be done in-kernel (parse the
embedded line program) or as a host-side post-processor keyed off the build's
`kernel.elf`.

## 2. Try to restore the kernel — recovery mode
Where the fault is attributable and contained (e.g. a recoverable per-CPU state, a
killable user process), prefer recovery over `panic()`:
* user-attributable faults already terminate the process and continue
  (`fault_handle_user_or_panic`) — extend the same "contain and continue" stance
  to more kernel-recoverable cases;
* a **recovery mode**: quiesce, reset the offending subsystem/core, and resume,
  rather than halting the whole machine. Pairs with **core-restart in the HAL**
  (DIR-02 §4): a core restart that preserves the framebuffer and graphics state.

## 3. On-screen panic (not only UART)
On a fatal fault, render a splash/panic screen to the framebuffer in addition to
the UART dump, so a user on the GUI (UTM / real hardware, no serial) sees the
fault, the RIP/ELR, and the top frames. Because the compositor/gpu live in the HAL
(DIR-02), the panic path can use a minimal HAL framebuffer blit that does not
depend on the (possibly faulted) scheduler or compositor service.

## Acceptance
* Backtrace frames print `file:line` for kernel addresses with DWARF present.
* A defined set of kernel faults enters recovery mode instead of halting.
* Fatal faults paint an on-screen panel via a HAL framebuffer primitive, in
  addition to the UART trace.

## Status (2026-07-02)

**Item 3 (on-screen panic) DONE; items 1-2 (DWARF file:line, recovery mode) NOT
STARTED.** The F0.0.4.2 stabilization detour (2026-06-27→28) shipped the
on-screen panic half of this direction plus a related crash→notification
bridge that this doc did not originally scope but belongs here:

* **On-screen red panic, UART-independent**: `panic_screen()`
  (`kernel/graphics/graphics.c:171-179`, `RED = 0xFFB91C1C`) blits directly to
  the primary GPU framebuffer, explicitly bypassing the compositor so a faulted
  compositor/scheduler can't block the panic paint — exactly the "minimal HAL
  framebuffer blit that does not depend on the (possibly faulted) scheduler or
  compositor service" this doc's §3 called for. Called from both the
  fault-context and normal panic branches in `kernel/lib/printk.c:279,304`.
* **Watchdog reboot** (not originally scoped by this doc, but the natural
  completion of "on fatal fault, do something visible and bounded"): reboots
  ~10s after a panic (`kernel/lib/printk.c:243-247`).
* **Userland crash → red notification**: a user-mode fault (not a kernel
  panic) now surfaces as a severity-flagged notification via
  `fault_notify_user()` (`kernel/core/fault.c:35-49`), distinct from the
  kernel-panic path above. This extends the existing "contain and continue"
  stance (§2) with a **visible** signal, not just silent process termination.
* **Still not started**: DWARF `.debug_line` resolution for `file:line`
  backtrace frames (verified: no `debug_line`/`addr2line` logic in
  `kernel/lib/backtrace.c`/`kernel/lib/fault_print.c`); a kernel recovery mode
  that quiesces/resets a subsystem instead of `panic()` (verified: no
  `recovery_mode`/`kernel_recover` symbol anywhere in `kernel/`). Both remain
  exactly as scoped in §1/§2 above.
