# Session 2 State — 2026-11-14

## Checkpoint

Branches: umbrella `DEV-Resurrection`; NX-CORE/NX-SDK/NX-USERKIT `main`.

Session 1 committed baseline:
- umbrella `1d866b0c2009df097ead21d7d69a789b13fbd6ca`
- NX-CORE `97fb5edd387b627dc38e63df6f4f2604d502a826`
- NX-SDK `3d0791ee619e3ffc14de0fcdbe912af593e64a6b`
- NX-USERKIT `c974c07e6832d84a786a5957e6eb3ce843bd8b95`

Session 2 NX-CORE hardening commit:
- `fab9d33 build: make kernel and HAL warnings fatal by default`

## Kernel/HAL warning policy

Default kernel/HAL builds retain all enabled warning classes, including
`-Wconversion` and `-Wsign-conversion`, and make them fatal through the
single `NX_WARNINGS_AS_ERRORS=1` policy. Assembler warnings and kernel/boot
linker warnings are fatal. This is compile-time policy only; it introduces no
runtime panic/blocking behavior.

Concrete findings were repaired instead of suppressed across USB/VirtIO,
GPT/ext4, scheduler/ELF, allocation/formatting/logging, graphics/terminal code
and AMD64 HAL.

## Runtime qualification

AArch64 UEFI clean standard gate:
- nxadb full display gate PASS
- KTEST 38/0
- faults 0
- shell/history/timer/counter PASS
- `pwd` -> `/home`
- `nxproc` PASS
- screenshot 1280x800

AMD64 BIOS clean standard gate:
- nxadb full display gate PASS
- KTEST 37/0
- faults 0
- shell/history/timer/counter PASS
- `pwd` -> `/home`
- `nxproc` PASS
- screenshot 1280x800

Architecture-specific KTEST totals differ; both have zero failures.

## Userland debt outside this checkpoint

The boot runner remains 18/19 because `04nxtest_coreutils` is 42/50. The
owner explicitly excluded this pre-existing userland portability debt from the
kernel/HAL staging criterion. It remains tracked in `ISSUES.md`.

## Power12

No Power12 off-switch exists. Full-project Power12 remains reporting/non-
blocking. Existing modified-file blocking scopes remain except that the next
policy block must stop blocking changes under `user/NexsOS1-main` while still
reporting findings. Future full strict qualification is intended first for
kernel + HAL.

## GRUB

`kernel/external/grub` is excluded only from the generic NX-CORE/HAL
architecture-leak scan. GRUB is owned code and requires a separate
target-appropriate quality gate.
