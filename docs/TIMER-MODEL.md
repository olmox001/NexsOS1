# TIMER-MODEL — NX-HAL time, deadlines and scheduler cadence

Status: implemented and runtime-qualified on amd64 and aarch64. This document
describes the current source; older jiffies/HZ=100 notes are historical.

## 1. Ownership

Time is split into three layers with one dependency direction:

```text
architecture provider -> NX-HAL time ABI -> NX-CORE time policy -> OS1 user ABI
```

Architecture code owns hardware discovery/programming only. NX-CORE owns elapsed
time, deadlines, software timers and scheduler policy. Userland never reads an
architecture counter directly.

Early entropy is not part of timekeeping. `hal/entropy.h` exposes an
architected hardware-random sample and a changing early jitter sample. The
jitter value has no frequency or wall-time semantics and exists so SSP entropy
can initialize before the runtime clocksource is published.

## 2. Stable NX-HAL time ABI

The public contract is `kernel/HAL/include/hal/time.h`, ABI version 1.

A clocksource descriptor reports:

- source identity;
- capability flags (continuous/calibrated/invariant/global/verify);
- counter width and mask;
- measured/architected frequency;
- maximum safe delta.

A clockevent descriptor reports:

- event identity and IRQ;
- per-CPU/periodic/oneshot/absolute capabilities;
- hardware frequency;
- minimum and maximum programmable deltas.

Public operations are:

- `nxhal_time_clocksource_info`;
- `nxhal_time_clockevent_info`;
- `nxhal_time_counter_read`;
- `nxhal_time_event_percpu_init`;
- `nxhal_time_event_program_absolute`;
- `nxhal_time_event_program_periodic`;
- `nxhal_time_event_stop`.

NX-CORE must not infer architecture from these values and must not call the
provider-private `arch_impl_timer_*` primitives.

## 3. Providers

### amd64

The runtime clocksource is the TSC. Its frequency is calibrated against the PIT
using bounded measurement windows and consistency checks in the existing APIC/
PIT provider. The LAPIC is the per-CPU clockevent. The provider declares its
actual periodic/absolute capabilities; Core does not pretend a periodic LAPIC is
a one-shot comparator.

AMD64 has no DTB requirement. The normal hardware-discovery path is PCI/ACPI/
firmware information. Absence of FDT is therefore valid and must not degrade
timekeeping or device enumeration.

### AArch64

The runtime clocksource is the architected virtual counter (`CNTVCT_EL0`) at
the architected `CNTFRQ_EL0` frequency. The virtual timer is the per-CPU
clockevent and uses its architected comparator/control registers.

FDT/EFI handoff is a boot-discovery input, not part of the time ABI.

## 4. NX-CORE monotonic clock

`kernel/core/timer.c` initializes the HAL descriptors once, validates the
reported capabilities and records the boot counter epoch. The Core API exposes:

- `OS1libkernel_time_counter_read()`;
- `OS1libkernel_time_monotonic_ns()`;
- checked absolute-deadline construction;
- deadline comparison;
- counter-to-nanosecond conversion;
- versioned OS1 time snapshots.

Counter scaling uses checked wide arithmetic. A failed or unavailable runtime
clocksource after initialization is a kernel invariant failure, not silently
replaced with a fake frequency.

`jiffies` remains only a derived compatibility/instrumentation view:

```text
jiffies = monotonic_ns / OS1LIBKERNEL_NSEC_PER_TICK
```

Software-timer correctness does not depend on incrementing jiffies once per IRQ.

## 5. Clockevent cadence versus scheduler policy

The per-CPU clockevent currently runs at:

```text
OS1LIBKERNEL_TIMER_HZ = 1000 Hz
```

That 1 ms event cadence is mechanism, not the scheduler quantum.

The historical scheduler stored `DEFAULT_QUANTUM=3` ticks. After the clockevent
moved from 100 Hz to 1 kHz that silently changed the intended 30 ms policy into
3 ms. Worse, every timer IRQ called `schedule()` and the outgoing RUNNING task
was made READY even while its tick counter had not expired. That made scheduler
activity effectively track the 1 kHz interrupt cadence.

The current scheduler instead stores an absolute monotonic deadline:

```text
OS1LIBKERNEL_SCHED_QUANTUM_NS = 30 ms
process.quantum_deadline_ns
```

`OS1libkernel_sched_timer_tick(frame, now_ns)` returns the interrupted frame
unchanged while a normal running task remains inside its quantum. It invokes the
full scheduler only when:

- the quantum expires;
- the CPU has no current task;
- the current task is idle;
- the current task is no longer RUNNING.

Blocking IPC, sleep, exit, fault handling and explicit yield still call
`schedule()` immediately; they do not wait for quantum expiry.

This separates timer frequency from scheduling policy and keeps a future
clockevent-frequency change from silently changing task fairness.

## 6. Per-CPU timer accounting

Each CPU tracks its clockevent target/error accounting in `struct cpu_info`.
`OS1libkernel_timer_percpu_arm/tick` reconcile the software schedule against
the free-running clocksource and use checked provider programming.

Lost/late timer IRQs do not create a second time base. The next event is derived
from hardware time, and Core monotonic time continues to advance independently
of IRQ delivery.

## 7. Software timers and sleeps

`struct OS1libkernel_timer` carries an absolute monotonic nanosecond deadline.
Timers live on the owning CPU's bounded timer list and wake through Core-owned
callbacks. `nanosleep`/OS1 sleep therefore block the process and wake against a
real absolute deadline; they are not busy loops and are not specified as
"one HZ tick" delays.

SMP bring-up waits and VirtIO block completion deadlines also use the Core
monotonic deadline API instead of raw counter loops.

The panic watchdog is a deliberate exception: IRQs are disabled and normal Core
scheduling is unavailable, so it reads the already-published HAL clocksource
directly using a bounded polling budget before reboot.

## 8. User ABI and diagnostics

The OS1 time ABI exposes monotonic and process-CPU time. POSIX
`clock_gettime(CLOCK_MONOTONIC)`, `get_time()` and `nanosleep()` are
adapters over that same source, not independent clocks.

`/sys/bin/nxtimerdiag --probe` validates these surfaces by bracketing each
compatibility read between two OS1 snapshots. A sequential fixed-tolerance
comparison is forbidden because a normal preemption between two reads can look
like clock drift.

`/bin/counter` is a scheduler/liveness diagnostic. It emits approximately one
UART heartbeat per second:

```text
[COUNTER] frame=<...> mono_ms=<...>
```

The UART heartbeat is diagnostic output only. Graphical applications render
through the window/surface APIs; debug printing is not part of the graphical UI.

## 9. Current verification

The current time/scheduler snapshot has passed:

- standalone NX-HAL build and boundary audit on amd64 and aarch64;
- NX-HAL host tests, ASan/UBSan and static-analysis gates;
- clean NX-CORE builds on both ISAs;
- amd64 GRUB-BIOS image: interactive NXShell, timerdiag PASS, heartbeat PASS,
  shell-after-heartbeat PASS, 22/22 KTEST, zero detected faults;
- amd64 direct kernel + rootfs: same result, 22/22 KTEST;
- AArch64 GRUB/UEFI image: same result, 23/23 KTEST;
- AArch64 direct kernel + DTB + rootfs: same result, 23/23 KTEST;
- independent `make run` reaches NXShell in all four current Makefile lanes.
  The amd64 image lane is still GRUB-BIOS; amd64 UEFI is a separate open boot
  requirement and must not be conflated with the validated BIOS image lane.

The direct and image lanes are mutually exclusive per architecture in nxadb
because they share one writable disk image; nxadb stops the sibling lane before
starting another profile.

## 10. Open performance work

Removing the accidental 1 kHz forced preemption did not by itself remove the
observed desktop context-switch rate. An amd64 MEMSTAT sample after the fix
still measured roughly 1.06–1.12k context switches/s.

Source audit identifies independent wakeup sources that must be converted to
event-driven services rather than hidden by a larger scheduler quantum:

- keyboard service now blocks on the raw-input wait path for IRQ-driven
  providers; polling-only USB HID explicitly falls back with `-EAGAIN`;
- mouse service still polls every 5 ms because it also multiplexes control IPC
  and a generic wait-many ABI is not implemented yet;
- shell still polls every 10 ms;
- several 33/50 ms service loops remain;
- compositor frame allocation/free churn remains on every frame and per window.

These are separate defects. They must be measured and converted to blocking
input/IPC/object waits and persistent bounded buffers while retaining the 30 ms
scheduler policy.

## 11. Required next time/IPC work

1. Replace fake POSIX `poll()` (which currently ignores timeout/readiness) with
   one generic readiness/wait-many contract over kernel wait queues and
   monotonic deadlines. The contract must remain device-class neutral; object
   code must not learn keyboard/mouse policy.
2. Convert the remaining mouse/shell/USB-HID polling loops to blocking event
   delivery using that generic readiness model.
3. Keep IPC timeout semantics on the same absolute monotonic clock.
4. Add per-operation wakeup/context-switch instrumentation so service-level
   sources can be attributed without guessing.
5. Re-run idle -> load -> post-load MEMSTAT campaigns on both architectures.
