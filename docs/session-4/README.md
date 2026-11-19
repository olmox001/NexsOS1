# Session 4 — ASTRA security, multiuser, services and GNU compatibility

Status: design and incremental implementation. All new source and documentation changes remain UNSTAGED until real tests pass. No commits, pushes, branch switches or remote operations.

Grounding: docs/legacy/ASTRA.md (sections 6 and 7), docs/legacy/direction/DIR-04-capabilities-and-services.md, docs/legacy/architecture/NXMANAGE-DD-BOOT-CAPABILITY-2026-11-10.md, docs/legacy/architecture/EXECSVC-COMMON-EXECUTION-CONTRACT.md and docs/legacy/coreutils-port-audit.md. These are design inputs; the current source and tests determine what is actually implemented.

Read ARCHITECTURE.md, SECURITY.md, GNU-QUALIFICATION.md and IMPLEMENTATION-STATUS.md before changing the code. Maintain NXPaX faults, HAL separation, userland compositor and the existing object ABI. Never implement a parallel supervisor, syscall, VFS or graphics manager.
