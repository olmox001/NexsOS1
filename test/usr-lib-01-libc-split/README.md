# PARKED — USR-LIB-01: split libc off the kernel sources

Work-in-progress parked by the maintainer (2026-07-18) to be redone with a
planned approach against the phase plan, rather than merged ad hoc. **Nothing
here is built**; it is reference material for Phase 10/12.

## The defect it addresses (real, and already documented in-tree)

`user/sys/lib/lib.c` carried this note:

> `USR-LIB-01 (W2 BAD-IMPL)` Directly `#includes` kernel/lib C sources

and did, literally:

```c
#include "../../kernel/lib/math.c"
#include "../../kernel/lib/string.c"
#include "../../kernel/lib/vsnprintf.c"
```

So the userland libc COMPILED KERNEL SOURCE FILES. That inverts the layering
ASTRA is built on: the POSIX personality is supposed to sit *above* the OS1 base
API, and instead it reached into the kernel tree. Consequences:

- any internal change to a kernel/lib C file silently changed userland
  behaviour (and vice versa — the files carry `#ifdef KERNEL` branches);
- the two sides could never diverge where they legitimately should (a kernel
  `vsnprintf` has no business growing float support for userland printf);
- it blocks the SRL/HAL source-tree split (ASTRA §6.4 B5) and Phase 12's
  "services divided from the standard libc, integrated modularly", because the
  dependency runs the wrong way.

## What is in here

| file | what it is |
|---|---|
| `usr-lib-01.patch` | `git diff` against commit `7779a4a` — the edits to `Makefile`, `include/api/math.h`, `include/api/string.h`, `user/sys/lib/lib.c` |
| `libc_string.c` | standalone userland string/memory primitives (275 lines) |
| `libc_math.c` | standalone userland math (128 lines) |
| `libc_vsnprintf.c` | standalone userland formatting (505 lines) |

Approach taken: implement the three units independently under `user/sys/lib/`,
build them via the existing pattern rule, and link them into every ELF alongside
`lib.o` — leaving `kernel/lib/*` untouched and kernel-only.

## To resume

```sh
git apply test/usr-lib-01-libc-split/usr-lib-01.patch
cp test/usr-lib-01-libc-split/libc_*.c user/sys/lib/
```

## Questions to settle BEFORE re-applying (why it was parked)

1. **Duplication vs divergence.** Three files now exist twice (kernel and
   userland). That is correct ONLY if the two are allowed to diverge on purpose;
   otherwise it is the "no duplicated logic" rule broken in a new place. Decide
   and write the rule down — e.g. kernel keeps a minimal freestanding set,
   userland owns the POSIX-complete one.
2. **Does the userland `vsnprintf` keep float support?** The current shared file
   guards it with `#ifndef KERNEL`; after a split that guard has no reason to
   exist and should be resolved one way or the other.
3. **Verification.** The formatting code is exercised by essentially every
   program, so a regression is broad but quiet. It was host-validated once
   before (`%f`/`%e`/`%g`/`%p` against the host libc) — that harness should be
   re-run against the split copy rather than trusting a clean build.
4. **Sequencing.** This is the same boundary Phase 12 moves (`/sys/services`,
   services split from libc) and ASTRA §6.4 B5 (SRL/HAL tree split). Doing it
   twice would mean moving the same files twice — see Phase 10/12 in
   `docs/PLAN-2026-07-17-STRATIFICATION.md`.
