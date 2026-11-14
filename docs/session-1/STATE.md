# Session 1 State — 2026-11-14

## Repository state

Active umbrella branch: `DEV-Resurrection`.

Current repository heads at this checkpoint:
- umbrella: `5a1264c310b0`
- NX-CORE: `ffba39569f37`
- NX-SDK: `b05e720f6bcd`
- NX-USERKIT: `cb68eb85cf7f`

All four worktrees are currently clean relative to their indexes: zero tracked unstaged changes and zero untracked files before these Session 1 documents are added.

Staged file counts at the checkpoint before these Session 1 documents:
- umbrella: 337
- NX-CORE: 14
- NX-SDK: 19
- NX-USERKIT: 2

No runtime QEMU session is intentionally left running at this checkpoint.

## Recovery evidence

The pre-clean recovery archive is stored under the umbrella Git metadata:
- `.git/session1-recovery/2026-11-14-session1-preclean.zip`
- SHA-256: `585cc2934c7fceebe188085faa6d06c2c6d0aae6ff47dde7ad28147c24352817`

It preserves tracked unstaged patches, staged-reference patches, index baselines, working-tree copies, untracked files, manifests and checksums for all four repositories.

A verified recovery copy of the AArch64 VirtIO input DMA fix is stored in:
- `.git/session1-recovery/verified-input-dma/verified-input-dma.patch`
- SHA-256: `ab175596905dc139acb5b0f9c126f4b420aa3eb500a97f5cd5ca339258d441e3`

## Verified staged corrections added in Session 1

### VirtIO input split-ring cache maintenance

NX-CORE now stages a complete split-ring span calculation and uses it for VirtIO input DMA cache maintenance. The previous interrupt path invalidated only `sizeof(struct vring_used)`, which is four bytes before the flexible array. With queue size 16 the used ring is 134 bytes and crosses multiple 64-byte cache lines.

The staged task includes:
- `kernel/include/drivers/virtio.h`
- `kernel/drivers/virtio/virtio_input.c`
- `kernel/test/virtio_ring_layout_test.c`
- `kernel/Makefile`

The test is integrated into NX-CORE `check`.

### NX-CORE contract snapshot alignment

NX-CORE ABI revision is staged as r2 and its ABI snapshot is synchronized with the already-staged, byte-identical NX-SDK contract headers:
- `object.h`
- `posix_types.h`

NX-CORE `check-contracts` passes for amd64 and aarch64 after this update.

### Governance

`/README-LLM.md` was restored byte-for-byte from the recovery archive and staged. Its SHA-256 is:
`286e10684aa12e296ffbaa1163bcd61384a56e645f7f48d064838df8cb25f04f`.

## Active documentation baseline

The Session 1 documentation split is now implemented and verified:

- `docs/` contains only `README.md`, `session-1/` and `legacy/`;
- 169 pre-Session-1 tracked documentation files are preserved under
  `docs/legacy/` byte-for-byte against their original `HEAD` blobs;
- `.presstoai.json` project-document paths resolve to the migrated namespace;
- current root/project references to moved documents use `docs/legacy/...`;
- `tools/test_docs_layout.py` verifies preservation, namespace shape, index
  paths and active Markdown links;
- active governance now includes methodology, architecture ownership,
  reference policy, ordered plan, state, issues and verification records.

Execution order is now explicit: documentation baseline, GPU/display/device
safety, project-wide Power12 qualification/upgrade, then compositor and wider
subsystem migrations.

A pre-existing documentation defect remains outside the migration:
`tools/rea/README.md` names `docs/primary/TOOLS_REA.md`, which did not exist
in the pre-migration `HEAD`. The file was left unchanged rather than inventing
a target.

## GPU/device safety phase

The first bounded generic PCI safety block is implemented and verified at its
declared scope:

- shared PCI enumeration enables address decode but explicitly clears
  BusMaster;
- BusMaster grant/revoke is separate from discovery;
- generic driver matching uses a provisional claim before DMA authority;
- probe failure revokes DMA before fallback; failed revoke quarantines;
- xHCI/EHCI/UHCI explicitly declare DMA requirements;
- focused host/sanitizer/cross-compile tests pass;
- scoped Power12 reports zero findings;
- clean AArch64 and AMD64 graphical boots reach the userland TTY with zero
  kernel faults;
- at least two nxadb commands execute on each architecture and screenshots are
  verified at 1280x800.

This does NOT complete PCI safety: AMD64 still has an independent legacy
`command | 7` path and VirtIO PCI still lacks explicit acquire/release DMA
ownership. See `GPU-DEVICE-CENSUS.md`.

A real AArch64 input-event loss was reproduced separately: a complete command
reached the TTY while the final Enter event was lost. The standard initial
nxadb shell-health probe is also intermittently unreliable on both
architectures. These are open blockers for the input/device phase, not evidence
against the focused PCI claim logic.

## Graphics/input architecture observed

The present tree is transitional, not the target architecture:
- the kernel still initializes and owns the legacy compositor/window table;
- `nxcompositor` starts later in userland and snapshots/renders that kernel-owned scene;
- `nxkeyboardd` uses the userland SDK keyboard layout implementation but selects the focused recipient through `OS1_window_enum()`;
- that enumeration still derives focus/window state from the kernel compositor;
- `nxttyd` owns the graphical terminal window and forwards input to its hosted child.

Therefore the userland compositor is not yet the authoritative scene/window/focus owner.

## Execution, TTY and boot-test checkpoint

The execution/terminal model is now separated by responsibility rather than by
caller:

- `execsvc_launch()` exposes only base foreground vs detached process
  ownership; hosted TTY execution is a separate `execsvc_launch_hosted()`
  composition;
- `nxttyd` owns TTY/CLI hosting and the capability-backed `NX_TTY_PORT`
  stream/history; `nxjobs` owns shell job-control state over process
  capabilities;
- graphical children launched from an interactive shell retain the parent TTY
  for stdout/stderr while pixel rendering remains isolated from terminal text;
- graphical children launched through hosted execution keep a hidden
  `nxttyd` diagnostic TTY/PORT/history instead of exposing an unwanted
  terminal window;
- terminal children launched without a ctty receive a visible hosted TTY and
  retain the existing press-Enter-to-close lifecycle;
- normal boot starts `nxtestrunner` asynchronously inside its own visible TTY;
  the runner retains all prior case IDs and appends
  `18nxtest_timerprobe`.

Final clean AArch64 verification for this checkpoint reports
`BOOT-NXTEST=DONE`, shell interaction/history PASS, TIMERDIAG PASS,
counter-heartbeat PASS, `KTEST=37/0`, and zero kernel faults.  The boot suite
is 18/19 solely because the pre-existing coreutils test remains 42/50.  A
runtime job-control probe verified background launch, `jobs`, foregrounding
and reap.  Ctrl-Z/bg remains a separate end-to-end test.

The changed NX-SDK core and `libctest` consumer compile for both AArch64 and
AMD64.  A fresh AMD64 runtime pass of the final execution/TTY checkpoint is
still pending and is not claimed by this checkpoint.

## Version discrepancy

The project-owner directive records the current project version as `0.0.6.2`.
The root Makefile currently declares `VERSION ?= V0.0.6.3`.
This discrepancy is catalogued and must not be silently normalized without an explicit version-state decision.
