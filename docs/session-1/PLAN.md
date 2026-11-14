# Session 1 Development Plan

This plan is assistant-authored and subordinate to `/README-LLM.md`.
`OBJECTIVES.md` is the compact status/acceptance index for the objectives in this plan.
The owner-defined execution order is:

1. complete and verify documentation;
2. make GPU/display/device management safe and observable;
3. begin project-wide Power12 qualification and architecture upgrade;
4. continue the compositor/userland and broader subsystem migrations in bounded blocks.

## Mandatory task protocol

Every block follows `METHODOLOGY.md`:

1. one bounded objective;
2. read relevant files and documents in full;
3. catalogue current behavior/gaps;
4. implement through the canonical mechanism;
5. add tests that exercise the real failure/behavior boundary;
6. run focused gates;
7. run clean dual-architecture integration/runtime gates when affected;
8. inspect logs;
9. update Session 1 documentation;
10. stage only the verified block.

## Phase 0 — Documentation and reproducible baseline

### 0A. Recovery and staged-baseline audit

Status: completed enough to proceed, with unresolved findings retained.

- prior unstaged work preserved under `.git/session1-recovery/`;
- staged baseline audited;
- AArch64 and AMD64 clean builds/boots exercised;
- contract and Power12 failures catalogued;
- VirtIO input DMA task separately verified/staged;
- no full-project Power12 claim.

### 0B. Active/legacy documentation split

Status: in progress.

Acceptance:
- `docs/session-1/` is the active work record;
- `docs/legacy/` contains all pre-Session-1 documentation without loss;
- rooted project references are migrated;
- PressToAI index paths are valid;
- a documentation layout test proves the migration;
- no invented target is created for a pre-existing broken link.

### 0C. Active architecture/governance set

Required active documents:
- owner directive: `/README-LLM.md`;
- methodology;
- architecture ownership boundaries;
- approved reference policy;
- plan;
- state;
- issues;
- verification;
- legacy migration record.

Acceptance: documents agree on execution order and do not claim unverified
behavior.

### 0D. Optional study references

After the documentation block is staged:
- select exact upstream Linux, seL4 and BSD repositories/revisions;
- add them below `references/` as optional submodules;
- normal setup/build must not initialize them by default;
- record license, revision and study purpose;
- verify a clean normal setup/build does not require them.

NXOLMO remains a separate project-owned reference/source candidate and is not
made an implicit build dependency.

## Phase 1 — GPU, display and device safety

This phase hardens the mechanisms before larger architecture migration.

### 1A. Complete source/contract census

Read in full and map:
- GPU core and every GPU provider;
- VirtIO transport/ring helpers;
- display resource/presentation path;
- framebuffer/boot framebuffer;
- PCI/MMIO/IRQ/DMA discovery and assignment;
- FDT/ACPI/platform discovery;
- device registry and driver matching;
- input devices where they share the same lifecycle mechanisms;
- callers in kernel, SDK, USERKIT and integration project;
- all existing tests.

Produce a live ownership/lifetime table before editing.

### 1B. Device lifetime and DMA safety

Required invariants:
- exact resource extents, no fixed fake BAR/map sizes;
- no unconditional BusMaster/DMA enable before validated driver ownership;
- queue cache-maintenance spans cover complete rings/descriptors/data;
- device handles have explicit generation/lifetime/ref ownership;
- unregister/detach quiesces IRQ, DMA and in-flight work before free;
- bounded waits and explicit timeout/quarantine;
- no QEMU-only assumptions in generic code.

Each discovered violation gets a focused regression test before or with its fix.

### 1C. Display/GPU model safety

Keep three states separate:
- physical output/scanout and supported device modes;
- compositor workspace/render resolution and scale;
- per-application logical surface/window geometry.

Requirements:
- hardware modes discovered from provider/device;
- explicit mode-setting remains supported;
- no hard-coded scale masquerading as a device mode;
- framebuffer ownership and scene/window ownership are not conflated;
- emergency/panic output remains independent;
- kernel does not absorb SDK runtime code.

### 1D. Architecture gates

For each verified GPU/device block:
- host/unit tests on actual implementation helpers;
- Power12 changed-scope;
- `make clean`;
- AArch64 graphical boot + nxadb suite + at least two commands;
- AMD64 graphical boot + nxadb suite + at least two commands;
- screenshot/input/device evidence where relevant;
- build/runtime warning catalogue.

Stage each block independently.

## Phase 2 — Power12 qualification and architecture upgrade

The full strict gate is currently red and remains the authority.

### 2A. Freeze the failure inventory

Regenerate the full strict findings and classify them by rule, subsystem and
ownership. Keep baseline and changed-scope evidence separate.

### 2B. Remove forbidden alternate artifacts

Resolve R10 stale/alternate sources only after content comparison proves the
canonical implementation. No blind deletion.

### 2C. ID/limit provenance

Resolve R11/R12-style hard-coded identifiers, sizes and bounds by tracing each
value to a real contract/provider/configuration. Do not merely move magic
numbers into another header.

### 2D. AST findings in bounded batches

Process a small subsystem/file set at a time:
- no unbounded loops;
- no forbidden goto/assert/static-assert production shortcuts;
- bounded allocation/work;
- short analyzable functions;
- checked results;
- explicit concurrency/lifetime rules.

Every rewritten kernel/HAL function receives real tests appropriate to its
behavior and boundary.

### 2E. Upgrade gate

A block is staged only after:
- focused tests;
- changed-scope Power12 zero findings;
- both architecture builds;
- runtime acceptance when behavior is affected.

Full project qualification is claimed only when `make power12-strict` passes.

## Phase 3 — Authoritative userland compositor

Only after the GPU/device substrate is safe enough to support the migration.

### 3A. Ownership census

Map every caller/function in `kernel/graphics` and classify:
hardware mechanism, surface/buffer mechanism, scene policy, rendering/theme,
compatibility bridge or dead code.

### 3B. Userland scene authority

`nxcompositor` becomes the single owner of:
window registry, focus, z-order, virtual geometry, decoration/theme/effects,
composition and presentation policy.

Do not create a second scene table.

### 3C. Buffer/surface ownership

Applications own backing buffers through SDK/resource contracts.
Kernel retains capability-checked buffer/surface lifetime and presentation
mechanisms only.

### 3D. Input/focus integration

`nxkeyboardd` and `nxmoused` route through the authoritative userland scene
model, not kernel compositor enumeration.

### 3E. Remove legacy kernel graphics policy

Delete kernel graphics policy/rendering only after every live function has a
tested destination and both architecture graphical gates pass.

## Phase 4 — SDK/userland stratification

Inventory first, then separate without duplication:
- userland primitive/sandbox layer;
- system libc;
- portability;
- extensions;
- UserKit;
- DriverKit.

POSIX remains outside the kernel. UserKit exposes service clients; DriverKit
exposes driver contracts and driver-sandbox primitives.

## Phase 5 — DriverKit and userland drivers

Design the driver sandbox and lifecycle first. Then migrate ordinary drivers in
small projects, beginning with non-boot-critical devices. Linux is a reference
for contracts/lifecycle, not a required source dependency.

## Phase 6 — VFS/storage/images

Study current code plus NXOLMO before implementation.
Move VFS/filesystem/driver policy into userland in bounded stages while
preserving installer/demo/full-system image behavior and real concurrency
coverage.

## Release direction

The owner baseline is 0.0.6.2 while the current root Makefile reports V0.0.6.3.
The discrepancy remains explicit. The development objective is a reproducible,
verified path to 0.1.0.0; version numbers do not advance merely because a plan
exists.
