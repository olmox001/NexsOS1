# NexsOS1 repository and contract separation session

Date: 2026-11-08
Checkout: NexsOS1-Resurrection/NexsOS1
Branch at inspection: DEV-Resurrection
Observed HEAD at inspection: 9f09f2e

This session separates the existing project into four collaborating projects: NX-CORE, NX-VIOLA, NX-SDK and the NexsOS1 integration superproject. The work starts with build-system boundaries and executable specification contracts; repository extraction follows only after those gates are reliable.

## Ownership model

- **NX-CORE**: existing `kernel/`, including the existing per-architecture HAL implementations, kernel tests, boot/entry assembly and kernel-private code.
- **NX-VIOLA**: existing `user/`, including services, applications, init, shell, resources and userland integration.
- **NX-SDK**: NexsOS-owned system libraries, native API, POSIX/C compatibility, runtime/startup interfaces and NexsOS portability implementations. It is logically part of the NX-VIOLA platform, independently versioned, and directly consumed by NX-CORE only through a narrow kernel-safe contract subset. It must not make NX-CORE depend on the NX-VIOLA repository.
- **NexsOS1 superproject**: pins all three module revisions and upstream dependencies; owns host/toolchain orchestration, rootfs, GRUB, mkdisk, release packaging and end-to-end boot/nxadb integration.
- **External upstream projects** remain separate dependencies. Preserve each gitlink, URL, branch, commit, nested submodule and local working state.

## Contract model

HAL specification, kernel/user ABI specification and NX-SDK contract have independent semantic versions, stable requirement IDs, explicit compatibility declarations and machine-checked conformance evidence. Support classification is separate from test state. See `CONTRACT-VERSIONING.md`.

## Non-negotiable constraints

1. Do not duplicate an existing implementation or create placeholder adapters.
2. Do not move files based only on names or current paths.
3. Preserve and reuse existing HAL implementations.
4. Preserve amd64/aarch64 boot, assembly, linker and userland startup paths.
5. Preserve GRUB, mkdisk, rootfs, release, nxrun and nxadb behavior.
6. Support macOS/Linux hosts on Intel/ARM; certify each host/target pair only after real verification.
7. Keep Coreutils gitlink and nested Gnulib state untouched.
8. Preserve the already-staged deletion of `user/sys/lib/lib-2.c`.
9. Do not modify `.gitignore`.
10. Do not commit, push or create remote repositories without explicit authorization.
11. Do not claim NASA/JPL Power-of-Ten compliance without reproducible per-module evidence.
12. Do not change source ownership until symbol, include, link and runtime consumers are mapped.

## Session records

- `PLAN.md`: incremental work plan and acceptance gates.
- `STATE.md`: checkout state, existing modifications and findings.
- `BUILD-INVENTORY.md`: current build, toolchain, assembly, rootfs, mkdisk, release and nxadb.
- `CONTRACT-VERSIONING.md`: HAL/kernel/SDK versioning, support classes and complete conformance model.
- `BOOT-ACCEPTANCE.md`: clean builds, GRUB boot and nxadb shell evidence.
- `LIBRARY-CLASSIFICATION.md`: libc/native API/service-client ownership.
- `PORTABILITY-CLASSIFICATION.md`: SDL2/Gnulib/Lua/Coreutils/graphics adapter ownership and support gaps.
- `NASA-POWER-OF-TEN.md`: ten original rules, observed conflicts and evidence requirements.
