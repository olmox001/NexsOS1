# Session 2 Verification Record

## Compile-time hardening

PASS:
- NX-CORE AArch64 strict kernel build
- NX-CORE AMD64 strict kernel build
- NX-HAL standalone AArch64 `check`
- NX-HAL standalone AMD64 `check`
- HAL host/unit/mutation/cross-link tests
- ASan/UBSan
- clang-tidy gate
- cppcheck gate
- kernel `make check` on AArch64 and AMD64
- contract and SDK-boundary checks
- `git diff --check`

HAL boundary audit was corrected to honor caller `BUILD_ROOT` and to exclude
`kernel/external` only from the generic NX-CORE architecture-leak scan.

## Clean AArch64 runtime

Standard build graph; normal Power12 behavior preserved.

`BOOT-NXTEST=DONE SHELL-INTERACTION=PASS SHELL-UP-HISTORY=PASS
TIMERDIAG=PASS SHELL-AFTER-TIMER=PASS COUNTER-HEARTBEAT=PASS
SHELL-AFTER-HEARTBEAT=PASS KTEST=38 passed/0 failed faults=0`

Additional: `pwd=/home`; `nxproc` PASS.
Screenshot: `build/runs/aarch64/iso-uefi/s1-o07-final.png` (1280x800).

## Clean AMD64 runtime

`BOOT-NXTEST=DONE SHELL-INTERACTION=PASS SHELL-UP-HISTORY=PASS
TIMERDIAG=PASS SHELL-AFTER-TIMER=PASS COUNTER-HEARTBEAT=PASS
SHELL-AFTER-HEARTBEAT=PASS KTEST=37 passed/0 failed faults=0`

Additional: `pwd=/home`; `nxproc` PASS.
Screenshot: `build/runs/amd64/iso-bios/s1-o07-final.png` (1280x800).

## Non-kernel suite debt

On both architectures the boot userland runner is 18/19 because the existing
Coreutils test is 42/50. Failing portability cases are `readlink`, `users`,
`who`, `nice`, `nohup`, `chmod`, `chown`, `chgrp`. This was not
used to weaken the kernel/HAL qualification result and remains open userland
work.
