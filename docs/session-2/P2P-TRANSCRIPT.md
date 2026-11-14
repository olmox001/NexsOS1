# Session 2 P2P Operational Transcript

This document is the English operational transcript for Session 2. It is a
translated decision/evidence record keyed to P2P message ranges. The exact raw
message bodies remain authoritative in `.git/p2p-dc/history.jsonl`.

Current documentation checkpoint: P2P message **#313**.

## #78–#112 — Kernel/HAL compiler-wall hardening

- Recovery was performed from the persistent P2P journal, Desktop Commander
  history, and Git state before editing.
- Kernel/HAL warning policy was hardened so enabled warning classes, including
  conversion/sign-conversion, are fatal at compile time through the common
  warnings-as-errors policy. No runtime panic/blocking behavior was tied to
  compiler diagnostics.
- AArch64 and AMD64 warning findings were repaired semantically across ext4,
  libc/string formatting, scheduler/ELF, graphics arithmetic, allocation, and
  AMD64 HAL-specific IDT/MMU/Multiboot/PIC paths.
- Strict kernel+HAL builds passed on both architectures.
- HAL standalone checks exposed gate bugs involving BUILD_ROOT and external GRUB
  classification; the generic HAL/Core architecture-leak audit was separated
  from the owned GRUB quality gate.
- A temporary Power12-off switch was proposed, rejected by the owner, removed,
  and the normal Power12 behavior was restored.
- Dual-architecture runtime remained mandatory before staging.

## #113–#177 — Verified kernel checkpoint, SDK/USERKIT hardening, and external boundaries

- Kernel/HAL strict runtime qualification completed and the kernel hardening
  checkpoint was committed as `fab9d33`.
- The hardening wall was then extended progressively to NX-SDK, NX-USERKIT, and
  NexsOS1-main.
- External-source warning handling was separated from first-party diagnostics:
  vendor warnings are captured and summarized, while first-party portability
  code remains strict.
- STB was reclassified under the SDK external source tree without weakening
  first-party checks.
- SDL, Lua, Gnulib, Coreutils, and related portability layers were audited and
  repaired for type/range/overflow issues.
- External user programs were separated by role under `user/BINEXTERNAL`,
  while external libraries remain under `user/LIBEXTERNAL`.
- The owner clarified that external repositories remain clean imports; NexsOS
  glue, patches, and implementation logic belong in portability layers.
- P2P display was improved so agent messages render red and owner messages green.
- Icon catalogs were audited for dark/light parity and missing assets.
- The shell dock icon bug was traced to `nxttyd` owning the window while its
  executable identity classified as unknown; title fallback was restored only
  when canonical identity has no icon.

## #178–#223 — SDK portability ownership, FTS boundary, NXPaX checks, and full userland builds

- Gnulib common portability ownership was corrected: common Gnulib adaptation
  belongs to NX-SDK portability, not NexsOS1-main.
- BINEXTERNAL and LIBEXTERNAL catalogs were kept as integration metadata; vendor
  trees remain untouched.
- SDK/userland header separation was audited so userland does not accidentally
  consume kernel-private headers.
- The owner required catalog/init checks for NexsOS1-main and demo1.
- NXPaX was verified to have no accidental disable flag. Core and stress tests
  passed, including million-event and concurrent stress runs.
- A real Gnulib FTS implementation was found to require a broader fd-relative
  directory model. The owner deferred that implementation to a later gate.
  Session 2 therefore uses an explicit ENOSYS FTS stub instead of pretending
  support.
- Coreutils remained at the known 42/50 runtime baseline; this was accepted for
  this checkpoint without lowering expectations.
- Integrated userland builds passed on AArch64 and AMD64. External warning
  summaries were recorded separately from first-party strict gates.

## #224–#247 — Freeze, release metadata, display diagnosis, and EDID prototype

- The owner requested an immediate coherent freeze after dual-architecture
  runtime qualification, followed by release metadata/docs as a second closure
  block.
- AArch64 runtime passed the full nxadb display/shell/history/timer/counter gate,
  KTEST 38/0, faults=0. Coreutils was the only boot-runner failure at 42/50.
- AMD64 runtime passed the same gate with KTEST 37/0 and faults=0.
- The complete verified pre-display phase was staged across the active repos.
- AMD64 initial display booted at 640x480 while AArch64 came up at 1280x800.
  Runtime `nxres 1280 800`, zoom 200/100, and light/dark themes all behaved
  coherently, proving the defect was initial native-mode selection rather than
  resize/theme corruption.
- A minimal EDID preferred-mode implementation was designed using the existing
  VirtIO transport, with no new syscall/ABI, and passed a host parser test plus
  strict kernel builds on both architectures.
- The owner stopped QEMU before the fresh post-EDID runtime test completed, so
  EDID remained post-freeze/unqualified work.

## #248–#269 — Security backlog and regression triage

- The owner recorded a lifetime-risk audit item: cross-process redirection still
  uses `process_find_by_pid()` without an explicit lifetime pin.
- ext4 truncate behavior was queued for a real audit: logical shrink, physical
  block reclaim, zero-on-regrow, inode preservation, read-only/directory/range
  behavior, and real tests.
- A fresh AMD64 run produced one timerqueue failure; it was treated as a
  regression until isolated.
- A later clean run passed the timer path, showing the failure was not accepted
  silently.
- Boot timing was classified into two categories:
  - finite watchdog/poll bounds, which remain legitimate safety bounds;
  - arbitrary dependency sleeps, which must become explicit readiness/ACK/event
    checks.
- The owner expanded the remaining Session 2 audit list to Coreutils completion,
  boot readiness, keyboard catalog coverage, lifetime checks, ext4 truncate,
  test-method review, global hardening review, and final dual-architecture
  qualification.

## #270–#303 — Keyboard scope, multi-output display architecture, and graphics migration requirements

- Keyboard scope was clarified as catalog/keymap and standard code translation
  coverage, not a rewrite of hardware keyboard controllers.
- AMD64 was confirmed to remain visually zoomed because the guest still selected
  a 640x480 initial VirtIO scanout.
- QEMU was found to expose both an explicit VirtIO GPU and an implicit VGA
  device. The owner required correct handling of multiple displays rather than
  hiding one device.
- Multi-display requirements were clarified:
  - hotplug and active/connected state;
  - adapter/output distinction;
  - CPU/software-scanout versus GPU-backed rendering distinction;
  - default clone behavior, selectable primary/extended/clone arrangements;
  - per-output scale/resolution/position controlled from userland;
  - hardware-generic architecture suitable for real hardware, with VirtIO only
    one provider.
- The owner reiterated the documented compositor migration boundary:
  - kernel keeps hardware mechanisms, capabilities, surfaces and buffers;
  - userland owns compositor/scene/layout/scale/clone/extended policy and UI.
- NetBSD was requested as an additional driver-model reference, but the owner
  explicitly warned not to reinvent existing project architecture.
- Additional future work was recorded for DPI/font ownership, a minimal
  emergency kernel display/debug seam coordinated with UART/NXPaX, and a boot
  logo/animation handoff to userland. These are design tasks requiring review,
  not implicit permission for a large rewrite.

## #304–#313 — Verified freeze push and release closure authorization

- Before pushing, the staged freeze was re-audited to ensure post-freeze EDID,
  docs/version, and later fixes were not mixed into the verified code
  checkpoint.
- NX-SDK, NX-USERKIT, and NX-CORE staged subsets passed staged diff checks and
  matched their remote branch heads.
- The verified nested-repository freeze was committed and pushed in dependency
  order:
  - NX-SDK hardening commit: `e9895c6`;
  - NX-USERKIT hardening commit: `116fbfb`;
  - NX-CORE machine-extension hardening commit: `47096a9`.
- The owner authorized release documentation and version metadata to join the
  umbrella closure push.
- NX-SDK release metadata was then bumped and pushed as `534aa0a`.
- Post-freeze EDID/resize and other unqualified code remains outside this
  closure commit and must pass its own full verification before being staged.

## Active work after this documentation checkpoint

The next verified-work cycle is not implicitly approved as one monolithic
rewrite. Each substantial design change must be discussed before implementation.
The current ordered backlog is:

1. finish display/multi-output diagnosis and qualification using the existing
   hardware/resource architecture;
2. audit boot readiness and remove arbitrary dependency sleeps while retaining
   finite watchdog bounds;
3. complete Coreutils portability without weakening tests;
4. extend the existing keyboard catalog/keymap coverage;
5. audit process-reference lifetime and ext4 truncate/reclaim semantics;
6. review test methodology and add missing real suites;
7. complete the global hardening review;
8. run clean AArch64 and AMD64 build/boot/nxadb/visual gates;
9. only then stage the complete next coherent phase;
10. progressively promote Power12 rules, HAL first and kernel second, after each
    scope is fully qualified.
