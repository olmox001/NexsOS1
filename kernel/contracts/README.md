# NX-CORE local contract snapshot

NX-CORE must build from its own checkout without NX-SDK or NX-VIOLA. The kernel
therefore carries a local, checksummed snapshot of the declarations that its
current implementation consumes.

This is a transitional snapshot of the existing contract headers, not a shared
repository include directory and not a second implementation of runtime code.
The maintainer's 2026-07-18 ruling is authoritative: a shared include/abi tree
is conceptually wrong because it preserves kernel↔userland coupling under a
different name. Kernel-owned definitions live in NX-CORE; the SDK receives its
own versioned, spec-derived declarations and compatibility tests.

The snapshot currently contains:
- include/abi/*.h — legacy ABI declarations still consumed by kernel source;
- include/api/elf.h — ELF wire-format declarations consumed by the loader;
- include/api/stdbool.h — compiler compatibility header selected by kernel.

The next contract phase must classify every declaration, move kernel-only
helpers into kernel headers, define normative HAL/kernel requirements and
generate the SDK-facing contract snapshot from the versioned specification.
Do not treat the legacy root include/abi directory as canonical.

The local snapshot integrity is checked by make check-contracts.
