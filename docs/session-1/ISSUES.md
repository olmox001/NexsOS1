# Session 1 Open Issues

Severity here reflects development blocking impact, not a release certification.

## Blocking / high priority

### S1-GFX-001 — Dual compositor/window ownership

The kernel still owns window state, focus and compatibility pixel buffers while the userland `nxcompositor` consumes snapshots and presents a second composition layer.

Required direction:
- kernel retains hardware/resource/capability/surface mechanisms only;
- scene, focus, window policy, decorations, shadows, buttons, effects, themes and virtual resize policy move to the replaceable userland compositor service;
- per-application/userland buffers are not to be recreated as kernel compositor buffers.

### S1-GFX-002 — Hardware display resize and virtual window resize are conflated

The current transition must preserve separate concepts:
- physical/scanout mode and supported hardware resolutions discovered from the actual display/GPU device;
- virtual window/surface geometry managed by userland;
- optional display scale/zoom as a policy state, not a hard-coded fake resolution.

Hard-coded QEMU/device assumptions are not acceptable as the driver model.

### S1-INPUT-001 — First input after graphical boot is not deterministic

Observed after otherwise healthy boots:
- an early AArch64 health probe could disappear;
- one AMD64 first `pwd` arrived as `ppwd`;
- after the system settled, repeated QMP commands were correct on both architectures.

This remains open. It must not be hidden by an arbitrary sleep in the test harness.

Relevant architecture facts:
- `nxttyd` publishes/focuses its window before spawning the shell;
- IPC queue capacity is 256 messages, so the initial keyboard burst does not overflow it;
- `keyboard_focus_pid` is a plain lockless shared integer; its comments assume integer access alone provides sufficient atomicity. That is not a complete C concurrency guarantee.
- focus ownership is expected to move out of the legacy kernel compositor, so any interim focus hardening must not create a second permanent model.

### S1-INPUT-002 — nxadb window-health expectations are stale

Some nxadb checks still expect the focused window title to match `nxshell`.
The current hosted terminal correctly appears as `/sys/services/nxttyd`.
Those checks can produce `NXWINS-FAIL` / `GUEST-HEALTH-FAIL` even while the actual focused TTY is present.

This is a test-harness defect and must be corrected separately from guest input defects.

### S1-CONTRACT-001 — NX-SDK historical revision gate failure

Current staged NX-SDK contract snapshots/revisions are internally updated, but `system/tools/check-contract-snapshot.py` also validates the already-committed previous HEAD transition. That previous HEAD changed USERKIT public headers without a revision bump (r1 -> r1), so the cross-repository gate still fails on history.

Do not weaken the checker. Resolve by preserving the corrected revision in the next valid commit and determining the intended policy for historical-invalid HEAD state.

### S1-POWER12-001 — Full-project Power12 qualification fails

`make power12-strict` is not passing.

Verified findings include:
- 3 stale `.old` source artifacts rejected by R10;
- 375 R11 ID/limit provenance findings;
- 5143 AST findings across the audited project.

The mutation tests for the verifier pass, and scoped changed-gates can pass, but that must never be reported as full-project Power12 compliance.

## Medium priority / catalogued debt

### S1-KERNEL-001 — POSIX/syscall and compositor legacy remain in kernel

`kernel/core/syscall_dispatch.c` still contains VFS/POSIX/window/compositor syscall-era implementation and policy. This is a migration baseline, not the target object-only architecture.

### S1-KERNEL-002 — Registry management remains kernel-visible

`kernel/include/kernel/registry.h` still exposes kernel registry management operations. Registry sets/policy must ultimately be selected by the system/project, not hard-coded as kernel policy.

### S1-DOC-002 — Pre-existing REA documentation target is missing

`tools/rea/README.md` references `docs/primary/TOOLS_REA.md`. That path did
not exist in the pre-migration `HEAD`, so the legacy migration cannot map it
to a real historical file. The reference is intentionally left unchanged until
the actual provider-version source is located or reconstructed from evidence.
Do not create a placeholder merely to make a link check pass.

### S1-REF-001 — Optional Linux/seL4/BSD references not yet added

Approved reference repositories must be placed under `references/` as optional Git submodules and must not become default setup/build dependencies.

### S1-RECOVERY-001 — late-unignored `icon 2.zip`

During the initial worktree reset, reverting `.gitignore` exposed `user/NexsOS1-main/home/Pictures/icon 2.zip` only after the initial untracked manifest had been captured. It was removed by the cleanup and no second indexed copy was found. Existing `icon.zip` must not be assumed identical.

### S1-VERSION-001 — 0.0.6.2 vs V0.0.6.3

Owner directive and root Makefile disagree. Resolve explicitly before release/version work.

### S1-DEV-001 — AMD64 PCI path still grants BusMaster during enumeration

The shared PCI core now clears BusMaster at discovery, but
`kernel/HAL/arch/amd64/hal.c` independently retains `command | 7`.
VirtIO PCI therefore still inherits DMA authority before explicit driver claim
on AMD64. Next bounded device-safety block must replace this duplicate and add
explicit VirtIO DMA acquire/release plus failure cleanup.

### S1-DEV-002 — GPU registry raw-pointer lifetime

`gpu_get_primary()` returns an unreferenced raw pointer while unregister has
no demonstrated quiesce/ref-drain/destroy protocol. Kernel graphics directly
consumes this pointer, so the fix cannot be a local wrapper-only change.

### S1-DEV-003 — Fixed PCI MMIO mapping sizes

xHCI/EHCI currently use fixed mapping extents even though BAR size information
is available. Replace with validated resource extents and test truncation/
overflow cases.

### S1-INPUT-001 — Intermittent guest input event loss/readiness failure

AArch64 reproduced a complete command whose final Enter event never reached the
TTY. A separately injected Return immediately executed the pending command.
The standard nxadb initial shell-health probe also fails intermittently on both
architectures while later commands succeed. Add an end-to-end regression test
that proves key press/release/SYN delivery and shell readiness instead of
masking the defect with retries.

### S1-TOOL-001 — QEMU runner can orphan a child holding disk.img

Terminating the project QEMU wrapper left the QEMU child alive and holding the
disk-image write lock, causing the next nxadb build/boot to fail until the
orphan was killed. Runner process-tree cleanup needs a regression test.

### S1-HARDEN-001 — AMD64 linked object implies executable stack

The AMD64 build reports `system/lib/malloc.o` missing `.note.GNU-stack`,
which implies an executable stack. Track as a hardening defect for the Power12/
upgrade phase.

### S1-CONC-001 — Session plan drift detected during verification

An unstaged concurrent edit changed the documented owner target from 0.1.0.0 to
0.0.6.4. It was not part of this task, contradicted the owner directive and was
restored to the already-staged 0.1.0.0 plan. Future drift must be detected
before staging.

### S1-USER-001 — Coreutils runtime case remains 42/50

The boot regression runner is now visible and automatic, exposing the existing
`04nxtest_coreutils` result consistently: 42/50 with failures in readlink,
users, who, nice, nohup, chmod, chown and chgrp.  The identical 42/50 state is
preserved in 2026-11-12 legacy evidence, so this is not attributed to the
execution/TTY checkpoint.

### S1-JOB-001 — Ctrl-Z/bg end-to-end job-control probe still missing

Runtime verification proves background launch, `jobs`, `fg`, foreground
wait and reap.  The user-facing Ctrl-Z -> stopped -> `bg` path is implemented
through PROCESS STOP/CONT capabilities but was not successfully driven by the
current QMP harness.  Add a deterministic key-chord/job-state regression test;
do not claim this branch verified from source inspection alone.

### S1-VERIFY-001 — Final AMD64 execution/TTY runtime refresh pending

The final execution/TTY changes compile through NX-SDK and the affected
userland consumer on AMD64, and earlier AMD64 graphical boots passed before the
last model refinement.  A fresh AMD64 runtime gate of the exact final tree is
still required.  This checkpoint records that limitation explicitly rather
than delaying the requested staged save.

## Resolved in Session 1

### S1-DOC-001 — Active/legacy documentation split

Resolved with a path census and verified migration. All 169 pre-Session-1
tracked documentation files are preserved byte-for-byte under
`docs/legacy/`; Session 1 is the active namespace, current project references
and the PressToAI index were migrated, and `tools/test_docs_layout.py` passes.
