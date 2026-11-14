# Session 2 Objective Index

| ID | Priority | Objective | Status | Acceptance |
| --- | --- | --- | --- | --- |
| S2-O01 | P0 | Kernel/HAL compiler diagnostics blocking by default | verified | strict dual-arch builds and clean runtime qualification |
| S2-O02 | P0 | Keep Power12 ownership/gating explicit | deferred-next-gate | full audit still reports; stricter activation waits for test-method review |
| S2-O03 | P0 | Harden NX-SDK and NX-USERKIT compiler diagnostics | verified | default strict dual-arch builds, repaired first-party findings |
| S2-O04 | P0 | Harden NexsOS1-main first-party userland | verified | integrated dual-arch userland builds; vendor warnings isolated from Werror |
| S2-O05 | P0 | Separate external program/library ownership | verified | BINEXTERNAL/LIBEXTERNAL imports clean; catalogs and portability boundaries verified |
| S2-O06 | P0 | Qualify both production boot paths | verified | clean make-run + nxadb on AArch64 UEFI and AMD64 BIOS; zero kernel-test failures |
| S2-O07 | P1 | Resolve Coreutils portability debt | deferred | baseline remains 42/50; real FTS/fd-relative model moves to next gate |
| S2-O08 | P1 | Verify NXPaX remains active and stress-safe | verified | core + 1M + 2M concurrent stress + boot test PASS |
| S2-O09 | P1 | Verify icon catalog and shell identity mapping | verified | asset parity audit + runtime shell icon check |
| S2-O10 | P1 | Resolve AMD64 initial display-size mismatch | diagnosed | resize/zoom/theme paths verified; preferred-mode discovery still open |
| S2-O11 | P1 | Close Session 2 release metadata/docs at 0.0.6.4 | verified | version alignment, English documentation/transcripts, coordinated nested-repo pushes and umbrella closure |
| S2-O12 | P1 | Prepare progressive strict Power12 HAL -> kernel | next-session | review test coverage first, then activate strict rules by ownership |
