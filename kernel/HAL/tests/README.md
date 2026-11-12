# NX-HAL verification

This suite is the HAL-owned integration of every NXEC preboot unit test. Test
logic is retained; only include roots changed after moving the implementation
out of the temporary HAL/nxec import.

Required gates are host tests, x86_64/AArch64 freestanding cross-link,
ASan/UBSan, clang-tidy/cppcheck, standalone HAL builds, standalone NX-CORE
builds and nxadb boot/runtime tests on both architectures.

A documented PASS from the imported repository is not accepted as evidence when
the current source fails a gate. The first import exposed and fixed two
clang-tidy failure classes in NXEC hal/mmu.c.
