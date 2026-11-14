# Session 2 Issues

## S2-ISSUE-001 — Coreutils portability suite 42/50

Open userland debt, explicitly outside the current kernel/HAL checkpoint.

Failures: `readlink`, `users`, `who`, `nice`, `nohup`, `chmod`,
`chown`, `chgrp`.

Observed causes include incomplete userland portability for symlink handling,
utmp/session reporting, POSIX exec replacement, `/dev/null`, and FTS-based
metadata traversal. Fixes belong in SDK/userland portability and the canonical
execution model, not as new POSIX kernel policy or weaker expected results.

## S2-ISSUE-002 — Userland warning debt

Clean image builds still expose warnings in application and external-port code.
Kernel/HAL hardening deliberately did not silently expand scope. SDK, USERKIT,
project userland and external libraries must be classified separately.

## S2-ISSUE-003 — Full Power12 qualification remains open

Full-project Power12 remains non-blocking. The owner intends future strict full
qualification for kernel + HAL after findings are classified and repaired.

## S2-ISSUE-004 — Documentation migration verifier baseline was moving

The Session 1 verifier used current `HEAD` as the source of pre-migration
documents. After the migration commit this produced false failures. The
verifier must pin the actual pre-migration commit as its preservation baseline.
