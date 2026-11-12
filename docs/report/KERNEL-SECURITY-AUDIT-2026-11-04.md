# Kernel security audit — 2026-11-04

Status: OPEN — source audit and runtime reconnaissance; no kernel source fix applied in this pass.
Scope: current NexsOS1-Resurrection/NexsOS1 only. AMD64 and AArch64 must remain supported. Findings are classified; not every item is yet a proven exploitable vulnerability.

## 1. Method and evidence discipline

- Compare current architecture/direction docs, ABI matrix, November migration notes and June/July planning against current source. June reviews are historical snapshots; July and November updates supersede them where explicit.
- Runtime exploration uses tools/nxadb.py with explicit architecture. Issue one guest-shell command at a time and wait for the prompt. A timeout is not proof of command completion.
- Preserve serial, QEMU stdout/stderr, manifest, screenshots and exact commands under docs/report/logs/KERNEL-SECURITY-AUDIT-2026-11-04/.
- Standard 30-second soak is shorter than historical ~345-second AArch64 fault interval. Fault recognition misses some fatal signatures; test-all is not proof of fault absence.
- Existing staged and unstaged work is preserved. Never reset, overwrite or commit mixed source changes without reconciling index and worktree.

## 2. Runtime exploration inventory

### 2.1 AArch64 live session

The AArch64 NXADB session remains running at report creation. Live serial: build/runtime/nxadb/aarch64/serial.log. QEMU stdout/stderr, manifest, interaction notes and screenshots are retained under docs/report/logs/KERNEL-SECURITY-AUDIT-2026-11-04/.

- GRUB/UEFI reaches desktop at 1280x800.
- Guest has one NXShell prompt. Shell requests must be serialized; concurrent requests can queue.
- nxinfo, nxperm, nxproc, nxwins, nxres, nxreg and nxsettings -cli return guest data. Historical bare command names may first emit ELF-not-found for /bin/name before PATH fallback; do not misread this as final failure.
- Observed services: nxinit PID 1, nxntfy_srv PID 2, nxexec PID 3, nxui PID 5, nxbar PID 6, nxlauncher PID 7, nxshell PID 8. PIDs are session-specific.
- nxpower -cli status reports power control unavailable. nxsettings -cli display is informational; nxres changes display mode/zoom/style/theme/background.
- Current image has /sys/bin and /sys/lib static libraries; /sys/sbin, /sys/lib/nexs and /sys/lib/nexs/services are absent. Those are proposed destinations, not current filesystem facts.

### 2.2 Historical evidence

- Prior-agent AArch64 exploration is retained in USERLAND-SHELL-EXPLORATION-AARCH64-2026-11-04/: serial, QEMU logs, manifest, mouse movement/click evidence, editor/file-manager screenshots. It covers process/window/registry inspection, nxfilem, mouse input and Ctrl-Q close.
- Prior AMD64 notes report 1280x800 desktop with multiple windows/dock and a VirtIO-Input IRQ 42 shared-handler warning. Correlate with IRQ registration/dispatch; not yet a diagnosis.
- Existing test-all logs report both architectures passing the standard gate, but do not prove absence of long-period, concurrency, input or lifetime faults.

## 3. Current-source findings

### KSEC-01 — AMD64 SIMD/FPU context preservation
Status: CONFIRMED SOURCE GAP; critical until fixed or disproved.
Audited AMD64 interrupt/context-switch paths save general-purpose state but do not save/restore XMM/FPU state. Kernel and user binaries contain XMM instructions; build flags do not globally disable SIMD/vectorization. AArch64 exception handling saves/restores q0-q31, FPSR and FPCR. Preemption can corrupt SIMD state across tasks and potentially kernel/user contexts.
Next: map IRQ, syscall, exception, voluntary switch and first-user-entry paths; inspect CR0/CR4/OSFXSR/OSXMMEXCPT and XSAVE/FXSAVE detection; define one policy covering interrupt nesting and kernel SIMD. Add adversarial two-task SIMD tests on AMD64 and regression on AArch64. Do not merely patch arch_cpu_switch_context.

### KSEC-02 — Registry VFS stale-node lifetime
Status: CONFIRMED SOURCE LIFETIME DEFECT; exploitability/mitigation scope to verify.
regfs_open stores a reg_node pointer in vfs_node.id. registry_del can free that node and descendants. An open file object can retain the old VFS node; sys_object_read attempts path refresh but continues with the stored node if the path no longer exists. regfs_read then dereferences stale id. The comment claiming nodes are never freed contradicts registry_del.
Next: remove raw registry-node pointers from persistent VFS handles or add synchronized reference/generation lifetime. Reopen by canonical path must fail closed if deleted; never fall back to stale pointer. Test delete/subtree-delete while open, concurrent read/delete, recreation at same path and close.

### KSEC-03 — AMD64 user-memory copy vs unmap
Status: DOCUMENTED OPEN RISK; current-source revalidation pending.
Hardening plan records a race between uaccess copy and concurrent unmap. Inspect exact copy helpers, page pin/reference mechanism, address-space lock ownership and unmap/free callers before classifying current status. No fix proposed yet.

### KSEC-04 — Scheduler outgoing-stack lifetime across interrupt return
Status: HIGH-CONFIDENCE RACE CANDIDATE; precise interleaving proof and regression required.
schedule changes cpu_ptr->current_task before interrupt dispatch completes EOI/IRET/ERET on the outgoing stack. READY outgoing tasks are protected with pending_reenqueue; SLEEPING/STOPPED tasks are not parked there. process_terminate common tail marks a sleeping task dead and frees it immediately if no CPU current_task scan finds it. There may be a window where current_task changed but the interrupt-return epilogue still uses the old stack.
Do not assign sleepers to pending_reenqueue: __sched_requeue_pending unconditionally enqueues live tasks and would wake sleepers. Likely design: distinct per-CPU outgoing_retire/stack-quarantine slot drained only at next scheduler entry, state-sensitive (READY requeue; SLEEPING/STOPPED stay parked; DEAD/ZOMBIE reap). Inspect voluntary switches, nested IRQs and cross-CPU migration; prove no double parking. Stress terminate vs sleep/wakeup on both architectures with stack poisoning/reuse instrumentation.

### KSEC-05 — nxexec privileged confused-deputy path
Status: CONFIRMED TRUST-BOUNDARY DESIGN DEFECT; impact depends on caller reachability and executable policy.
nxexec --service runs ROOT and publishes OS1nx_exec. OS1_NS_PORT permits SEND access to a published port without per-service ACL. Kernel stamps m.from, but service uses caller PID for ownership/FD redirection, not to constrain child privilege. OS1low_process_spawn_redir runs as ROOT nxexec, so kernel derives child authority from the service.
Next: define kernel-enforced delegated spawn (authenticated caller + explicit privilege ceiling/attenuation + executable policy + FD capability transfer). Never trust payload PID. Preserve spawn ABI via versioned request/capability extension; test USER/GUEST against privileged executables and ensure child rights never exceed caller-authorized ceiling.

### KSEC-06 — Global display configuration authorized by CAP_WINDOW
Status: CONFIRMED AUTHORIZATION MISMATCH.
PLVL_GUEST receives CAP_WINDOW; dispatcher gates SYS_SET_DISPLAY_MODE, SYS_SET_STYLE and SYS_SET_ZOOM with that bit. These operations affect global desktop state, not one window. Style persistence writes Registry entries with system ownership. November ABI matrix identifies distinct display capability, not applied by current gate.
Next: separate per-window rights from global display administration; audit display/style/font operations and test GUEST/USER/ROOT/MACHINE. Preserve legitimate guest window creation/drawing if compatibility requires it.

### KSEC-07 — Handle ABA / no public generation
Status: CONFIRMED DESIGN LIMITATION.
handle_entry stores object pointer and rights; handle_install_locked reuses first free integer slot. Public handles have no generation. A stale integer can alias a subsequently installed object. Existing capability tests do not establish ABA resistance.
Next: evolve existing object manager and ports with versioned/generation-bearing handles. Do not create parallel handle table or IPC. Preserve old ABI through compatibility wrappers; use typed generation-checked tokens for new surface/device APIs.

### KSEC-08 — Process creation rollback and allocation under scheduler lock
Status: CONFIRMED SOURCE DEFECTS; cleanup dependencies need call-graph validation.
process_create_caps allocates a process descriptor page with pmm_alloc_page while holding sched_lock (interrupts disabled), despite lock policy. Textual checker misses it because it treats an error-branch unlock as release on normal path.
__proc_create_stdio_and_pgd does not fail when vmm_create_pgd returns NULL. __proc_create_rollback was observed not to release proc->env or destroy an already-installed handle table. Enumerate initialization stages and ownership transitions before changing cleanup; make rollback idempotent and stage-aware.

## 4. Additional audit queue (not yet classified)

- Runtime W^X: page flags include RX/RO/NX and NXE/CR0.WP setup; still audit identity/direct-map aliases, intermediate page-table permissions, kernel text/data aliases and user mappings. June comments saying W^X is absent are stale where current code contradicts them.
- AArch64 EL1/IRQ frames and SIMD save/restore: correlate SSP faults with complete first-fault frame, interrupted SP, stack bounds and EL0/EL1 classification.
- nxadb health gate: recognize nested EL1 exception signatures; check guest liveness after soak, not only QEMU process liveness; extend soak beyond 345 seconds; preserve full logs.
- IRQ sharing: correlate AMD64 VirtIO-Input IRQ 42 warning with registration, dispatch fanout and device teardown.
- nxadb/nxrun: distinguish build freshness from boot, serialize shell interactions, expose soak duration and GUI/headless modes, preserve full QEMU logs. Avoid helpers bypassing per-architecture lock.
- Keep VFS, Registry, object manager, port namespace and syscall migration aligned with July/Nexs-NG plans. No file moves or ABI changes during this security pass without documented migration step.

## 5. Fix sequence

1. KSEC-04 scheduler outgoing-stack quarantine: review and verify the existing worktree patch first; dual-arch lifetime stress is mandatory because this is the closest source-level candidate for the intermittent AArch64 stack fault.
2. KSEC-01 AMD64 FPU/SIMD preservation: architecture entry/return design, implementation, dual-arch build, SIMD isolation regression. This is a separate confirmed architecture gap; do not conflate it with the AArch64 SSP incident.
3. KSEC-02 Registry/VFS stale-node lifetime: safe provider lifetime design and concurrent delete/read tests.
4. KSEC-05 delegated spawn privilege ceiling: kernel-enforced attenuation and adversarial USER/GUEST tests.
5. KSEC-06 display capability separation: ABI-compatible authorization correction and role matrix.
6. KSEC-08 process creation allocation/rollback: stage-aware unwind and checker control-flow fix.
7. KSEC-03 uaccess/unmap and KSEC-07 handle ABA: complete source audit, evolve the existing object manager.
8. Expanded fault/soak/input/window/process stress on AMD64 and AArch64. Only then resume GFX caller/terminal audit and the userland compositor + nxsurfaceserver split, following existing migration plans.

No downgrade: preserve functionality, both architectures, boot paths, ABI compatibility, performance intent and userland migration direction. Do not disable SIMD globally, remove capabilities wholesale, revert graphics or delete features as a substitute for a secure fix.

## 6. Change control

At audit start the index/worktree already contained substantial staged and unstaged changes (fault instrumentation, input tracing, caller inventory, test logs, and kernel/sched/process.c plus user/bin/coreutils deltas). Preserve them. This report stages/commits no source changes. Each fix must be isolated, built and tested on both architectures, documented with exact logs, then staged only after reconciling index and worktree diffs.

## 7. Audit corrections and source cross-checks

The AMD64 SIMD finding was checked against the current architecture implementation, not only earlier notes:
- kernel/arch/amd64/cpu/cpu.c explicitly enables CR0.MP and CR4.OSFXSR/OSXMMEXCPT and documents that no FXSAVE/XSAVE is done in context-switch or ISR entry.
- kernel/arch/amd64/cpu/isr_stubs.S saves GP registers only; restore_context restores GP registers and IRETQ state only.
- kernel/arch/amd64/cpu/syscall.S constructs a GP-only syscall frame and returns via IRETQ; it does not preserve XMM/FPU state.
- kernel/arch/amd64/cpu/context.S saves callee-saved GP registers for cooperative ctx_switch; it does not save FPU state. The current scheduler path calls arch_cpu_switch_context for address-space/TSS state, not this assembly routine.
- Makefile COMMON_FLAGS and ARCH_CFLAGS do not include -mno-sse/-mno-sse2 or equivalent. The compiler is therefore not contractually prevented from emitting SSE2 instructions.
- kernel/arch/amd64/cpu/idt.c has a stale header line claiming CPU-AMD64-01 is resolved in arch_cpu_switch_context, immediately contradicted by the rest of the same file and cpu.c. Treat the explicit implementation and ISR path as authoritative; fix this documentation inconsistency when implementing the real solution.

This narrows the fix design: the missing state must be preserved on every AMD64 asynchronous entry/return path that can preempt user or kernel code, and on syscall/cooperative paths where the context can be suspended. Saving only around the timer's C scheduler call is insufficient. Any FXSAVE-area addition must maintain 16-byte alignment, define a valid initial image for new tasks, handle nested interrupts, and preserve the existing pt_regs assembly offsets through compile-time layout assertions.

### KSEC-01 historical implementation review (June 19)

The July 17 stratification plan and ASTRA explicitly state that CPU-AMD64-01 landed in commit 184637d and was reverted the same day by a80bbc1 (2026-06-19). The old patch was inspected directly with git show:
- It added a per-process aligned 512-byte FXSAVE area, seeded from a boot-time fninit/fxsave image.
- It saved the outgoing process and restored the incoming process in arch_cpu_switch_context.
- The original commit itself documented the remaining defect: a preempted user task could have XMM state clobbered by compiler-generated kernel SSE between interrupt entry and the late save point. It tracked this as #167.
- The revert removed the field, hook and save/restore. The revert message only says it reverts the commit; it does not explain the technical reason. Do not infer a reason not present in history.

Therefore the old patch is useful evidence and a partial design, but MUST NOT be reapplied unchanged: it does not close the kernel-clobber window and its stated verification (build/boot/demo3d) did not test cross-task SIMD preservation under adversarial preemption. The replacement design must protect state at architecture entry before any compiler-generated kernel code can clobber it, retain nested-interrupt correctness, and seed first-run contexts. The June review suggests eager FXSAVE/FXRSTOR or lazy #NM ownership; evaluate both against the current IRQ/syscall/exception frame model before choosing.

### KSEC-02 Registry/VFS source cross-check

The exact current code path was re-read:
- regfs_open takes reg_read_lock, resolves walk_path(), then stores the raw reg_node address in vfs_node.id and releases the lock.
- registry_del takes reg_write_lock, unlinks and kfree()s a leaf and prunes/kfree()s empty ancestors.
- regfs_read casts node->id back to reg_node * BEFORE taking reg_read_lock, then dereferences n->is_leaf and n->value under the lock. Acquiring the lock after the cast cannot make an already-freed node valid.
- sys_object_read explicitly treats vfs_open(path) refresh failure as best-effort and falls back to the cached node, to preserve the documented open-after-delete behavior. That policy is unsafe for this provider because its node identity is a reclaimable pointer.
- struct vfs_node documents that provider-private id remains valid while the mount remains mounted; root /reg mount is not being unmounted in this scenario, so the provider currently violates its own node-lifetime contract.

A path-only fail-closed change would remove the UAF but would silently change the documented open-after-delete semantics. Prefer a provider lifetime solution (stable refcounted/tombstoned node or equivalent object-owned snapshot) and add a VFS provider release contract if needed. Audit all providers before adding a generic release callback; do not special-case /reg in object.c without documenting why.

## 8. Review of the in-progress scheduler/process patch

The current worktree (not yet fully verified) contains a separate pending_stack_release pointer in cpu_info and corresponding scheduler changes. Source-level review confirms the intended state transitions:
- schedule pins every non-idle prev task after selecting a different next task, not only READY tasks.
- The pin is released at the next schedule() entry, after the prior architecture restore epilogue has installed the selected stack.
- DEAD/ZOMBIE goes to deferred reap; READY is enqueued; SLEEPING/STOPPED remains parked.
- __proc_still_current_anywhere() treats pending_stack_release as live.
- __enqueue_task() suppresses queue insertion while the task remains pinned; this covers wake_up, process_cont, IPC wake and the public enqueue wrapper because those paths acquire the target CPU scheduler lock before calling it.
- The pointer is in cpu_info, statically zero-initialised, and is visible to the existing panic/SSP diagnostics.

The change also moves process descriptor allocation outside sched_lock and snapshots creator PID/ctty under sched_lock before performing compositor/object/VMM work. The helper now checks vmm_create_pgd() failure and rollback destroys installed handles, the environment block and any PGD.

Review caught one compatibility regression in that in-progress patch: it initially made env-block allocation mandatory, while struct process explicitly defines a NULL env as a supported fallback to system defaults. That check was removed; environment allocation remains best-effort, while the process descriptor allocation remains mandatory. This preserves the documented OOM degradation behavior.

Remaining source-level checks before accepting this patch:
- verify every __enqueue_task caller holds the exact target CPU sched_lock and no caller can select a different target CPU while the pin is active;
- verify no other teardown path bypasses __proc_still_current_anywhere() or the deferred reaper;
- verify the new outgoing pin does not conflict with pending requeue/deferred-free diagnostics or stack trace semantics;
- run deterministic sleep/wakeup/stop/continue/kill races and repeated spawn/exit stress on both architectures.

At this point the stack-pin and process-create changes remain unverified worktree changes. Do not stage them until clean dual-architecture builds and guest tests pass. The build attempt that overlapped an earlier background clean/rebuild was stopped to avoid concurrent Make invocations; its log is not a passing build result. A fresh serialized clean build is running separately with dedicated logs.

## 9. Existing scheduler test coverage and gaps

The current boot KTEST runner is single-threaded before SMP starts and runs the registered .ktests section. The audited kernel/lib/ktest_samples.c includes memory, FDT, graphics, VFS mount lifecycle, allocator and VMM tests, but no scheduler outgoing-stack lifetime, cross-CPU work-steal, sleep/wakeup/stop/continue, concurrent terminate, or repeated self-exit regression. A boot KTEST cannot prove this runtime SMP invariant.

user/bin/stress.c has a useful existing spawn lane: it repeatedly spawns /bin/captest (or /bin/crash when fault injection is enabled) and waits for each child. It also has alloc/file/gui lanes and a default 2-hour duration. This is a broad regression driver, not a deterministic sleeping-task race test: it does not specifically hold a child in nanosleep/wait-queue sleep while another process kills or continues it. Reuse the existing stress harness without replacing it, then add a focused scheduler-lifetime test with deterministic barriers/IPC and a documented expected interleaving. Do not rely only on random stress or boot KTEST counts.

## 10. Historical scheduler fixes that must not regress

The June 11 scheduler addenda and ASTRA §7.9–7.10 were re-read before accepting the current stack patch. They document already-landed fixes, not open work to reimplement:
- SCHED-UAF-01: per-CPU PGD adoption and deferred resource freeing after a CPU switches to a different address space.
- SCHED-UAF-02: local IRQ masking around the per-CPU deferred-free drain, preventing nested schedule() from double-draining the same corpse.
- SCHED-UAF-03: panic instead of returning the just-reaped prev frame if the idle fallback is unexpectedly absent.
- SCHED-IRQ-01: schedule() masks IRQs at its own entry and has explicit no-switch vs switch IRQ restore contracts.
- #169/#170: READY outgoing tasks are not made stealable until the next schedule() on the owning CPU; STACK-ALIAS guard detects shared process kernel stacks.
- SCHED-STACK-ISO: deep compositor rendering was removed from timer IRQ context; do not reintroduce render-on-IRQ or a bespoke kernel render stack.

KSEC-04 is NOT a proposal to undo or replace those fixes. It addresses a remaining lifecycle distinction: the previous pending_reenqueue protected READY tasks, while the new pending_stack_release aims to keep every outgoing non-idle stack alive through the IRQ/exception epilogue, including tasks that became SLEEPING/STOPPED. The existing single-owner deferred reaper and all IRQ/PGD invariants above must remain intact. The June docs also record a historical rapid-close __dequeue_task UAF; the new test plan must include rapid window/process termination to ensure the expanded pin does not regress that path.
