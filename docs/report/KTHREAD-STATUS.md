# KTHREAD-STATUS — the kernel-thread infrastructure is DISABLED (do not enable)

**Status:** staged scaffolding, **not used at runtime**, known unstable.
**Date:** 2026-07-12 (S-STAB stabilization pass).

## TL;DR

The `kthread_*` / `arch_cpu_yield` / input-server-thread code committed the week
of 2026-07-03..10 (commits `f0dd624`, `4b731b6`, `b6732b7`) is **kept only as a
reference skeleton**. It must **not** be wired up. Input runs safely from the
timer-tick bottom-half (`input_drain`, `kernel/core/timer.c`); the correct end
state for IRQ-decoupled input is a **supervised userland process** (the same
model as the notification service `nxntfy_srv` + `init`), **not** a kernel
thread.

The single entry point that would activate it, `input_server_start()`
(`kernel/drivers/keyboard/keyboard.c`), is **compiled out** unless
`NEXS_ENABLE_UNSTABLE_KTHREAD_INPUT` is defined. It is never called anywhere in
the tree.

## Why it is disabled

1. **Yield-to-USER stalls CPU0.** `kthread_block()` parks a kernel thread via
   `arch_cpu_yield()` (a hand-rolled synthetic `pt_regs` frame + `schedule()` +
   `restore_context`). Switching cooperatively TO a freshly-woken **user** task
   from that path stalls CPU0 (a cross-CPU EL0/ring3-return interaction the
   hand-rolled epilogue does not handle). A pure kernel→kernel switch works, but
   the input server ultimately has to wake user tasks.

2. **Outside the SMP idle-task ordering.** The SMP scheduler relies on a strict
   per-CPU invariant: each CPU's idle task exists before that CPU can run, and
   the pick order sandboxes tasks per core (see `smp_create_idle_task`,
   `kernel/core/smp.c`, and the #169/#170 fixes). A thread created through
   `kthread_create()` sits *outside* that ordering and can perturb the
   idle/pick invariants — a corruption risk on 4-core configs.

## What is live vs. dead

| Symbol | State | Notes |
|---|---|---|
| `restore_context` (amd64 `isr_stubs.S`, aarch64 `exception.S`) | **LIVE, correct** | The shared ISR/trap restore epilogue. Behavior-identical to the pre-4.3 inline restore (verified by diff). Keep it. |
| `arch_cpu_yield` | dead at runtime | Only reached from `kthread_block`. |
| `kthread_create` / `kthread_block` | dead at runtime | Only caller is the disabled `input_server_start`. |
| `input_thread_entry` / `input_server_start` | **disabled** (compiled out) | Reference skeleton for the future userland input service. |

> Note: removing this scaffolding is a **separate, optional** cleanup. It is dead
> code, so leaving it (documented + guarded) is safe; the maintainer chose to
> document rather than delete so the design intent (input-as-process) is not
> lost.

## Correct path forward (do this instead)

Reimplement IRQ-decoupled input as a **supervised userland process**, mirroring
`nxntfy_srv`:
- the kernel enqueues raw events (the existing `input_ring` already does this);
- a userland input service drains them over IPC/registry and dispatches;
- `init` owns/respawns it (same `register_service_pid` pattern).

This keeps input inside the normal process/scheduler model — no bespoke kernel
thread, no bespoke blocking primitive.
