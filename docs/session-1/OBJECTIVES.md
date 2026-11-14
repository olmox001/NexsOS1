# Session 1 Objective Index

This index is assistant-authored and subordinate to `/README-LLM.md`. It is
the compact operational register for Session 1. Detailed rationale and task
ordering remain in `PLAN.md`; verified facts remain in `STATE.md` and
`VERIFICATION.md`; unresolved defects remain in `ISSUES.md`.

Status vocabulary:
- **verified** — the declared acceptance gate has passed;
- **in progress** — implementation or verification is active;
- **queued** — accepted objective, not yet the active block;
- **blocked** — cannot advance until the named dependency is resolved.

| ID | Priority | Objective | Status | Acceptance / evidence required |
| --- | --- | --- | --- | --- |
| S1-O00 | P0 | Preserve owner directives and establish active/legacy documentation governance | verified | `README-LLM.md` preserved; Session 1 namespace active; legacy docs preserved byte-for-byte; documentation layout gate passes |
| S1-O01 | P0 | Establish a reproducible dual-architecture baseline | in progress | clean `make run` and graphical shell on AArch64 + AMD64; full nxadb gate and at least two meaningful commands on both; logs inspected |
| S1-O02 | P0 | Prevent PCI BusMaster/DMA authority during enumeration | in progress | enumeration leaves BusMaster disabled; explicit grant/revoke only after ownership; focused PCI/driver tests + dual-arch runtime |
| S1-O03 | P0 | Complete VirtIO DMA ownership lifecycle | in progress | PCI transport grant/revoke, MMIO-safe semantics, queue setup requires ownership, all blk/input/GPU failure paths revoke or quarantine, both architectures pass |
| S1-O04 | P0 | Repair graphical TTY double-echo without changing UART semantics | in progress | graphical command appears once, command executes once, UART remains debug-only; QMP screenshot + shell evidence on both architectures |
| S1-O05 | P0 | Run the real userland regression suite automatically at normal boot | in progress | manifest has explicit asynchronous one-shot mode; `nxtestrunner` starts without blocking PID1; `[NXTEST]` records visible on normal boot; suite completes |
| S1-O06 | P0 | Restore timing-behavior coverage in the runtime suite | in progress | existing `nxtimerdiag --queue-stress` remains; separate `--probe` case runs; host sampling regression remains green; runtime timing case passes |
| S1-O07 | P0 | Restore kernel/HAL warning and error policy to blocking | queued | kernel/HAL warnings fail the build by default on both architectures; no warning-demotion bypass; userland policy remains separately classified |
| S1-O08 | P0 | Freeze and classify the full Power12 debt | queued | strict report regenerated; findings numbered and grouped by rule/subsystem/owner/severity; no changed-scope regression hidden by baseline debt |
| S1-O09 | P0 | Drive Power12 findings to zero in priority order | queued | bounded batches, real tests per affected kernel/HAL function, changed-scope zero after each block, full strict gate eventually passes |
| S1-O10 | P1 | Remove hard-coded device/display assumptions | in progress | provider-discovered hardware modes/resources; physical scanout separated from compositor workspace and application geometry; no QEMU-only generic policy |
| S1-O11 | P1 | Add explicit device lifetime/generation/hotplug/quiescence | queued | handles have lifetime/generation ownership; detach drains IRQ/DMA/in-flight work before free; bounded timeout/quarantine |
| S1-O12 | P1 | Make userland `nxcompositor` authoritative | queued | one userland scene/focus/z-order authority; kernel retains only hardware/capability/surface-buffer mechanisms; no duplicate scene registry |
| S1-O13 | P1 | Complete terminal/window resize ownership model | queued | terminal history reflows correctly; graphical apps resize buffers/surfaces rather than crop or thicken borders; hardware mode changes remain separate |
| S1-O14 | P1 | Stratify the SDK | queued | primitive/sandbox, libc, portability, extensions, UserKit and DriverKit boundaries documented, built and independently testable |
| S1-O15 | P1 | Move ordinary drivers toward DriverKit/userland | queued | driver sandbox defined; platform/HAL ownership reduced; each migrated driver has explicit capability/resource lifecycle |
| S1-O16 | P1 | Remove POSIX/VFS policy from kernel ownership | queued | kernel object/capability mechanisms remain policy-neutral; userland/NXOLMO-backed VFS/storage plan verified before migration |
| S1-O17 | P1 | Preserve diagnostics/fault separation and NXPaX visibility | queued | HAL/kernel/userland layers remain distinct; UART debug shows diagnostics/fault records; no fake print-only scaffolding |
| S1-O18 | P1 | Restore/verify service, execution, TTY and focus contracts | in progress | `execsvc` common contract used; service authority explicit; TTY focus/input/output routed to correct window; sbin protections retained |
| S1-O19 | P2 | Add optional upstream study references without build coupling | queued | exact Linux/seL4/BSD revisions/license/purpose recorded; normal setup/build never requires submodule initialization |
| S1-O20 | P2 | Resolve version baseline and qualify the path to 0.1.0.0 | queued | 0.0.6.2 vs current 0.0.6.3 discrepancy resolved explicitly; version advances only after verified release gates |

## Current checkpoint

The current checkpoint covers **S1-O01 through S1-O06** plus the execution/
TTY portion of **S1-O18**.  On the final clean AArch64 gate, graphical boot,
boot-suite lifecycle, shell interaction/history, independent timer probe and
counter heartbeat all pass; `KTEST=37/0` and kernel faults are zero.  The
graphical double-echo is fixed by the existing `nxttyd --shell` mode.

The execution model is now explicitly layered: `execsvc` owns base foreground
vs detached process ownership; `nxttyd` is a separate TTY/CLI hosting layer;
`nxjobs` extends foreground execution with shell job control.  A CLI started
without a ctty receives a hosted terminal, while a pixel/self-rendered GUI
retains inherited or headless TTY diagnostics without writing text over its GUI
surface.  Boot `nxtestrunner` runs in a visible independent TTY.  The stable
suite remains 18/19 because the already-documented coreutils case is 42/50;
both timerqueue and the appended `18nxtest_timerprobe` pass on the clean gate.

A real shell job-control probe passed background -> `jobs` -> `fg` ->
completion.  Ctrl-Z -> stopped -> `bg` is still an explicit follow-up test,
and the latest execution/TTY changes still need a fresh AMD64 runtime gate;
their SDK/userland compile paths are already verified for both architectures.

The next bounded block after this staged checkpoint is **S1-O07 only**:
restore kernel/HAL warnings and errors to blocking.  Timing qualification and
the broader **S1-O08** Power12 inventory remain separate later blocks and must
not be pulled into S1-O07.
