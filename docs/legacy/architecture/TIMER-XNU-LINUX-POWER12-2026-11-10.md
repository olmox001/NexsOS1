# Timer engineering checkpoint: XNU, Linux and Power12

Date: 2026-11-10
Branch: DEV-Resurrection
Status: work in progress; project-wide Power12 and complete timer qualification remain open.

## Source authority and comparative references

NexsOS1 source and its current NX-HAL/NX-CORE contracts are authoritative.
XNU and Linux are design and verification references, not source replacements.

- Apple XNU timebase/calendar synchronization: https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/clock.c
- Apple XNU ARM64 CPU timer queue migration on CPU-down: https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/machine_routines.c
- Linux clocksource, clockevent, sched_clock separation: https://github.com/torvalds/linux/blob/master/Documentation/timers/timekeeping.rst
- Linux high-resolution timer queues, callbacks and cancellation: https://github.com/torvalds/linux/blob/master/kernel/time/hrtimer.c

The architectural comparison establishes required invariants:

1. Hardware owns a monotonic counter and frequency (clocksource). System
   policy owns epoch, absolute nanosecond deadlines and calendar conversion.
2. Per-CPU clockevents generate IRQs for programmable deadlines; the software
   timer queue and scheduler quantum are separate mechanisms.
3. High-resolution timers require explicit QUEUED/FIRING/cancel ownership and
   callback/data lifetime. On Linux a CPU timer base tracks the executing
   callback separately. XNU has CPU timer queue migration at shutdown.
4. Timer queue selection must follow actual bounded workload measurements.
   The existing generic intrusive list remains valid until a measured and
   verified alternative preserves arm/rearm/cancel/fire semantics.
5. Real ARM virtual timer and AMD64 TSC/PIT/LAPIC rates cannot be replaced by
   fixed artificial frequencies, fabricated capabilities or quiet fallbacks.

## Actual NexsOS1 implementation and outstanding risks

- NX-HAL AMD64 uses bounded PIT-referenced TSC samples and the measured LAPIC
  countdown. TSC-deadline is selected only when CPUID exposes it.
- NX-HAL AArch64 uses CNTVCT_EL0/CNTFRQ_EL0 with a per-CPU CNTV_CVAL_EL0
  absolute one-shot clockevent.
- NX-CORE timer.c owns the actual per-CPU intrusive software timer lists,
  deadlines, callback/data and scheduler integration.
- The TIMER-UAF-01 fix currently depends on callback execution under
  timer_lock. Moving callbacks outside the lock without a proven FIRING,
  cancellation, rearm and synchronous teardown barrier is prohibited.
  This remains mandatory engineering work, not an accepted limitation.
- PCID/INVPCID is an optional AMD64 TLB capability and unrelated to TSC
  calibration; do not fake it or treat its absence as a timer failure.
- AMD64 GRUB/ACPI and AArch64 DTB/EFI require different hardware discovery.
  Full AMD64 ACPI table discovery is still open. Do not simulate an AMD64 DTB.
- Full strict Power12 findings (including R1/R2/R10/R11/R12) remain open.
  Successful boot tests do not imply Power12 qualification.

## Source changes in this checkpoint

- AMD64 APIC: after selecting a valid three-sample PIT/TSC quorum, report
  actual deviating measurements as WARN (also observed through NXPAX),
  preserve the selected measured frequency, and use compile-time bounded
  sample sorting/selection rather than an unchecked dynamic loop bound.
- NX-CORE: timer_ns_to_counter_ceil computes the 128-bit quotient and
  verifies its range before narrowing to 64 bits, avoiding deadline wrap.
  The existing software queue, comparator and scheduler remain unchanged.
- Kernel KTEST: add real API validation of overflow rejection and real
  queue arm/cancel with an extreme future deadline. These do not independently
  establish concurrency-safe callback teardown.
- Power12 AST: distinguish an array write through a loop index from a real
  mutation of that induction variable. Clang-AST mutation tests preserve
  failure on genuinely modified/unbounded loops and other existing rules.

The current LAPIC periodic routine already initializes event_hz from the
measured ticks_per_ms. An initial interpretation of a truncated source excerpt
was incorrect; that existing code was not replaced or duplicated.

## Incremental blocking policy and scope

The original project-wide make power12-strict / make check-strict is unchanged
and remains the final mandatory release gate.

The new make power12-changed runs real tests and a scoped hard Power12 audit
against HEAD-to-working-tree, covering staged, unstaged and untracked files.
Its inputs are the existing policy and AST/provenance rules. Modified source
functions and inserted lines are audited; deleted hunks are anchored to their
surviving source context. Failed AST parsing is a blocking R10 finding, not a
pass. Changed build files are screened for warning demotion or bypasses.
JSON evidence is at build/reports/power12/changed.json.

make power12 retains the full-project report-producing warning-mode audit
and additionally runs the new strict scoped gate. It does not suppress or
erase existing full-project findings. An empty changed scope means only that
nothing uncommitted is under review, NEVER that all code is Power12-qualified.

The changed-gate tests use a real disposable Git repository with separately
staged, unstaged, untracked and deletion-only cases. The AST mutation suite
uses real Clang parsing for index-write and induction-variable mutations.

## Verification and evidence procedure

Every iteration must compile the final sources for AMD64 and AArch64, run
the full NX-HAL host/sanitizer suites, and run the actual NXADB shell,
timerdiag, heartbeat and post-heartbeat tests. Inspect complete compiler,
UART and NXPAX logs for errors, warnings, faults and unreported fallbacks.
Staging is allowed only after the checkpoint passes.

The existing file docs/NXPAX-IMPLEMENTATION-STATE.md contains actual NXPAX
warning/fault evidence. The earlier compilation logs are:
- /tmp/nexs-timer-amd64-build-20261110.log
- /tmp/nexs-timer-aarch64-build-20261110.log
- /tmp/nexs-timer-hal-sanitize-20261110.log

Those logs cover an earlier source revision and MUST NOT qualify later edits.
Final source build, guest runs and exact diagnostics are to be appended only
after they have actually executed.

## Mandatory next timer phases

1. Measure per-CPU IRQ-off and callback wall time, armed-depth, scan/fire
   costs, event and software lateness with idle, service and mixed loads.
2. Implement a complete callback FIRING/teardown protocol with generation,
   queue ownership, bounded cancel synchronization, rearm and CPU-offline
   migration. Preserve generic callbacks and the TIMER-UAF-01 guarantee.
3. Benchmark unsorted/ordered intrusive lists and a proper RB timerqueue.
   Do not replace the current list without proven full semantic equivalence.
4. Validate cross-CPU monotonic counter stability, counter wrap, absolute
   deadline precision, LAPIC hardware min/max and ARM timer IRQ provenance.
5. Qualify full AMD64 ACPI discovery and AArch64 DTB/EFI independently.
6. Run five independent NXADB lanes and full Power12 strict qualification.
   None of these tasks may be marked complete on build-only evidence.
