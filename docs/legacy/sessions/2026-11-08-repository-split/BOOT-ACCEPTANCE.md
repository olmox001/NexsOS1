# Clean build and GRUB/nxadb acceptance — 2026-11-08

## Host and scope

- Host: macOS 26.5.2, Intel x86_64.
- Toolchains: x86_64-elf for amd64; aarch64-none-elf for AArch64.
- Both target images were built after `make clean`.
- This verifies only the macOS Intel host. Linux x86_64/arm64 and macOS Apple Silicon remain untested.

## Build results

| Target | Result | Outputs |
|---|---|---|
| amd64 | PASS, exit 0 | kernel ELF, userland ELFs, rootfs, 512 MiB disk image |
| aarch64 | PASS, exit 0 | kernel raw binary, userland ELFs, rootfs, 512 MiB disk image |

Full logs:
- `build/runs/repository-split-clean-build-amd64-20261108.log`
- `build/runs/repository-split-clean-build-aarch64-20261108.log`

Both builds traversed the new `kernel/Makefile` and `user/Makefile` entrypoints. These are currently facades to the superproject's private build targets; recipes have not yet been fully extracted.

## GRUB and shell acceptance

| Target | Boot route | nxadb status | Shell marker |
|---|---|---|---|
| amd64 | GRUB BIOS ISO → QEMU | shell=yes, faults=0 | `NX_SPLIT_ACCEPTANCE_AMD64` |
| aarch64 | GRUB arm64-efi ISO → QEMU | shell=yes, faults=0 | `NX_SPLIT_ACCEPTANCE_AARCH64` |

Commands sent through NXShell:
- `echo NX_SPLIT_ACCEPTANCE_AMD64`
- `echo NX_SPLIT_ACCEPTANCE_AARCH64`

Both exact markers are present in their corresponding serial logs. Both QEMU stderr logs are 0 bytes. The post-boot scan found no `PANIC`, `PAGE FAULT`, `PROTECTION FAULT` or AArch64 `Data Abort (Lower EL)` markers. Both QEMU sessions were stopped after acceptance.

Captured artifacts:
- Transcript: `build/runs/repository-split-nxadb-acceptance-20261108.log`
- amd64 build/ISO transcript: `build/runs/amd64/nxadb-build-20261108-112357-316575000.log`
- amd64 session archive: `build/runs/amd64/boot-20261108-112408-617286000/`
- amd64 serial/stdout/stderr: `build/runtime/nxadb/amd64/`
- aarch64 build/ISO transcript: `build/runs/aarch64/nxadb-build-20261108-112413-715433000.log`
- aarch64 session archive: `build/runs/aarch64/boot-20261108-112621-531974000/`
- aarch64 serial/stdout/stderr: `build/runtime/nxadb/aarch64/`

The acceptance shell transcript also contains a harmless host-side command-line mistake: `nxadb bootlog --no-follow` is not a supported option. The command printed usage and did not alter the guest. The boot and shell results are independently verified from nxadb status, exact serial markers, complete captured serial logs and QEMU stderr files. Do not treat the unsupported bootlog invocation as a guest failure.

## Layering audit

The in-tree `scripts/check-layering.sh` completed:

- kernel including userland headers: **19** findings (includes test-only and public ABI candidates; each needs ownership classification);
- userland including kernel code: **0** findings;
- generated userland dependency files referencing `kernel/include` or `kernel/arch`: **0**.

This is not a fully clean layering result. The remaining kernel→userland include sites must be classified and resolved through the versioned kernel contract, not hidden by include flags.

## Warnings and limits

Warnings remain in SDL2, Lua, nxempire, Gnulib and Coreutils. Builds are successful but are not warning-zero. This evidence does not certify every optional application, hardware device, or physical platform; QEMU GRUB/shell acceptance is the scope of this record.

## NX-CORE-only build and boot acceptance — 2026-11-08

This is a separate acceptance profile from the full-system nxadb test above.
It deliberately has no userland or rootfs.

| Gate | amd64 | aarch64 |
|---|---|---|
| Standalone module clean build | PASS | PASS |
| Exact compiler/binutils check | PASS | PASS |
| GRUB BIOS / arm64-efi image build | PASS | PASS |
| GRUB QEMU boot | PASS | PASS |
| Kernel reaches K1+K2 healthy kernel-alone mode | PASS | PASS |
| Kernel panic/fault during observed boot | none | none |
| Kernel self-test summary | no KTEST summary in captured amd64 log | 12 PASSED, 0 FAILED |

Standalone build commands:
- make -C kernel clean all ARCH=amd64
- make -C kernel clean all ARCH=aarch64
- make -C kernel check ARCH=amd64
- make -C kernel check ARCH=aarch64
- make -C kernel boot-iso ARCH=amd64
- make -C kernel boot-iso ARCH=aarch64

The isolated build gate copied only kernel/ to /tmp/NX-CORE-isolated, excluding
its build directory and with no parent checkout, SDK, or userland present.
That isolated copy clean-built both targets and booted both kernels directly
under QEMU. Serial logs:
- /tmp/nxcore-isolated-amd64-boot.log
- /tmp/nxcore-isolated-aarch64-boot.log

The actual NX-CORE working tree also built and booted both GRUB images. The
AArch64 GRUB route used GNU GRUB 2.14 and EDK2 split pflash firmware. QEMU was
stopped by the 20–25 second timeout after confirming the kernel remained
running; exit 124 is the expected timeout, not a guest failure.

Kernel-only boot has no /sbin/nxinit by design. K3 logs that userland could not
start and continues in "kernel-alone mode (K1+K2 healthy, no reboot; inspect
via UART)". This is the acceptance marker for the isolated kernel profile,
not a full NexsOS1 desktop/shell acceptance.
