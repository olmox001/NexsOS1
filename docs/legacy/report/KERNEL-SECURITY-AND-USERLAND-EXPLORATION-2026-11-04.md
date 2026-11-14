# Kernel security and live userland exploration — 2026-11-04

Status: audit in progress; no kernel source changes made by this report.
Scope: NexsOS1-Resurrection/NexsOS1 only.
Runtime: AArch64, GRUB/UEFI, four CPUs, visible QEMU display, NXADB-controlled shell.

## 1. Live boot and runtime evidence

Booted with `python3 tools/nxadb.py --arch aarch64 start --display default`.
NXADB reached both `Entering supervisor loop` and the actual NXShell prompt.
The session used the complete UART path:
`build/runtime/nxadb/aarch64/serial.log`
and QEMU stdout/stderr:
`build/runtime/nxadb/aarch64/qemu.stdout.log`,
`build/runtime/nxadb/aarch64/qemu.stderr.log`.

Observed boot sequence: four CPUs online; compositor initializes at 1280x800; PID 1 nxinit; PID 2 nxntfy_srv; PID 3 nxexec; PID 5 nxui; PID 6 nxbar; PID 7 nxlauncher; PID 8 nxshell. No PANIC/FAULT/SSP/STACK-ALIAS marker was present in the captured live log at the end of this exploration. This is a bounded observation, not proof of kernel correctness.

Initial screenshot:
`build/runs/aarch64/kernel-security-aarch64-initial.png`
Verified 1280x800. The visible desktop showed the NXShell window and the dock/bar.

## 2. NXADB usage and shell exploration

Important tool semantics confirmed from `tools/nxadb.py`:
- All mutating/session commands share a per-architecture flock. Do not run multiple guest-input operations concurrently.
- `nxadb shell <command>` injects one command and waits for a prompt. It does not provide shell chaining semantics; semicolon-separated commands are not a valid batch and can be typed as literal input.
- Shell prompt detection is essential: after focus leaves NXShell, `run_shell_c` times out because input is delivered to the newly focused window.
- Recovery was performed through the documented NXADB QMP mouse helper: move pointer and click inside NXShell, then verify prompt access again.
- `nxadb mouse dx dy --click` supports relative pointer movement and click, but not a maintained-button drag gesture.
- `nxadb screenshot` captures and verifies the QEMU display.

Commands exercised in the guest:
- `ls /sys/bin`: confirmed the system tools are installed under /sys/bin.
- `/sys/bin/nxinfo`: reported AArch64 NexsOS1 V0.0.6.0, 1280x800, uptime and PID. This is the historical result recorded on 2026-11-04; the subsequent V0.0.6.3 build must be tested separately.
- `/sys/bin/nxperm`: reported root caller and spawn/files/ipc/windows/registry services.
- `/sys/bin/nxproc`: showed idle tasks and system process states across CPUs.
- `/sys/bin/nxwins`: showed window IDs, owner PIDs, visibility/minimized/focus state.
- `/sys/bin/nxsettings -cli`: reported display presets, applied style/theme/background, permissions, processes, PMM/kheap/scheduler/object stats and notifications. It explicitly does not change display mode or zoom.
- `/sys/bin/nxreg`: opens its interactive registry UI; it is not a harmless --help command.
- `/sys/bin/nxres --help` and `/sys/bin/nxwins --help`: usage output confirmed; no display setting was changed.
- `/sys/bin/nxwins focus 102`: focus operation accepted; focus was restored to NXShell by clicking the shell window with NXADB mouse.
- Final `nxadb status`: QEMU running, shell detected, faults=0.

A command without its full `/sys/bin/` path may resolve through /bin first; use the explicit path during automated tests to avoid ambiguity. The earlier semicolon probe also showed that a multi-command string is not executed as a POSIX shell script.

## 3. Runtime performance observation

During normal desktop boot and shell operation, the UART repeatedly emitted `[GFX-TIMING]` entries with render_hold commonly around 30–100 ms, and occasional lock_wait values in the 30–35 ms range. This is a serious latency signal for a 1 ms timer/tick environment. It must be profiled by call site and lock ownership before changing compositor synchronization. Do not assume all time is spent doing pixel composition; measure phases and lock wait/hold separately.

## 4. Security findings requiring remediation or proof

### KSEC-01 — Registry VFS stale-node lifetime (high)

Source path:
- `kernel/lib/registry.c`: regfs_open stores a `reg_node *` in `vfs_node.id`.
- `registry_del()` removes/frees registry nodes and descendants.
- `regfs_read()` dereferences the stored node.
- `kernel/core/object.c`: file object retains the VFS node; sys_object_read refreshes by path but has a fallback to the stored node when lookup fails.

This creates a stale pointer after deletion if a file handle outlives the Registry node. Required design: pin/refcount the registry node or make regfs file handles store a stable key/identity and fail cleanly after deletion. Do not rely on the contradictory comment that nodes are never freed. Add regression coverage: open key/file, delete key/subtree, read through old handle; expected safe error, no UAF.

### KSEC-02 — Handle ABA / stale index reuse (high)

`struct handle_entry` contains object pointer and rights; `handle_install_locked()` reuses the first free slot. No generation is encoded in the public handle. A stale integer can refer to a different object after close/reopen. Capability tests 64/64 do not by themselves prove ABA resistance. Need a versioned handle ABI (or a compatibility-safe generation side table) and stale-handle tests before relying on long-lived surface/buffer capabilities. Audit every ABI caller before changing the representation.

### KSEC-03 — nxexec privileged confused deputy (critical; confirm/fix before service migration)

`nxexec` runs as a privileged /sys/bin service and accepts requests through the named `OS1nx_exec` port. `OS1_NS_PORT` permits any process to obtain a SEND-only handle to an existing named port. The message's `from` PID is kernel-stamped and nxexec uses it as logical owner and for client-FD redirection, but the child spawn path calls the ordinary path-based spawn syscall as nxexec. The kernel's per-path level policy therefore sees nxexec as creator; a USER caller requesting a /sys/bin executable can receive a child with the /sys/bin preset instead of the caller's authority. The code comment claiming the child under /bin lands USER does not address the /sys/bin fallback. The kernel must bind the original requester to the delegated spawn and enforce a creator ceiling/policy at the syscall boundary; service-side checks alone are insufficient. Regression test matrix must include USER/GUEST requests for /bin and /sys/bin, direct spawn vs nxexec spawn, and capabilities observed in the child. Do not perform an exploit probe against the live desktop; reproduce in a controlled test image.

### KSEC-04 — Display-global rights conflated with window rights (high)

The current capability presets include `CAP_WINDOW` for GUEST. Display mode, style, and zoom setters in syscall dispatch are gated using `CAP_WINDOW`, even though they mutate global display state. The November ABI matrix already anticipates a separate `CAP_DISPLAY`. Separate window-local operations from global display configuration and test each level against every setter. No display settings were changed in this session.

### KSEC-05 — Scheduler outgoing-stack lifetime window (high; strong source-level candidate)

The scheduler comments explicitly state that interrupt EOI and IRET/ERET still execute on the outgoing process's kernel stack after `schedule()` selects another task. The current `pending_reenqueue` protects only an outgoing task in `PROC_READY`. `__sched_retire_prev()` leaves a sleeping outgoing task off the runqueue; after `cpu_ptr->current_task` is updated to the next task, an external `process_terminate()` can see the sleeping victim as not current on any CPU and enter the immediate free path, while the CPU may still be executing the interrupt-return tail on that victim's stack. This is a narrow SMP race and a plausible source of intermittent stack corruption/SSP. The correct fix needs an explicit per-CPU outgoing-stack retirement reference covering every task switched away from, not only READY tasks. At the next schedule entry (after the previous IRET/ERET), resolve it according to state: requeue READY, retain SLEEPING, reap DEAD/ZOMBIE. External termination must treat this pending outgoing reference as still-live. Audit all paths that can free a PCB/stack, including wait-queue, IPC, sleep timer, stop/continue and process pool removal. Add SMP stress tests for killing a task while it transitions through sleep/wakeup and for repeated self-exit.

### KSEC-06 — process_create_caps allocation and rollback (high)

The current `process_create_caps()` allocates the environment block before taking `sched_lock`, but allocates the process descriptor with `pmm_alloc_page()` while the lock is still held and IRQs are disabled. This contradicts the lock/allocator rule in the surrounding comments and creates an allocator lock-order/deadlock risk. Move all potentially blocking allocations before the scheduler lock, then revalidate pool slot/quota under lock and release speculative allocations on failure.

Also inspect `__proc_create_stdio_and_pgd()` and `__proc_create_rollback()`: verify every failure branch after stdio/handle table and PGD creation destroys all acquired objects and allocations. Current code path must explicitly check PGD creation failure and release the environment block and installed handles during rollback. Confirm exact helper implementations before patching; this report records the source-level gap, not a claimed fix.

### KSEC-07 — lockcheck parser false negative

`make lockcheck` reported 2 RULE-A and 5 RULE-B. Source inspection shows its text-based control-flow approximation can mis-handle early unlocks in error branches and miss a later allocator call still inside the normal lock-held path. The checker must be upgraded to model branch/return paths or complemented with a targeted static rule/test. Preserve the raw output in the security audit log directory.

## 5. What this session did not do

- No kernel/userland source was edited.
- No staged, unstaged, or untracked project changes were reset or staged.
- No commit or push was made.
- No guest display mode, zoom, style, theme, or background was changed.
- No exploit was attempted against nxexec or Registry.
- This session only performed AArch64 runtime exploration. AMD64 must receive the same security and interaction test matrix after the source fixes.

## 6. Ordered next work

1. Preserve the live logs and complete source-level audit.
2. Fix KSEC-05 scheduler stack retirement first; it is directly relevant to the intermittent SSP and must be tested under SMP.
3. Fix KSEC-06 process creation lock/rollback.
4. Fix KSEC-03 delegated spawn authority and KSEC-04 display rights.
5. Fix KSEC-01 Registry/VFS node lifetime and KSEC-02 handle ABA with ABI compatibility tests.
6. Improve lockcheck and add deterministic regression tests for each issue.
7. Run clean builds and NXADB tests on both amd64 and aarch64, then live userland/window/input tests.
8. Only after the kernel baseline is hardened, resume GFX caller/terminal audit and design the userland compositor + nxsurfaceserver split according to the existing migration plans. The single-file surface-manager reference prompt is obsolete and must not be ported literally.

## 7. Historical-plan reconciliation (June/July 2026)

The scheduler concern above was checked against the historical records rather than treated as a new discovery in isolation:

- `docs/review/analysis/10-addendum-2026-06-11-sched-uaf-followup.md` records SCHED-UAF-01/02/03, the parked-sleeper leak and the rule that stack/PGD release must occur only after switching away. Its review explicitly says fixes were built and headless-booted on both architectures.
- `docs/review/analysis/11-addendum-2026-06-11-external-trace-triage.md` records the earlier SLEEPING/IPC-sleeper race and the existing `__proc_still_current_anywhere()` mitigation. The current concern is narrower: `schedule()` updates `cpu_ptr->current_task = next` before returning through the interrupt dispatcher, while its own comment says EOI + IRET/ERET still execute on the outgoing stack. The outgoing stack is therefore no longer represented by `current_task` during the return tail. The present `pending_reenqueue` covers READY only. This is a re-audit of the invariant after the June fix, not a claim that the June fix never existed. Confirm with exact exception-entry/exit assembly and add a deterministic race test before changing it.
- `docs/PIANO-LIBC-ASTRA-2026-07-16.md` puts privileged execution and service migration behind kernel-enforced capability policy, and says the service split follows the ASTRA object model. Its R6 notes nxexec as a privileged daemon, but the live code must not let it silently widen the requester's spawn authority.
- `docs/PLAN-2026-07-17-STRATIFICATION.md` orders scheduler substrate and lock hardening before compositor serverization: Phase 1 preemption/block/wake correctness; Phase 2 compositor/scheduler/object lock contention; Phase 3 compositor/window server. Phase 5 then separates fine-grained process/window/display rights and per-service privilege masks. This ordering directly supports the current security-first instruction.
- `docs/PLAN-2026-07-23-HARDENING-SDK-RUST.md` is a staged hardening plan, not a requirement to add Rust now. Reuse its review gates and memory-safety sequencing only after checking their current implementation status.
- `docs/ASTRA.md` and `docs/ROADMAP-ASTRA-SERVERIZATION.md` remain the target architecture: kernel primitives are exposed through object/capability contracts; services become supervised userland processes; there must not be parallel legacy and server paths after a migration. The supplied single-file surface-manager sample is not a target implementation and must be decomposed into the existing object, buffer, surface, input, graphics, font/theme and compositor responsibilities.

The July plans are historical/living direction documents, not proof that every listed step is still current. For each change, code and current test evidence remain authoritative.

### KSEC-05 proof refinement — architecture return epilogues inspected

The outgoing-stack window is now supported by both architecture paths, not inferred from comments alone:

- AArch64 `kernel/arch/aarch64/cpu/exception.S`: `irq_stub` builds the 816-byte `pt_regs` frame on the interrupted stack, calls `irq_handler`, branches to `restore_context`; only `restore_context` executes `mov sp, x0` to move to the selected task's frame, then restores registers and executes `eret`.
- AMD64 `kernel/arch/amd64/cpu/isr_stubs.S`: `common_isr_entry` saves registers on the interrupted stack, calls `amd64_isr_dispatch`, then branches to `restore_context`; only there does `movq %rax, %rsp` switch to the selected frame before `iretq`.
- AMD64 `arch_cpu_switch_context()` also updates `cpu->current_task = next` and `cpu->stack_top` before the common epilogue changes RSP. AArch64 updates `cpu_ptr->current_task` in `schedule()` before the same epilogue changes SP.
- `schedule()` releases `cpu_ptr->sched_lock` before returning the selected `pt_regs *`. Therefore an external terminator can acquire that lock, observe the new `current_task`, fail to see the outgoing process in `__proc_still_current_anywhere()`, and free the old stack while the IRQ/exception return code is still executing on it.

The required invariant is now precise: **a process stack remains pinned from the moment it is selected as `prev` until the selected frame is installed by the architecture restore epilogue**. A per-CPU outgoing-frame/stack pin must be visible to termination and to wake/continue enqueue paths. Merely extending the READY-only `pending_reenqueue` pointer without handling wakeup/continue would still permit a sleeping outgoing task to be re-enqueued and stolen before its old stack is abandoned. Implementation must audit all runqueue insertion paths and both architecture restore paths.

## 8. First kernel hardening patch — pending clean-build/runtime verification

Source changes currently made (not yet staged as source, not committed):

- Replaced `cpu_info.pending_reenqueue` with `pending_stack_release`. Every non-idle outgoing process is pinned until the next scheduler entry on that CPU. The reaper/requeue helper now distinguishes READY, SLEEPING, STOPPED and DEAD/ZOMBIE.
- `__proc_still_current_anywhere()` treats the outgoing-stack pin as live. `__enqueue_task()` defers READY publication without runqueue insertion while the pin remains, covering wait-queue wake, IPC wake, public enqueue and process continue through their common enqueue seam.
- Panic and SSP diagnostics now print `pending_stack_release`.
- `process_create_caps()` now allocates both the environment page and process descriptor before taking `sched_lock`; quota/pool failure paths release both speculative allocations.
- The creator PID and controlling-terminal window are snapshotted while `sched_lock` protects the creator. The post-unlock setup helper no longer dereferences a possibly terminated creator pointer.
- `__proc_create_stdio_and_pgd()` now treats a NULL PGD as failure. Rollback destroys any installed handles, frees the environment page through `proc_env_free()`, destroys a created PGD, and then frees the process descriptor.

These changes are intentionally grouped because they address one kernel lifecycle boundary. They remain provisional until the clean AArch64+AMD64 build, boot, KTEST, NXADB shell and process-lifecycle stress tests pass. If a test fails, do not stage the source changes.

## 9. Process-kill model and stress-harness reconciliation

`docs/PROCESS-KILL-MODEL.md` preserves an additional, separate unresolved issue: Pitfall C, a possible `__dequeue_task` lifetime race under rapid window close, plus the documented spawn/window ownership handoff race (zombie PID and orphan window). The outgoing-stack pin patch above does **not** claim to fix those subtree/window teardown issues. They need their own source audit and regression cases; do not conflate them with KSEC-05.

The old process-kill test plan also refers to `tools/run-stress.sh`, but that script is not present in the current checkout. The current in-tree workload driver is `/bin/stress`; its documented lanes include spawn/wait, allocation, file I/O and GUI create/destroy. The current statistics service supports `/sys/bin/nxmemstat --log [interval] --run /bin/stress [args...]`, spawning the USER workload from the ROOT logger and emitting MEMSTAT CSV to serial. The supported harness is therefore the checked-in nxadb lifecycle/test gate plus nxmemstat+stress, not an assumed run-stress.sh script.

### Historical manual NXADB log details

The checked-in `nxadb-manual-aarch64.log` shows the prior agent's shell exploration at approximately 499 seconds uptime: Registry enumeration and `cat /reg/system/version`, `nxfilem` launch (Registry records PID 26), window enumeration before/after close/kill, `nxproc` snapshots, final `nxinfo`, and `jobs` reporting no background jobs. The file shows harmless `/bin/<sys-tool>` lookup errors before the PATH fallback resolves the corresponding `/sys/bin` program. It also contains one host-side command mistake: `nxperm whoami` was passed to nxadb itself (which rejected the unknown argument) rather than typed into NXShell; that particular line is not evidence of a guest permission test. The later direct `nxperm` output is the valid permission observation.

## 10. Stress driver correction

The first 12-second AArch64 spawn-lane run exposed a test-driver defect, not a valid kernel pass/fail result. `user/sys/lib/lib.c:837` explicitly documents `wait(pid)` as NON-BLOCKING (`-1` means the child is still alive); `user/bin/stress.c` called it once and immediately spawned another child. It also used `/bin/captest` as its supposedly trivial child, causing many full capability suites and GUI windows to overlap. The run consequently reached the compositor's user-window limit and showed concurrent `captest` port/ftruncate failures; it is invalid as a serial process-lifecycle measurement.

Corrected the stress spawn lane to use `/bin/hello` for normal exits, `/bin/crash` for intentional fault exits, and poll non-blocking `wait(pid)` with `yield()` until it returns the PID or `-2` (already gone). Only then does the next cycle spawn another child. This makes the default spawn lane a controlled create/exit/reap test. A separate bounded multi-child lane will be needed for genuine concurrent stress. Rebuild and rerun both architectures before treating this corrected harness as evidence.

The first correction's normal-child choice was also wrong: `/bin/hello` is not a trivial utility in this tree; it is a roughly 900 KB OpenGL black-hole demo. The subsequent 3-second no-fault run therefore waited forever on an interactive renderer. The normal child is now `/bin/true` (confirmed present in the built rootfs), while `/bin/crash` remains the intentional fault child. The previous timed-out runs are not kernel regressions and are not counted as successful stress evidence; the corrected `/bin/true` workload must be rebuilt and rerun.

## 11. Corrected kernel patch verification results

Builds:
- A clean rebuild after `make clean` completed for AArch64 and AMD64; both bootable GRUB ISOs were produced.
- After the stress-driver correction, incremental `make ARCH=aarch64 boot-iso` and `make ARCH=amd64 boot-iso` completed successfully again. The resulting rootfs binaries are newer than `user/bin/stress.c`.

NXADB test gate, visible display, 30-second soak:
- AArch64: `NXWINS=PASS(window=104,focus=PASS)`, `SOAK=30s`, `GUEST-HEALTH=PASS`, start/end screenshots 1280x800, `KTEST=11 passed/0 failed`, `faults=0`, exit 0.
- AMD64: same window/focus and 30-second soak results, start/end screenshots 1280x800, `KTEST=10 passed/0 failed`, `faults=0`, exit 0.

Process lifecycle:
- `/bin/capkill`: zero failures on both architectures; child spawn, denied kill without DESTROY right, child survives denied kill, kill via capability, child termination all passed.
- Corrected `/bin/stress` spawn lane, deterministic seed `0xA11CE`, 5 seconds, normal child `/bin/true`: AArch64 133 spawn/reap cycles, AMD64 218; zero errors.
- Same 5-second run with 10% intentional `/bin/crash` children: AArch64 91 cycles / 8 user faults / 0 errors; AMD64 184 cycles / 19 user faults / 0 errors. AArch64 reports `Data Abort (Lower EL)`; AMD64 reports `PAGE FAULT`; both are correctly attributed to the deliberately faulting user process. NXADB kernel fault counters remained zero; no kernel panic or SSP marker appeared in the captured runs.

These results demonstrate clean boot, the existing KTEST suite, window/focus operation, process spawn/exit/reap and expected user-fault teardown on both architectures. They do **not** yet constitute a deterministic reproduction of the narrow SMP race that motivated KSEC-05: the corrected stress lane is sequential spawn/wait. A bounded multi-child SMP test that kills a SLEEPING outgoing task during the post-`current_task`/pre-`restore_context` interval is still required before KSEC-05 can be closed. Process-create allocation-failure injection and the Registry/handle/nxexec/display-right findings also remain open.
