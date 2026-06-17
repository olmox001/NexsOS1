# TIMER-MODEL — the three-tier time subsystem (NEXS)

> Status: **design + in-progress implementation** (branch `comprehensive-review`).
> Supersedes the ad-hoc notes in MANUAL §6 and PIANO-DRIVER-MATURITY about the
> per-CPU software timers landed in `00a83e6`. This document is the single
> reference for *how time works* in the kernel and *what userland sees*.

## 0. Why

After `00a83e6` the kernel had per-CPU software-timer wheels and a real blocking
`nanosleep`, but the **time base itself was naive**: `jiffies` was just `++`'d by
CPU 0 on every tick. If a timer IRQ was late or dropped (IRQ latency, a long
critical section, a focused CPU-bound task), `jiffies` silently fell behind the
wall clock and *never recovered* — sleeps drifted long, `get_time()` lagged real
time, and there was no agreement between cores. On amd64 it was worse:
`timer_get_us()` was a stub (`jiffies*1000`, ARCH-03) and the LAPIC timer had no
drift correction at all.

The requirement: **three clocks, each compared against real hardware time so lost
time is recovered**, in the seL4 spirit (kernel exposes the minimal mechanism;
userland builds policy).

## 1. The real-time reference (HAL primitive)

The one thing the kernel trusts is a **free-running hardware counter** and its
frequency. Both are already ASTRA HAL primitives (`kernel/include/kernel/arch.h`):

| | aarch64 | amd64 |
|---|---|---|
| `arch_timer_get_count()` | `CNTVCT_EL0` (virtual counter) | `RDTSC` |
| `arch_timer_get_freq()`  | `CNTFRQ_EL0` | **calibrated TSC Hz** (was a hardcoded `1e9` stub — fixed) |

From these the core derives one arch-neutral function:

```c
/* kernel/core/timer.c — monotonic nanoseconds since boot, the real-time base
 * every tier compares against. 128-bit mul/div so it never overflows. */
uint64_t mono_ns(void);   /* = (count - boot_count) * 1e9 / hw_freq */
```

`mono_ns()` is the **truth**. `jiffies` is a derived, cheap, integer view of it.

`mono_ns()` uses the natural `(__uint128_t)delta * NSEC_PER_SEC / freq` form.
The kernel and userland both link `-nostdlib` with no libgcc, so the `__int128`
runtime helpers the compiler emits (`__udivti3`/`__umodti3`/`__multi3`) are
provided in the shared math library `kernel/lib/math.c` (compiled into the
kernel and into `lib.o`). This makes `__uint128_t` a first-class type across the
codebase. (Use the predefined `__uint128_t`/`__int128_t` typedefs, not the bare
`__int128` keyword, which trips `-Wpedantic -Werror`.)

### amd64 TSC calibration

`arch_impl_timer_get_freq()` must return the *measured* TSC frequency, not a
constant. Calibration reuses the existing PIT path in
`kernel/arch/amd64/cpu/apic.c` (NOT `platform.c`, which is frozen until B4):
sample `RDTSC` across a known PIT interval (the same ~10 ms window
`lapic_timer_calibrate()` already busy-polls) and publish `tsc_hz` into a global
that `arch_impl_timer_get_freq()` reads. Done once on the BSP before SMP; APs
share the value (constant-rate / invariant TSC assumed on the QEMU targets —
gate behind CPUID `0x80000007` EDX[8] when present, else trust the calibration).

## 2. Tier 1 — the global clock (kernel, CPU 0 owns it, synced to real time)

`jiffies` stays the global monotonic tick counter, but CPU 0 no longer blindly
increments it. On every tick CPU 0 **reconciles** it against `mono_ns()`:

```c
/* CPU 0 only, in kernel_timer_tick(): */
uint64_t expected = mono_ns() / NS_PER_TICK;   /* NS_PER_TICK = 1e9 / HZ */
if (expected > jiffies)
    jiffies = expected;                        /* recover dropped ticks, monotonic */
```

* **Tied to core 0**: only CPU 0 writes `jiffies`, so there is no SMP write race
  (other cores read it). This is the *synchrony mechanism*: one authoritative
  clock, advanced to match real elapsed time.
* **Comparison + recovery**: if three ticks were lost, `expected` jumps ahead and
  `jiffies` catches up in one step instead of limping one-per-tick. It never goes
  backwards (`expected > jiffies` guard).
* A normal tick advances `expected` by exactly 1, so steady state is unchanged.

## 3. Tier 2 — the per-CPU clock (kernel)

Each core keeps its own tick cadence and **corrects its own drift against the
hardware counter**, so a core that was starved still programs its next interrupt
relative to *real* time, not relative to when it happened to run.

The accounting is **one arch-neutral, HAL-driven routine** —
`timer_percpu_tick()` in `kernel/core/timer.c` — so the logic is not duplicated
per arch. Expressed entirely in HAL counter units (`arch_timer_get_freq()` /
`arch_timer_get_count()`), it advances `cpu->next_tick_target` by the per-tick
interval with `tick_error_acc` fractional correction and a catch-up clamp
(`next_tick_target <= now → now + interval`), then reprograms the compare via
`arch_timer_set_compare()`:

* **aarch64** (one-shot CNTV timer): `arch_timer_set_compare()` writes
  `CNTV_CVAL_EL0`, so each tick rearms the next interrupt. `timer_handler()`
  (drivers/timer/timer.c) is now just `timer_percpu_tick(); kernel_timer_tick();`.
* **amd64** (periodic LAPIC): `arch_timer_set_compare()` is a **no-op** (the LAPIC
  auto-reloads), so the same routine maintains the per-CPU *software* schedule
  against the TSC without touching the hardware. The vector-32 ISR
  (`kernel/arch/amd64/cpu/idt.c`) calls `timer_percpu_tick()` before the tick.

Both arches seed the schedule on per-CPU bring-up with `timer_percpu_arm()`. The
shared fields live in `struct cpu_info` (`next_tick_target`, `tick_error_acc`,
`tick_count`). This is the ASTRA shape: the arch exposes only the HAL timer
primitives; the per-CPU clock logic is core.

The per-CPU software **timer wheel** (`cpu->timer_list` + `cpu->timer_lock`,
landed in `00a83e6`) is unchanged: each core fires its own timers against the now
real-time-synced global `jiffies`.

## 4. Tier 3 — the per-process clock (userland)

Two notions, both on the `mono_ns()` base:

1. **Sleep / timeout deadlines are absolute real time.** `nanosleep` arms the
   per-process `sleep_timer` (already per-CPU, woken locally), but the deadline is
   stored as **`wake_ns` (absolute `mono_ns`)**, not raw `wake_jiffies`. The wheel
   still fires on a jiffies edge, but the wake condition is `mono_ns() >= wake_ns`,
   so the process wakes at the right *wall-clock* instant even if ticks were lost
   (recovers lost time at the per-process level too). The jiffies-based arming is
   just the coarse trigger; `mono_ns()` is the fine check on re-entry.
2. **Per-process CPU time** (`struct process.cpu_time`, reported in `ps_info`) is
   accumulated in real ns from `mono_ns()` deltas across context switches, so
   `get_procs()` reports true CPU consumption rather than a tick count.

### Userland API (os1.h is the base; POSIX built on top)

os1.h is NEXS's **proprietary base API** (POSIX-like + graphics + primitives);
the real POSIX/libc layer is built *on* it. Time primitives added to os1.h:

```c
/* os1.h — proprietary base */
uint64_t os1_mono_ns(void);    /* monotonic ns since boot (SYS_CLOCK_GETTIME) */
uint64_t os1_cpu_ns(void);     /* this process's consumed CPU time, ns */
/* existing: get_time() ms, sleep()/usleep()/nanosleep blocking */
```

```c
/* <time.h> POSIX layer, on top of os1_mono_ns(): */
int clock_gettime(int clk, struct timespec *ts);  /* MONOTONIC, REALTIME→MONOTONIC for now */
```

New syscall: `SYS_CLOCK_GETTIME 258` returns ns directly (64-bit return register,
both arches), `arg0` selects the clock id (0 = monotonic, 1 = per-process CPU).

### Not in scope (filed separately)

Capability-based **timer objects** (arm-and-notify, timer-fd, multiple concurrent
timeouts) are the next seL4 step but need the real handle table — see
**issue #135 (TIMER-CAP-01)**.

## 5. ASTRA / layering compliance

* Arch provides only the four HAL timer primitives (`arch_timer_get_count` /
  `_get_freq` / `_set_compare` / `_control`) and the periodic tick IRQ. ALL clock
  *logic* — `mono_ns`, the Tier-1 reconciliation, and the Tier-2 per-CPU
  accounting (`timer_percpu_tick`/`timer_percpu_arm`) — is arch-neutral in
  `kernel/core/timer.c`. The per-arch difference collapses to the
  `arch_timer_set_compare` implementation (writes CNTV_CVAL on aarch64, a no-op
  on the periodic amd64 LAPIC). The arch timer IRQ handlers
  (`drivers/timer/timer.c`, `arch/amd64/cpu/idt.c`) are thin: call
  `timer_percpu_tick()`, then `kernel_timer_tick()`.
* No new code in `kernel/arch/amd64/platform/platform.c` (frozen until B4). The
  amd64 provider work lives in `apic.c` (TSC calibration → `tsc_hz`) and `arch.h`
  (`arch_impl_timer_get_freq` returns `tsc_hz`); the drift logic is shared core,
  not arch. The legacy `timer_get_us()` stub in `platform.c` is left dead and
  removed with the rest of that file in B4; nothing real calls it.
* `__uint128_t` runtime helpers live in the shared `kernel/lib/math.c` (kernel +
  userland), keeping `mono_ns()` in its natural 128-bit form without libgcc.

## 6. Verification (both arches mandatory)

```
make all ARCH=aarch64    # needs build/aarch64/virt.dtb once
make all ARCH=amd64
# headless boot per docs/PHASE-B-PLAN.md; health:
grep -cE 'PANIC|NESTED|Unhandled interrupt' /tmp/nexs.log   # want 0
```

Functional checks:
* `get_time()` advances at real-time rate on **both** arches (amd64 no longer
  stubbed): ~1000 over ~1 s.
* A focused CPU-bound task that previously made `jiffies` lag: after the rework,
  `get_time()` still tracks wall clock (reconciliation recovers the stolen ticks).
* `sleep(1000)` wakes after ~1 real second under load, not long.
* `notify_srv` and other services idle at ~0% CPU (blocking, USR-NOTIFY-02 #134).
