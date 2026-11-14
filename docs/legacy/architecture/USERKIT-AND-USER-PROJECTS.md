# USERKIT and user-project architecture

## Ownership

The integrated user tree is split into five independent ownership domains:

- `user/NexsOS1-main`: the ordinary NexsOS first-party userland. System programs such as `nxshell`, `nxui`, `nxbar`, `nxlauncher` and one-shot helpers such as `nxenvinit` remain here. Being supervised by `nxmanageDD` does not make a program a USERKIT service.
- `user/BINEXTERNAL`: read-only third-party/upstream **program** repositories. The current catalog contains base-nexs, BusyBox, Coreutils, DOOM and Kilo. Image-selected programs remain user programs and are installed under `/bin`; external source ownership never moves them into `/sys/bin`.
- `user/LIBEXTERNAL`: read-only third-party/upstream **library/source** repositories. These remain superproject-owned and keep their upstream gitlinks.
- `user/USERKIT`: NX-USERKIT service and `/sbin` projects. A project belongs here only when it implements a system service/interface exposed through NX-SDK, or is an authenticated `/sbin` bootstrap project.
- `user/demo1`: an independent minimal user project used to prove that a different init and different service implementations can boot from the same installation media.

NX-USERKIT is a separate repository. `NexsOS1-main`, `BINEXTERNAL`, `LIBEXTERNAL`, and `demo1` remain coordinated by the superproject.

## SDK interface plane

USERKIT server implementations are never copied into NX-SDK. NX-SDK owns only public interface headers, reusable client/model code, extension metadata, and reusable project tooling under `system/lib/extensions/USERKIT` and `system/tools/userkit`.

Generic facilities are not reclassified as USERKIT merely because a USERKIT daemon uses them. `nxicon.h` is owned by `system/lib/extensions/icon`; `nxkeyboard_layout.h` and its implementation are owned by `system/lib/extensions/input`; object/graphics adapters remain under their generic extension directories. USERKIT project metadata may depend on those contracts without duplicating them.

## External providers and portability

`BINEXTERNAL` and `LIBEXTERNAL` repositories are imports, not writable homes for NexsOS glue. `user/BINEXTERNAL/catalog.mk` records external program source paths, image participation, `/bin` placement and internal/external library dependencies. `user/LIBEXTERNAL/catalog.mk` records external library/source providers.

Reusable portability belongs at the lowest shared first-party layer. For example, common Gnulib adaptation is owned by NX-SDK under `system/lib/portability/gnulib`, while Coreutils-only glue remains in `user/NexsOS1-main/portability/coreutils`. Program-specific patches such as Kilo hardening are applied to build-tree copies by the project portability layer so the imported gitlink remains clean. This layered rule prevents duplicate implementations and keeps NexsOS1-main independent of external source ownership.

## Service and sbin projects

The base service catalog contains `nxntfy_srv`, `nxexecd`, `nxtimerdd`, `nxpaxd`, `nxcompositor`, `nxkeyboardd`, `nxmoused`, and `nxttyd`. Each project owns `project.mk`, one server entrypoint, a pre-build test, and a runtime supervision probe.

`user/USERKIT/sbin` is the catalog for privileged bootstrap programs. `nxinit` and `nxdemo1init` are separate projects using the shared authenticated manifest-loading runtime. Their public identity/runtime headers are in NX-SDK. This layout permits additional `/sbin` implementations without treating `nxinit` as a special directory class.

`user/USERKIT/services/example` contains alternate/demo service projects. `nxnotify_registry` implements the same notification interface as `nxntfy_srv`; `nxexample_echo` adds a capability-PORT echo service with its client interface in NX-SDK. These examples are real build/runtime projects and are subject to the USERKIT Power12 gate.

## Boot ownership and project selection

NX-CORE owns the initial machine-extension entry in `kernel/config/boot-entry.conf`; user projects do not contain a duplicate boot entry. `kernel/config/user-projects.conf` is the firmware-visible catalog used by GRUB/UEFI media generation.

The common `/krnl/extension/nxmanageDD` reads the selected rootfs project's protected `manager.conf`, `services.conf`, `project`, and `profile`. It does not hardcode service implementations. The main project selects `/sbin/nxinit` and the base USERKIT service set. `demo1` selects `/sbin/nxdemo1init`, replaces the notification backend with `nxnotify_registry`, and adds `nxexample_echo`.

AMD64 GRUB selects a project rootfs as a Multiboot module. AArch64 UEFI uses one loader entry per project and passes the selected embedded rootfs through the common boot-info module fields. The kernel and manager remain identical across projects.

## Verification

`make check` in NX-USERKIT runs the SDK-owned catalog checker, all project pre-build tests, the strict USERKIT Power12 mutation/build/limit/AST gates, then builds every base service, example service, `/sbin` project and runtime probe for the selected architecture. The superproject calls this check during integrated builds.

Final acceptance requires `make run` plus real nxadb interaction on amd64 and aarch64. The main project must expose `nxinit` and the base service set; demo1 must expose `nxdemo1init`, the alternate notification backend, and the echo module. Existing whole-project userland Power12 debt is reported independently and is not reclassified as USERKIT compliance.
