# NexsOS1 — Build, Boot, NX-HAL/NX-CORE and NXPAX Stability Checkpoint

Date: 2026-11-10. Branch: `DEV-Resurrection`. Recovery started from
`34a6bf5`; the tested source changes were subsequently committed as
`3b71274` (2026-11-10 11:19 +0100). This checkpoint concerns the tested
implementation, not a certified release or proof of every remaining obligation.
The separately staged `V0.0.6.1` version update requires its own clean-build
and NXADB acceptance evidence before being treated as verified.

## Goal and scope

Recover deterministic cross-architecture clean builds and working QEMU/NXADB
boot paths without replacing the timer, downgrading fault reporting, removing
the intrusive timer queues, inventing stub tests, or discarding previously
staged implementation work. Preserve the explicitly requested deletions of
orphaned `kernel/graphics/draw2d.c` and `draw3d.c`.
The two userland `system/lib/common/{string,vsnprintf}.c` sources are
present and match tracked source; they were not deleted by this recovery.

## Build profile and strictness

- `make clean`: passed, removing AMD64/AArch64 build outputs while retaining
  runtime records and ignored diagnostic logs.
- `make ARCH=amd64 boot-iso`: passed; generated `kernel.elf`, `disk.img`,
  and the GRUB BIOS `grub-bios.iso`.
- `make ARCH=aarch64 boot-iso`: passed; generated `kernel.bin`,
  `disk.img`, `virt.dtb` and the GRUB UEFI ISO.
- Independent `make -C kernel ARCH=<arch> BUILD_ROOT=../build kernel`:
  passed for both architectures with the mandatory NX-CORE/NX-HAL compiler
  error classes.
- NX-CORE and NX-HAL compiler defaults are independently blocking for
  correctness/hardening warning categories. `NX_WARNINGS_AS_ERRORS=0` is
  an **explicit**, caller-selected bypass of that baseline gate, not the
  default. `-Wconversion` and `-Wsign-conversion` remain enabled and visible
  in ordinary kernel/HAL builds while each narrowing site is audited;
  `NX_CONVERSION_AS_ERROR=1` promotes them to hard errors. The conversion
  remediation has **not** been completed; do not describe this as warning-zero.
- First-party userland migration builds keep their warning diagnostics visible
  without failing for existing warnings. The opt-in compiler escalation is
  `NX_WARNINGS_AS_ERRORS=1`.
- `make power12` is a **nonblocking development audit**. It runs all checker
  stages on changed audited inputs, preserves their true non-zero statuses and
  uses a content-digest cache only for identical audited input sets. The
  evidence manifest retains `verified=false` for open findings.
- `make power12-strict`, `make check-strict`, `release` and `release-arch`
  keep the **blocking** project-wide Power12 gate. A strict run on this source
  failed as required: 373 R11/R12 ID/limit provenance findings and 5,045
  structural AST findings. The build-policy stage reports zero findings;
  the 13 project-wide mutation tests pass. **Power12 qualification is OPEN.**
- `make run` is an interactive/long-running QEMU target by design. Its former
  apparent hang at a repeated AST audit was mitigated with an input-verified
  audit cache. A successful QEMU launch is not the exit of `make run`; use
  NXADB for automated acceptance and stop the guest after the test.

## Fresh NXADB runtime acceptance

All lanes were tested serially against the freshly compiled artifacts.
Each lane ran the real NXADB test gate: shell interaction, history navigation,
timer diagnostics through the supervised service, shell recovery, live counter
heartbeat, kernel self-tests and an ordinary NXPAX service probe. Each also
executed a distinct, serial-confirmed shell marker.

| Target | Boot path | NXADB | Kernel tests | Unexpected faults |
| --- | --- | --- | --- | --- |
| AMD64 | GRUB BIOS ISO | PASS | 28 passed, 0 failed | 0 |
| AMD64 | Direct `-kernel` | PASS | 28 passed, 0 failed | 0 |
| AArch64 | GRUB UEFI ISO | PASS | 29 passed, 0 failed | 0 |
| AArch64 | Direct `-kernel` + DTB | PASS | 29 passed, 0 failed | 0 |

All four guest runs were shut down. The acceptance commands were equivalent
to `python3 tools/nxadb.py -a <arch> --boot-mode <iso|direct> test -n 1
-c 'echo NX_STABILITY_20261110_<arch>_<mode>'
-c '/sys/bin/nxpax --probe'`. The output marker must occur in serial at
least once. The former wrapper incorrectly expected two marker occurrences;
that wrapper condition was corrected without changing any guest verdict.

### Controlled user-fault regression

Each architecture also booted via ISO and launched the existing real
`/bin/crash` executable. AMD64 reported a user page fault and AArch64 a lower
exception-level data abort. In both cases the offending process was
terminated, a following shell `echo` returned the expected marker, and
`/sys/bin/nxpax --probe` returned hardware `kind=1` and Core user-termination
`kind=2` records, each with `units=1`, `overrun=0`, `dropped=0`.
These expected fault records were not counted as failures in the separate
zero-unexpected-fault boot matrix.

## Timer service and SDK boundary

The generated kernel TIME statistics transport currently uses
`os1_timer_stats_snapshot_v4`, but the public SDK API no longer embeds
a revision-suffixed TIME statistics type. `system/lib/lib.c` exposes a
caller-sized privileged raw read without naming the wire structure.
`nxtimerd` alone reads that revisioned object and validates wire ABI version,
record sizes and bounded CPU count. It copies the 23 meaningful fields of each
CPU record into the stable SDK service model; the SDK client checks the
bounded response before presenting it to `nxtimerdiag` and future programs.
Version updates to this wire contract therefore change the broker/contract
boundary rather than every consumer's source type. The existing timer queue
and scheduler logic remain unchanged by this service-interface migration.

## NX-HAL security and instrumentation

- The existing architectural IRQ-mask stats instrumentation (AArch64 DAIF;
  AMD64 RFLAGS) and HAL fault protocol are retained, with no new architecture
  layer or alternate interrupt dispatcher.
- The IRQ-mask telemetry host tests cover nested IRQ masking, transitions,
  invalid counter samples, bounded snapshots and argument limits.
- `make -C kernel/HAL test-host` and `test-sanitize` passed. The latter
  uses actual AddressSanitizer/UBSan-instrumented test executables.
- The existing NXPAX HAL ring is real, bounded, nonblocking in fault context
  and supports concurrent event writers. The unchanged host stress tests
  passed scenarios of 1,000,000 and 2,000,000 events, recording genuine drop
  accounting rather than claiming lossless capacity.
- Pending: full architecture-entry fault coverage, actual IRQ-off root-cause
  measurement for AArch64 timer latency, complete system Power12 proof,
  and cross-host/hardware qualification.

## NX-CORE security and build

- The kernel keeps its existing object, timer, queue, scheduler and
  user-fault disposition implementations.
- `SYS_DRAW` validates its unsigned coordinate/size domain before narrowing
  syscall arguments to the graphics API. Invalid dimensions return
  `-EINVAL`, rather than relying on sign-changing implicit conversions.
- The NXPAX Core termination record is emitted after an actual user fault,
  distinct from the preceding HAL hardware record.
- `make check-kernel-contracts` passed, including generated TIME contract
  layout for AMD64/AArch64 and NXPAX ABI header parity.
- Pending: complete semantic classification and correction of remaining
  conversion warnings, R11/R12 catalog provenance, structural AST findings,
  capability-provenance validation of the NXPAX broker reply handle, and
  bounded in-flight/gap accounting for NXPAX event sequences.

## Static and staged source review

Before the recovery work, the index held 46 staged files. Each staged path
was enumerated and `git diff --cached --check` passed. The review checked
the source manifests and callers of every staged deletion; the removed
`compositor.c.new`, `registry.h.new` and `nxshell.c.old` entries are stale
draft/backup alternatives, not the compiled canonical sources. The deletion
of `draw2d.c` and `draw3d.c` remains explicitly authorized. Do not restore
either merely because an archived graphics plan still mentions them.

Other indexed edits are organized across ASTRA and direction documentation,
HAL architecture/core IRQ-mask telemetry, timer statistics ABI v4 and
snapshot contracts, object/registry cleanup, kernel timer export,
`nxqemu` CPU selection and real tests, userland timer client/service and
the Power12 policy. Cross-architecture clean builds, host tests and the
full guest matrix were used as a regression check on the combined tree.
These checks are **not** complete formal proof for every modified function.

The accidentally removed `tools/power12_audit.py` and
`tools/test_power12_nxpax.py` were recovered from the unchanged HEAD
contents without resetting Git. The nine dedicated NXPAX mutation tests
and thirteen project-wide mutation tests pass. R11's identifier classifier
was corrected with positive/negative tests: `KEY_RIGHT`, `BTN_RIGHT`,
`NS_PER_TICK` and VirtIO hardware event bits are no longer incorrectly
treated as capability operations; real operation/right/namespace IDs still
require actual policy entries. 373 R11/R12 findings remain, not suppressed.

## Evidence and handoff

Local transcripts (intentionally outside tracked sources):
`/tmp/nexs-final-build-amd64.log`,
`/tmp/nexs-final-build-aarch64.log`,
`/tmp/nexs-final-matrix.log`,
`/tmp/nexs-final-fault-matrix.log`,
`/tmp/nexs-final-nxpax-host.log`,
`/tmp/nexs-final-hal-sanitize.log`,
`/tmp/nexs-final-contracts.log`,
`/tmp/nexs-final-power12-strict.log` and
`/tmp/nexs-final-power12-development.log`.
Complete UART and QEMU stderr outputs are under
`build/runtime/nxadb/<arch>/<lane>/`; per-run artifacts under
`build/runs/<arch>/<lane>/` and checker reports under
`build/reports/power12/`.

This checkpoint is eligible for preserving and pushing as a **tested
development baseline**, not as a Power12-qualified release. Re-run the hard
compiler/contract gates and guest matrix after any further runtime code
changes. Never mark open catalog, ownership, ABI/security or fault-handling
work as completed solely because an ISO booted.
