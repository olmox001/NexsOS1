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
PIT provider. The LAPIC is the per-CPU clockevent. Absolute programming is
normalized to the TSC count domain: when CPUID.01H:ECX[24] is present the
provider programs IA32_TSC_DEADLINE directly; otherwise it converts the absolute
TSC target to a calibrated LAPIC one-shot countdown. The periodic LAPIC path is
retained as an explicit fallback provider operation.

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

Clockevent mechanism and scheduler policy are now deliberately separate.

On AArch64 the architected virtual timer is an absolute one-shot event source.
NX-CORE programs the next event to the earliest of:

- the owning CPU's cached software-timer deadline;
- the currently running non-idle process quantum deadline;
- the provider's bounded idle target when neither exists.

An idle AArch64 CPU therefore receives no periodic 1 kHz scheduler interrupt.
When new runnable work is published to an idle CPU, the scheduler sends a
targeted reschedule SGI; the IRQ enters `nxhal_core_schedule(frame)` without
incrementing timer accounting or firing software timers.

On amd64 the provider also exposes an absolute one-shot contract in the TSC
count domain. It selects TSC-deadline when CPUID advertises the feature and
otherwise translates each absolute TSC target to the calibrated 32-bit LAPIC
one-shot countdown. The previously qualified 1 kHz periodic LAPIC path remains
available as fallback, but it is no longer the standard clockevent mode.

The local QEMU TCG backend explicitly rejects CPUID tsc-deadline even when
requested, so the default amd64 qualification exercises the LAPIC one-shot
countdown path. The TSC-deadline MSR path is compiled and reference-checked but
is not claimed runtime-qualified on this host; see §12.12.

The scheduler quantum is independent of either clockevent mode:

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

The tickless conversion also removed two accidental dependencies on the old
periodic tick. First, idle-to-runnable transitions now have an explicit
architecture-neutral reschedule event implemented by the IRQ-controller HAL
(targeted SGI on AArch64, reserved LAPIC IPI vector on amd64). Second, a process
with no CPU affinity is assigned through the scheduler's bounded round-robin
online-CPU policy instead of silently defaulting to CPU0 and relying on periodic
work stealing to rebalance it.

This keeps timer frequency, CPU-idle policy and task fairness as separate,
testable contracts.

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

The current 2026-11-10 time/scheduler checkpoint has passed:

- NX-HAL host tests and the integrated ASan/UBSan suite;
- generated TIME-contract checks plus independent amd64/AArch64 Clang layout
  probes;
- warning-as-error NX-CORE builds on amd64 and AArch64;
- amd64 GRUB-BIOS runtime with calibrated LAPIC one-shot countdown: shell
  interaction/history, timer diagnostics, post-timer shell input, counter
  heartbeat and post-heartbeat shell input all PASS; 28/28 KTEST; zero detected
  faults;
- AArch64 GRUB/UEFI runtime with the absolute one-shot clockevent: the same
  interaction/liveness gates PASS; 29/29 KTEST; zero detected faults;
- capability/object regression `captest`: 66/66 PASS on both architectures,
  including named-port late-sender wake and TRANSFER-right enforcement.

The AArch64 qualification is specifically significant because the first
one-shot attempts exposed real scheduler assumptions that the old periodic tick
had masked. Those failures were corrected and rerun rather than waived; §12.11
records the sequence and evidence.

Earlier direct-kernel lanes were independently qualified before this tickless
checkpoint. They are not being silently relabelled as requalified by the image
runs above; a future release gate must rerun the direct profiles after the
clockevent work is finalized.

The amd64 image lane remains GRUB-BIOS. amd64 UEFI is a separate boot
requirement and must not be conflated with the validated BIOS lane.

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

## 12. 2026-11-10 timer-queue security/performance study

The timer container is not selected by convenience. The 2026-11-10 review
rejected a premature binary-heap rewrite because it also narrowed the generic
timer abstraction into a process-only wakeup mechanism. That was a semantic
downgrade and has been removed. The current intrusive-list implementation is
the measurement baseline.

### 12.1 Workload and bounds visible in the current source

- NX-CORE currently embeds one software sleep timer in each process.
- MAX_PROCESSES is 128, so the current process-sleep workload has a global
  upper bound of 128 simultaneously armed process timers, before future timer
  object/service expansion.
- each CPU owns its timer queue and timer_lock;
- arm is O(1) list insertion and updates a cached earliest deadline;
- cancellation is O(1) for a non-earliest timer; cancelling the cached earliest
  performs an O(n) bounded recomputation of the next deadline;
- a clockevent arriving before the cached earliest deadline does not traverse
  the timer list; when timers are actually due, expiry scans the bounded local
  list to fire due callbacks and recompute the next earliest deadline;
- scheduler quantum and software-timer deadlines drive one-shot clockevents on
  AArch64 and amd64; the 1 kHz periodic LAPIC remains only a fallback mode;
- callbacks execute while timer_lock is held with local IRQ state protected.

The last point is the primary safety/latency defect. Container replacement is
not required to fix it and must not be used as an excuse to change timer
semantics.

### 12.2 Candidate comparison

| Candidate | Arm | Cancel | Earliest | Expire due | Runtime allocation | High-resolution suitability | Main risk |
| --- | --- | --- | --- | --- | --- | --- | --- |
| current cached unsorted intrusive list | O(1) | O(1), O(n) only when cancelling cached earliest | O(1) cached | O(n) only when due | none | good at current bounded depth | due-event scan cost |
| expiry-ordered intrusive list | O(n) | O(1) | O(1) | O(k) prefix | none | good at bounded low/moderate n | insertion cost |
| cached intrusive RB timerqueue | O(log n) | O(log n) | O(1) | O(k log n) removal | none | excellent; Linux hrtimer model | code/rotation proof complexity |
| indexed binary heap | O(log n) | O(log n) | O(1) | O(k log n) | fixed per-CPU array | excellent | index maintenance and migration races |
| hierarchical timing wheel | near O(1) bucket ops | near O(1) | bucket dependent | efficient batches | fixed buckets | not sufficient alone for maximum-resolution deadlines | quantization/complexity |

A timing wheel is therefore not a replacement for the high-resolution queue.
If ever introduced, it is a separate coarse/long-deadline tier, as in systems
that also retain a high-resolution timer path.

For the current <=128 process-sleep workload, an expiry-ordered intrusive list
is a serious candidate because it preserves O(1) cancellation and pointer-free
allocation behavior while making earliest-deadline and due-prefix processing
cheap. A cached RB timerqueue becomes preferable if runtime measurements show
large queue depths or high arm rates. A heap remains a valid data-structure
candidate, but only if it preserves callback/data semantics and wins measured
work; it is not the default.

### 12.3 Expiry/cancellation safety state machine

Moving callbacks outside timer_lock is a latency goal, but the current lock is
also a lifetime barrier. The 2026-11-05 TIMER-UAF-01 follow-up deliberately
keeps the owner CPU published for the whole callback and makes timer cancellation
take the same timer_lock, so a synchronous cancel cannot return while the
callback still uses its opaque data. Removing that lock coverage without an
equivalent quiescence protocol would reintroduce the already root-caused
use-after-free class on both architectures.

Simply unlinking and marking a timer idle before invoking its callback would
therefore be unsafe: a concurrent cancel/owner teardown could observe idle and
free callback data while the callback is still running.

The refined timer design therefore needs explicit states, conceptually:

- IDLE — not queued and not executing;
- QUEUED — owned by one CPU queue;
- FIRING — detached from the queue, callback execution in progress;
- CANCELLED/REARM transition represented by generation/state rather than a
  second legacy timer path.

Cancellation must distinguish queued from firing. A synchronous owner teardown
must not free callback/data storage until FIRING has completed. A nonblocking
try-cancel may report BUSY. Process teardown can use its existing deferred
reaping path rather than spin indefinitely. Timer callbacks may not hold the
queue lock, and callbacks that need sleepable work should be dispatched to a
bounded per-CPU timer-work context rather than executed as arbitrary work in
hard IRQ context.

This preserves generic callbacks and improves the existing semantics instead of
special-casing process sleep.

### 12.4 Clockevent implications

AArch64 exposes the architected absolute one-shot virtual timer through
`CNTV_CVAL_EL0`. amd64 now exposes the same Core-level absolute contract through
a provider-selected implementation:

1. TSC-deadline mode when CPUID.01H:ECX[24] exists, using `IA32_TSC_DEADLINE` and
   the required xAPIC ordering fence;
2. calibrated LAPIC one-shot countdown otherwise, translating the absolute TSC
   deadline to LAPIC ticks inside NX-HAL;
3. the existing periodic LAPIC operation retained as a qualified fallback.

The absolute clockevent ABI defines `frequency_hz`/min/max as the count domain
accepted by `program_absolute()`. That lets amd64 keep the TSC as the public
absolute domain while hiding LAPIC countdown conversion inside the provider,
rather than leaking architecture arithmetic into NX-CORE.

The Linux reference tree was used to verify the CPUID bit, LVT mode encoding,
`IA32_TSC_DEADLINE` MSR and xAPIC fence sequence. The local QEMU TCG backend does
not implement the requested TSC-deadline feature, so only the LAPIC one-shot
branch is runtime-qualified in the local automated lane.

### 12.5 Measurement before container cutover

Instrumentation must be per CPU and allocation-free. Record at minimum:

- current and high-water queue depth;
- arm, rearm, cancel and fire counts;
- timer nodes visited by expiry scans;
- due timers per IRQ;
- late-expiry count and maximum lateness;
- clockevent interrupts;
- queue-lock hold work/cycles using a sampled hardware-counter measurement so
  instrumentation does not dominate the path;
- callback hard-IRQ time separately from queue-lock time.

Measurements are required for idle, shell/services, timer stress and mixed
scheduler/compositor load on amd64 and aarch64. Only after those measurements
may the queue container change.

### 12.6 User monotonic fast path

Maximum-resolution user monotonic time is separate from the timer container.
The target is a read-only time-conversion page plus direct user counter read
when hardware permits it, with the object/gateway path as the semantic
fallback.

- amd64: RDTSC availability is not enough; invariant/global behavior and read
  ordering must be verified before publishing a fast path.
- AArch64: CNTVCT_EL0 access from EL0 requires CNTKCTL_EL1 configuration and
  verification on every CPU before the SDK may use it.

A timer daemon is therefore telemetry/control/service infrastructure, not a
replacement for the hot monotonic read path.

### 12.7 Verified baseline after rejecting the process-only heap rewrite

The generic intrusive-list timer remained the source baseline. The only timer
code added at this checkpoint is allocation-free per-CPU accounting for arm,
cancel, fire, scan work, queue depth/high-water and maximum scan/fire batch,
plus a KTEST that validates accounting deltas while retaining the existing
generic callback rearm/cancel test.

The test source was held at one verified hash while building. No
TIMER_HEAP/timer_heap/process_timer_wake/process-only timer symbol remained in
the Core, scheduler or tests.

A clean controlled amd64 run passed:

- lane: amd64/iso-bios;
- shell interaction and history: PASS;
- nxtimerdiag: PASS;
- shell after timer diagnostics: PASS;
- counter heartbeat: PASS;
- KTEST: 25 passed, 0 failed;
- runtime faults: 0.

The first controlled AArch64 attempt failed during build because a separate,
already-running make run process was concurrently writing build/aarch64. The
linker observed nxempire.o while that object was incomplete and reported
"file truncated". This is build-output corruption, not timer evidence. The
competing build process was stopped, the AArch64 output namespace was cleaned,
and the architecture was rebuilt in isolation from the same timer source.

The isolated clean AArch64 run then passed:

- lane: aarch64/iso-uefi;
- shell interaction and history: PASS;
- nxtimerdiag: PASS;
- shell after timer diagnostics: PASS;
- counter heartbeat: PASS;
- KTEST: 26 passed, 0 failed;
- both test_timer_stats_accounting and test_timer_rearm_cancel: PASS;
- runtime faults: 0.

The timer container therefore remains deliberately undecided beyond the current
intrusive-list baseline. No data-structure cutover is permitted until the new
accounting is exposed through the time capability/service and measured under
idle, timer-stress and mixed desktop workloads on both architectures.

### 12.8 TIME stats capability and measured queue baseline

The queue accounting is now exported without a new syscall. NX-CORE publishes
two privileged read-only objects under OS1_NS_TIME:

- `system` — the existing `os1_time_snapshot_v1`;
- `stats` — `os1_timer_stats_snapshot_v2`, containing per-CPU queue work.

Both kernel and SDK headers are generated from
`kernel/contracts/time-v1.json`. The contract gate regenerates/checks both
copies, validates the kernel snapshot hash and probes structure size/offsets
with Clang for amd64 and AArch64. The existing system snapshot stayed 3632
bytes. The timer stats CPU record is 64 bytes and the complete 64-CPU snapshot
is 4128 bytes.

The OS1nx_time service holds both privileged TIME capabilities. The diagnostic
client takes a before/after queue snapshot around its 160-sample sleep campaign
and reports deltas. This is observability only; it does not change queue
ordering, timer callback/data semantics, cancellation or owner-CPU behavior.

Verified runtime after the TIME stats integration:

- amd64/iso-bios: nxadb PASS, KTEST 26/26, timer diagnostics PASS, faults=0;
- aarch64/iso-uefi: nxadb PASS, KTEST 27/27, timer diagnostics PASS, faults=0;
- `test_timer_stats_export`, `test_timer_stats_accounting` and
  `test_timer_rearm_cancel` all pass on the AArch64 lane; the corresponding
  timer tests pass on amd64 as part of its 26-case suite.

Representative diagnostic campaigns observed the following active-CPU queue
work:

- amd64: queue high-water 6-7 timers; average nodes scanned per expiry pass
  approximately 2.57-2.65 in the captured campaign;
- AArch64: queue high-water 5-6 timers; average nodes scanned per expiry pass
  approximately 2.31-3.20 across two captured campaigns;
- an idle/non-owning CPU can execute timer passes with zero queue scans.

These results do not justify replacing the intrusive list with a binary heap.
At the measured queue depths a heap would add index maintenance, O(log n)
arm/cancel work and more migration/cancellation state without demonstrated
benefit. The intrusive list therefore remains authoritative.

This is not a claim that the current unsorted list is the final high-resolution
queue. The next measurement required before any container change is queue-lock
hold time and callback hard-IRQ time. Callback execution still occurs under
`timer_lock` because that lock currently supplies the TIMER-UAF-01 synchronous
quiescence guarantee. That guarantee must be replaced explicitly by a proven
FIRING/quiescence protocol before callbacks can move outside the lock.


### 12.9 Earliest-deadline cache: measured result and retained list

The first queue optimization deliberately did **not** replace the intrusive list.
Each CPU now caches the earliest queued software deadline in
`cpu_info.timer_next_deadline_ns`:

- arm remains O(1) and lowers the cached deadline when needed;
- a timer tick with no queued timer, or with `now < earliest`, performs no list
  traversal;
- a due tick scans the existing intrusive list, expires every due timer and
  computes the next earliest deadline in the same pass;
- cancellation remains O(1) unless the cancelled timer was the cached earliest;
  only that uncommon case rescans the remaining list to rebuild the cache;
- callback/data, owner CPU, synchronous cancel and the TIMER-UAF-01 lock-based
  quiescence semantics are unchanged.

The implementation also corrected an initialization-layer defect exposed by the
AArch64 tests.  The software timer lists and locks had been initialized from
`process_init()`, but KTEST runs before process initialization.  Timer state now
has its own `OS1libkernel_timer_core_init()`, invoked from the timer driver
bring-up after the validated clocksource is initialized.  `process_init()` no
longer owns timer-list initialization.

A first AArch64 boot after the cache change faulted in
`OS1libkernel_timer_cancel()` while a KTEST cancelled the final timer.  The
fault was reproduced to `timer_find_next_locked()` and was not waived.  The
lifecycle error above was corrected, the empty-list rescan was changed to walk
raw `list_head` nodes, and the architecture was rebuilt and rerun.  The
corrected AArch64 lane then passed all 27 KTEST cases, the timer diagnostic,
post-diagnostic shell interaction and fault gate with zero runtime faults.

Measured before/after queue work under the same 160-sleep diagnostic workload:

| Architecture | Before cache: active CPU avg nodes scanned/tick | After cache | Queue high-water |
| --- | --- | --- | --- |
| amd64/iso-bios | about 2.4–3.3 | about 0.31–0.63 | 5–8 |
| aarch64/iso-uefi | about 2.0–2.8 | about 0.34–0.66 | 5–6 |

Representative post-cache deltas:

- amd64 CPU0: 622 arms, 618 fires, 1470 node visits over 4733 passes
  (`avg_scan_x1000=310`);
- amd64 CPU2: 679 arms, 679 fires, 1797 visits over 4680 passes
  (`avg_scan_x1000=383`);
- AArch64 CPU0: 604 arms, 607 fires, 1452 visits over 3892 passes
  (`avg_scan_x1000=373`);
- AArch64 CPU2: 649 arms, 647 fires, 1571 visits over 3986 passes
  (`avg_scan_x1000=394`).

At the measured service/shell workload this removes roughly 70–90% of timer-list
node visits without paying heap/tree maintenance costs and without narrowing
the timer abstraction.  The intrusive list therefore remains the authoritative
container at this stage.

This result still does not prove that an unsorted list is optimal at high queue
depth.  The next data-structure gate is a concurrent sleeper/timer swarm that
drives materially larger per-CPU queue depth and measures arm, cancel, expiry
scan and lock work.  An ordered list, cached RB timerqueue, heap or wheel may
replace the current list only if that stress evidence demonstrates a net
benefit on both architectures.


### 12.10 Concurrent queue stress exposed and closed a scheduler lifetime defect

A bounded real-system queue stress was added to `nxtimerdiag` rather than
benchmarking a host-only container.  The test spawns 24 bounded sleeper
processes (below the kernel per-parent quota of 32), requires at least 16 to be
concurrently armed, samples TIME queue telemetry before/during/after, then
reaps every child.  The child count, observation windows and polling quantum
are recorded in the Power12 limit catalogue.

The first amd64 run reached an active depth of 32 and deliberately broke the
system: a user process was still current while its kernel stack had already
been returned to PMM and poisoned with `0xCC`.  The resulting kernel
instruction-fetch fault at RIP 0 was treated as a real scheduler defect, not a
timer benchmark failure.

The scheduler audit found three lifetime holes:

1. local picking rejected `PROC_DEAD` but could still select
   `PROC_ZOMBIE`/already-reaping entries;
2. cross-CPU work stealing accepted states other than `READY`, so a stale
   corpse/runqueue entry could be resurrected;
3. a stolen process disappeared from the source runqueue before it was
   published as destination `current_task`, leaving a migration handoff window
   invisible to termination/reap validation.

The fix makes scheduling authority explicit:

- only `PROC_READY && reaping == 0` can become runnable;
- a cross-CPU steal publishes `cpu_info.migration_inflight` before releasing
  the source CPU lock and changes ownership to the destination while both
  scheduler locks protect the handoff;
- publication as destination `current_task` clears the migration pin;
- termination scans current, outgoing-stack pin, migration pin and all
  runqueues instead of trusting `on_cpu`;
- the deferred reaper is bounded by `MAX_PROCESSES` and refuses to free a
  stack/PGD while any CPU/runqueue still references the process; unsafe entries
  remain deferred for a later safe point.

The same stress then passed on both architectures:

- amd64/iso-bios: 24 spawned, active depth 29, high-water 26, remaining 0,
  `STRESS DONE ... PASS`, 26/26 KTEST, zero faults;
- aarch64/iso-uefi: 24 spawned, active depth 30, high-water 23, remaining 0,
  `STRESS DONE ... PASS`, 27/27 KTEST, zero faults.

Representative high-depth post-cache queue work:

- amd64 CPU0: high-water 26, 327 arms / 330 fires, 5412 node visits over
  1386 timer passes (`avg_scan_x1000=3904`);
- AArch64 CPU0: high-water 23, 117 arms / 118 fires, 1254 visits over
  577 passes (`avg_scan_x1000=2173`).

This stress proves that the current list remains safe after the scheduler
lifetime fix and quantifies where it becomes more expensive. It did **not**
justify replacing the generic intrusive timer with a tree/heap. Deadline-driven
clockevents are now active on both architectures and the cached earliest
deadline eliminates list traversal on non-due events. Container replacement
therefore remains gated on a new high-depth comparison under the current
deadline-driven clockevent model, not on the old 1 kHz baseline.

### 12.11 Deadline-driven AArch64 clockevent and explicit reschedule events

The next optimization removed periodic no-work clockevent interrupts on the
provider that already had the required hardware contract. No timer-container
cutover was performed.

AArch64 now uses `CNTVCT_EL0`/`CNTV_CVAL_EL0` as a continuous clocksource plus
absolute per-CPU one-shot clockevent. NX-CORE programs the minimum of the cached
software deadline and current scheduler quantum. This section records the
scheduler dependencies exposed by that first tickless conversion; amd64 was
converted to the same Core-level absolute contract afterward (§12.12).

The first AArch64 one-shot boot did **not** reach `nxinit`: KTEST completed, PID1
was made runnable, and the BSP then remained idle because the old design had
implicitly relied on the next periodic tick to enter `schedule()`. The repair
added a semantic targeted-reschedule operation to the HAL IRQ-controller
contract:

- AArch64: SGI1 targeted at the logical CPU;
- amd64: reserved LAPIC vector `0xFC`, distinct from the existing TLB and panic
  vectors;
- Core IRQ dispatch calls `nxhal_core_schedule(frame)` for that event and does
  not account it as a timer tick.

The next run reached userland but exposed a second hidden dependency: first-time
task placement defaulted unassigned processes to CPU0 and periodic idle ticks on
other CPUs had previously provided opportunistic work stealing. With one-shot
idle those CPUs stayed asleep and the 33 ms sleep diagnostic regressed to about
75–81 ms average. Initial runnable placement now uses the scheduler's existing
bounded round-robin policy over **online** CPUs and sends a reschedule event when
work is published to an idle target CPU.

That balancing change exposed one final no-tick liveness race in the permanent
keyboard regression. A task woken while still referenced by
`pending_stack_release` was marked READY but intentionally not placed on the
runqueue until the next scheduler entry. The old code returned “no kick” in
that state. Under periodic scheduling this eventually healed; under tickless
idle the keyboard service could remain READY forever. The lifetime pin remains
unchanged, but `__enqueue_task()` now requests a reschedule event when that
deferred READY transition targets an idle/null CPU. The next scheduler entry
releases the old stack pin and performs the existing safe enqueue.

Timed-sleep callbacks also retain the proven lock order. Timer callbacks execute
under `timer_lock`; their wake path uses only the already-owned CPU runqueue lock
and never takes global `sched_lock`, avoiding a cycle against
stop/termination's `sched_lock -> timer_lock` order.

The final controlled AArch64 run passed shell input before and after the timer
campaign, counter heartbeat, KTEST 29/29 and zero fault detection. Representative
160-sleep telemetry was:

- `OS1_sleep`: min 33.428 ms, average 40.258 ms, max 80.816 ms;
- `nanosleep`: min 33.398 ms, average 37.736 ms, max 76.653 ms;
- no samples exceeded the 50 ms lateness band;
- timer-pass deltas during the campaign were CPU0=885, CPU1=214, CPU2=149,
  CPU3=446, rather than roughly one pass per millisecond on every CPU;
- CPU2 had no queued software timers during the interval but still observed
  scheduler-quantum events, demonstrating that timer-queue scans and scheduler
  deadlines are now separately attributable.

The same scheduler/IRQ changes first passed the amd64 periodic fallback lane
with 28/28 KTEST and zero faults. `captest` subsequently passed 66/66 on both
architectures, including the late-sender named-port wake that directly exercises
an idle blocked task. The later amd64 one-shot conversion is recorded separately
in §12.12.

This evidence supports retaining the intrusive timer list while clockevent
overhead is removed first. Queue-container replacement remains gated on
post-clockevent measurements.

### 12.12 amd64 absolute clockevent: LAPIC one-shot and TSC-deadline selection

The amd64 provider now implements the same absolute-deadline contract as
AArch64 without exposing LAPIC-specific arithmetic to NX-CORE.

The selection order is:

1. if CPUID.01H:ECX[24] is set, configure LAPIC LVT timer mode 2, issue the
   xAPIC ordering fence and program `IA32_TSC_DEADLINE` directly;
2. otherwise configure the LAPIC timer in one-shot countdown mode and translate
   the absolute TSC target into calibrated LAPIC ticks;
3. retain the periodic LAPIC programming operation as a fallback path.

The countdown conversion uses amd64 `mulq`/`divq` to compute the bounded
64x64/64 scale directly in the provider. It does not import libgcc/compiler-rt
into NX-HAL. The provider publishes min/max absolute deltas in TSC counts based
on the one-tick minimum and 32-bit LAPIC initial-count maximum, so Core clamps
targets before programming hardware.

The standard QEMU TCG lane does not expose TSC-deadline. An explicit
`max,+tsc-deadline` qualification attempt produced QEMU's warning that TCG does
not support CPUID.01H:ECX.tsc-deadline[24]; the guest therefore correctly stayed
off the MSR path. `NEXS_QEMU_AMD64_CPU` was added to the QEMU launcher so this
feature can be qualified on a backend/hardware combination that actually
exposes it without changing the default CPU model.

The calibrated LAPIC one-shot path is runtime-qualified. The amd64 GRUB-BIOS
suite passed shell interaction/history, timer diagnostics, post-timer input,
counter heartbeat, post-heartbeat input, KTEST 28/28 and zero detected faults.
`captest` also passed 66/66 after the conversion.

For the same 160-sleep diagnostic shape, the earlier periodic baseline recorded
about 14,977 timer passes across four CPUs. The one-shot run recorded 1,992
passes (CPU0=705, CPU1=615, CPU2=176, CPU3=496), an approximately 86.7% reduction
in clockevent/timer-pass work. Node visits fell from about 32,696 to 5,218.
Representative latency remained close to the old baseline:

- `OS1_sleep`: min 33.141 ms, average 34.485 ms, max 42.486 ms, no >50 ms late
  samples;
- `nanosleep`: min 33.171 ms, average 34.942 ms, max 114.106 ms, with one >50 ms
  outlier;
- monotonic/get_time comparison reported zero regressions and zero mismatches;
- the diagnostic completed with zero failures.

The single POSIX outlier is retained as evidence rather than hidden. It is not a
reason to restore periodic interrupts; future latency work should attribute
service wakeups and scheduler contention using the same per-operation telemetry.

The latest shared-Core idle-target change was rerun on AArch64 after the amd64
provider work: the GRUB/UEFI lane again passed timer diagnostics, both post-load
shell-input gates, counter heartbeat, KTEST 29/29 and zero detected faults.


### 12.11 Deadline-driven clockevent qualification and fire-lateness telemetry

The deadline-driven clockevent path is now qualified functionally but is not yet
accepted as the final latency target.  Timer queue telemetry was extended to
separate hardware/Core expiry lateness from scheduler wake latency instead of
inferring one from end-to-end sleep duration.

The TIME statistics contract now uses `OS1_TIMER_STATS_ABI_VERSION = 2`.
There is no compatibility alias for the old timer-stat record.  The current
wire records are `os1_timer_cpu_stats_v2` and
`os1_timer_stats_snapshot_v2`, generated from the single TIME contract
specification and layout-checked with Clang for amd64 and AArch64.  Per CPU,
the new counters record:

- number of fires that occurred after their absolute software deadline;
- accumulated fire lateness in nanoseconds with saturating accounting;
- maximum observed fire lateness in nanoseconds.

The TIME service protocol moved to version 2 with the same cutover; the general
`os1_time_snapshot_v1` remains version 1 because its layout did not change.

The first amd64 qualification remains on the real periodic LAPIC provider.  The
provider advertises only `PER_CPU | PERIODIC`, absolute programming remains
unsupported, and Core therefore does not activate deadline mode.  A controlled
nxadb run passed shell interaction/history, timer diagnostics, post-diagnostic
shell interaction, heartbeat, 28/28 KTEST and the zero-fault gate.  Under the
33 ms diagnostic workload, active CPUs observed approximately 0.97–1.15 ms
average software-timer fire lateness.  That is consistent with the 1 kHz
periodic fallback and provides a useful control measurement.

AArch64 uses the architected virtual timer as an actual absolute one-shot
clockevent.  Comparison with the Linux ARM architected-timer implementation
identified a missing architectural ordering requirement: writes to
`CNTV_CTL_EL0` must be followed by an ISB before subsequent execution may rely
on the new enable/mask state.  The HAL wrapper now performs that ISB; no Core
policy or timer-container semantics were changed.

After the ordering correction, an isolated AArch64 nxadb qualification passed
shell interaction/history, timer diagnostics, post-diagnostic shell
interaction, heartbeat, 29/29 KTEST and the zero-fault gate.  The correction
reduced early maximum fire-lateness observations from roughly 94–111 ms to
roughly 48–54 ms.  The full diagnostic campaign still measured non-acceptable
lateness on some active CPUs: representative deltas were approximately
8.97 ms average / 54.7 ms maximum on CPU0, 8.09 ms average / 64.0 ms maximum on
CPU1, and 1.60 ms average / 69.5 ms maximum on CPU3.  End-to-end 33 ms sleep
samples averaged about 44.7 ms for OS1_sleep and 37.5 ms for nanosleep in that
campaign.

Therefore the one-shot path is **functionally verified but latency refinement
remains open**.  The evidence specifically shows that the clockevent/Core fire
path itself can be late; this is not only scheduler wake latency.  The next
timer step must investigate compare reprogramming, interrupt delivery and
per-CPU deadline publication before changing wake-preemption policy or replacing
the intrusive timer list.

The freestanding integer runtime was also completed during this qualification.
amd64 selected the compiler ABI helper `__udivmodti4` for the checked
nanosecond-to-counter conversion.  The existing self-contained 128-bit runtime
now implements that helper through the same bounded long-division core used by
`__udivti3`/`__umodti3`.  Two runtime KTESTs force non-constant 128-bit
division/modulo, including the timer's 1e9 divisor and a true 128-bit divisor;
both pass on amd64 and AArch64.  No libgcc/compiler-rt dependency was added.


### 12.12 Clockevent delivery telemetry (TIME stats ABI v3)

Timer statistics ABI v3 adds compare-to-IRQ delivery telemetry and absolute
program counts.  A first implementation sampled `next_tick_target` from
`OS1libkernel_timer_percpu_tick()`, after `nxhal_core_timer_tick()`.  That
was rejected after review because the Core tick can schedule and reprogram the
next clockevent before the sample is taken.  Those first event-lateness numbers
are invalid evidence and are intentionally discarded.

The corrected implementation samples the free-running counter and the target
that caused the interrupt at entry to `OS1libkernel_timer_tick()`, before
software expiry callbacks or scheduler work.  `late_tick_count` now uses that
same entry observation.  Rearming remains after Core timer/scheduler handling.

The corrected cross-architecture runtime qualification passed:

- amd64/iso-bios: shell/history, timerdiag, post-timer shell and heartbeat PASS,
  28/28 KTEST, zero faults;
- AArch64/iso-uefi: the same gates PASS, 29/29 KTEST, zero faults.

Representative corrected amd64 periodic-fallback campaigns measured active-CPU
clockevent lateness around 1.0–2.5 ms average and software-fire lateness around
1.1–1.7 ms average.  A CPU with no software timers can show larger scheduling
jitter in the QEMU lane, but the 33 ms sleep campaign remained approximately
34.8–35.8 ms average with no 50 ms-late samples in the captured runs.

The corrected AArch64 SMP=4 TCG campaign showed that the larger lateness exists
before the timer queue scan: representative compare-to-IRQ averages were about
6.8 ms, 8.3 ms, 18.9 ms and 12.6 ms across the four CPUs; corresponding
software-fire averages on CPUs carrying timers were about 7.5 ms, 10.4 ms and
12.6 ms.  The 33 ms campaign averaged about 49.0 ms for OS1_sleep and 62.0 ms
for nanosleep in that captured run.

The host is x86_64 Intel and `qemu-system-aarch64 -accel help` exposes only
TCG, so this lane is not physical-AArch64 latency evidence.  A controlled
`NEXS_QEMU_SMP=1` run did not remove the problem: compare-to-IRQ averages
remained roughly 6.8–9.7 ms and concentrating every service on one vCPU made
end-to-end sleeps substantially worse.  Therefore multi-vCPU TCG contention
alone is not a sufficient explanation.

The QEMU `virt` DTB was also inspected directly.  Its architected virtual
timer describes PPI 11 / INTID 27 as level-triggered.  An experiment that
explicitly rewrote the banked GICv2 ICFGR1 field to level mode compiled but did
not qualify: nxadb subsequently failed the heartbeat/post-heartbeat gate.  That
experiment was fully reverted and was never staged.  No performance claim is
made from it.

The remaining ambiguity is now narrower: compare expiration can be observed
late either because the QEMU/TCG virtual timer is delivered late, or because the
guest keeps IRQs masked across long critical sections.  The next required
measurement is therefore bounded per-CPU IRQ-off duration telemetry.  Timer
container replacement, artificial early deadlines and scheduler compensation
remain prohibited until that boundary is measured.


## 13. Stable service-facing timer statistics (2026-11-10)

TIME statistics transport is owned by NX-CORE and the privileged
`/sys/services/nxtimerd` broker. The kernel object exports the generated
`os1_timer_stats_snapshot_v4` ABI, and the broker validates its version,
complete wire size, per-CPU record size and CPU count before publishing
any response. The wire type and version are never used by ordinary programs.

The SDK's `OS1low_time_stats_snapshot(handle, buffer, size)` reads a
caller-sized privileged TIME object and does not embed the wire type in
`system/include/api/os1.h` or `system/lib/lib.c`. Only nxtimerd calls
this low-level method for the timer-statistics transport.

The supervised broker converts each wire CPU record field-by-field into
`struct os1_time_service_timer_cpu` and publishes the stable
`struct os1_time_service_timer_stats`. The SDK client validates the bounded
CPU count, while ordinary programs (including `nxtimerdiag`) consume the
stable model through `OS1_time_service_timer_stats`. No version-suffixed
TIME statistics structure appears in the public SDK service API or in the
diagnostic program. A future kernel wire revision updates the broker's
decoder, not every consumer's type name.

The 2026-11-10 freshly cleaned AMD64/ISO and AMD64/direct boots each
passed 28/28 KTEST and timerdiag, with no unexpected faults. The same
AArch64/UEFI and AArch64/direct matrix passed 29/29 KTEST and timerdiag.
This verifies the real service route for the current wire ABI; future wire
changes still require exact ABI, negative-size, permissions and guest
interoperability regression tests.
