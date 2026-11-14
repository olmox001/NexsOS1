# Session 2 Objective Index

| ID | Priority | Objective | Status | Acceptance |
| --- | --- | --- | --- | --- |
| S2-O01 | P0 | Kernel/HAL compiler diagnostics blocking by default | verified | strict dual-arch builds, standalone HAL gates and clean dual-arch runtime pass |
| S2-O02 | P0 | Align Power12 delta blocking with ownership | queued | full audit remains reporting; modified-file gate reports but does not block `user/NexsOS1-main`; other existing scopes preserved |
| S2-O03 | P0 | Progressively harden NX-SDK and NX-USERKIT warnings | queued | bounded warning classes, repaired findings, dual-arch cross-build |
| S2-O04 | P0 | Audit remaining `README-LLM.md` declared blockers | queued | every declared issue mapped to code/test/evidence |
| S2-O05 | P1 | Add GRUB-specific quality gate | queued | owned GRUB checked without classifying it as generic kernel/HAL source |
| S2-O06 | P1 | Resolve Coreutils portability debt | queued | real 50/50 runtime pass without weakened expectations or kernel POSIX policy |
| S2-O07 | P1 | Prepare full strict Power12 for kernel + HAL | queued | findings classified/repaired before making full gate blocking |
