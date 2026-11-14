# Session 2 Issues

## S2-ISSUE-001 — Coreutils portability suite remains 42/50

Status: **deferred, no regression**.

Failures remain:
- `readlink`
- `users`
- `who`
- `nice`
- `nohup`
- `chmod`
- `chown`
- `chgrp`

The boot runner remains 18/19 solely because of this existing 42/50 test.
Repairs belong in SDK/userland portability and the canonical execution model,
not in new POSIX kernel policy.

A real Gnulib FTS implementation is explicitly deferred. The current SDK
portability layer returns `ENOSYS` from its temporary FTS stub instead of
pretending traversal works. The next portability gate must complete and test
the fd-relative directory model (`dirfd`, `fdopendir`, `fchdir`,
fd-relative `openat` semantics) before replacing the stub.

## S2-ISSUE-002 — External-source warning debt

First-party SDK/USERKIT/NexsOS1-main warnings found during Session 2 were
repaired under strict compilation. Imported vendor sources still produce
warnings:
- AArch64: 558 warning lines / 123 files.
- AMD64: 517 warning lines / 125 files.

The external compiler wrapper keeps successful vendor diagnostics out of normal
first-party output while retaining per-object logs and a bounded summary.
Future work should reduce these warnings through portability boundaries where
possible, without editing imported repositories or suppressing first-party
warnings.

## S2-ISSUE-003 — AMD64 BIOS preferred display mode

Status: **diagnosed, implementation decision pending**.

AMD64 BIOS boots VirtIO-GPU with a 640x480 current scanout. The driver currently
uses `GET_DISPLAY_INFO` as the native-mode source, so the compositor initializes
at 640x480. AArch64 UEFI already arrives with 1280x800.

Runtime mode setting is healthy:
- 1280x800 manual mode PASS.
- 200% zoom -> 640x400 PASS.
- 100% zoom -> 1280x800 PASS.
- light/dark themes at 1280x800 PASS.

The driver already has a real `set_mode` path. The proposed hardware-driven
follow-up is VirtIO-GPU EDID/preferred-mode discovery with validated fallback to
the current display-info path. No Make/QEMU resolution hardcode should be used.

## S2-ISSUE-004 — Runtime/UART observability quality

Status: **post-commit work**.

Several subsystems still emit verbose or low-value logs while important state
transitions require screenshots to infer behavior. Future hardening should make
UART diagnostics concise and semantically useful, especially for device mode
selection, ownership transitions, faults and service supervision.

This work must improve real instrumentation rather than add decorative prints.
Display work may add precise mode-selection logs when that subsystem is changed.

## S2-ISSUE-005 — Full Power12 qualification

Status: **next hardening phase**.

Full-project Power12 remains reporting/non-blocking. Before stricter activation,
the test methodology and missing suites must be reviewed. Strict qualification
then progresses in order:
1. HAL
2. kernel

Runtime behavior must never become blocking merely because compiler/audit
policy is strict.

## S2-ISSUE-006 — Documentation migration verifier baseline

The Session 1 verifier used current `HEAD` as the source of pre-migration
documents. After migration this can create false failures. The preservation
check should eventually use the actual pre-migration commit as its immutable
baseline.

## S2-ISSUE-007 — Cross-process redirection lifetime

Status: **next hardening gate**.

Cross-process redirection still reaches another process through
`process_find_by_pid()` without an explicit lifetime pin for the returned
reference. The next security review must determine whether the current process
lifetime model already guarantees safety or whether a reference/pin mechanism is
required. No speculative lifetime API is introduced by this closure.

## S2-ISSUE-008 — Ext4 truncate reclaim and zero-on-regrow

Status: **next hardening gate**.

The current truncate path supports logical shrink/grow behavior but requires a
complete audit of physical block reclaim, zero-on-regrow, inode preservation,
read-only/directory rejection, size bounds, and failure recovery. Existing code
and tests must be reviewed before any reimplementation.

## S2-ISSUE-009 — Boot readiness versus arbitrary delays

Status: **next hardening gate**.

Finite watchdog and polling bounds are valid safety mechanisms, but arbitrary
sleep-based dependencies are not accepted as readiness proofs. In particular,
boot/service sequencing must migrate remaining dependency sleeps to explicit
ACK, registry-ready, event, or equivalent completion signals while retaining
bounded watchdogs.

## S2-ISSUE-010 — Keyboard catalog coverage

Status: **next hardening gate**.

The existing keyboard layout/keymap catalog must be extended rather than
replaced. Scope is catalog/layout and standard key-code translation coverage;
hardware controller rewrites are explicitly out of scope for this item.

## S2-ISSUE-011 — Multi-output display/resource model

Status: **design/review required before implementation**.

The current display path still assumes a primary VirtIO scanout in places and
does not yet expose the complete hardware-generic adapter/output model required
for multiple connected displays, hotplug, mode lists, clone/extended policy, and
per-output scale/position.

The target boundary remains:
- kernel: hardware/capability mechanism plus surfaces/buffers;
- userland: compositor, scene/layout, clone/extended/primary policy and UI.

Existing HAL/resource/device code and migration documentation must be reviewed
before implementation. NetBSD may be added as a driver-model reference, but
existing NexsOS mechanisms must be extended rather than duplicated.
