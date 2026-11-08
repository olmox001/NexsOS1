# Four-project repository ownership matrix

Date: 2026-11-08
Status: ownership map established from current source/build consumers; source
moves and gitlink relocation are the next execution phase.

## Project boundaries

| Project | Repository identity | Superproject mount | Owns |
|---|---|---|---|
| Kernel | NX-CORE | kernel/ | Kernel implementation, per-ISA HAL, drivers, kernel tests, linker scripts, kernel boot handoff, kernel build and boot verification |
| Userland | NX-VIOLA | user/ | User programs, init, services, shells, app resources, application-specific integration and tests |
| System SDK | NX-SDK | system/ | NexsOS-owned system runtime/library implementations, public userland API headers, startup/syscall ABI objects, portability adapters, SDK manifests and SDK conformance tests |
| Integration | NexsOS1 | repository root | Pins all project/dependency revisions, coordinates module builds, rootfs installation manifests, GRUB/EFI image and disk/release packaging, cross-project acceptance |

The SDK mount is system/, not sdk/. This is a peer project root, not a
subdirectory of NX-VIOLA. The name reflects the source ownership found in the
audit: system API, runtime, startup, and portability. It avoids both the
unauthorized root sdk/ proposal and nesting NX-SDK inside user/.

## Source ownership and moves

| Current path | Destination | Reason |
|---|---|---|
| kernel/ | NX-CORE, unchanged | Already a coherent source owner; standalone build is verified |
| user/sys/lib NexsOS-owned C sources | system/lib | These are system runtime/library implementations, not applications |
| include/api OS1/POSIX/libc declarations | system/include/api | The SDK owns the public userland API surface |
| include/abi legacy public declarations | system/include/contract | SDK-side compatibility snapshot; the root shared include/abi directory is not canonical |
| user/arch/<arch>/syscall.S | system/arch/<arch>/syscall.S | Startup, syscall wrappers, setjmp/longjmp are SDK runtime objects |
| user/sys/lib/portability generic adapters | system/lib/portability | These implement SDK-facing compatibility surfaces |
| user/sys/lib/portability/coreutils | user/portability/coreutils | App-specific glue belongs with the Coreutils port, not generic SDK |
| user/sys/lib/portability/gnulib generic OS1 glue | system/lib/portability/gnulib | Generic SDK adapter; its app-specific overlay entries are split out |
| user/sys/lib/sdl, lua, gnulib, musl, opengl, direct3d gitlinks | user/lib/<dependency> | Upstream sources remain independent dependency repositories; preserve URL, commit and nested state |
| include/api/stb_image.h, stb_easy_font.h, pl_mpeg.h | user/lib/<dependency>/include | Third-party headers, not NexsOS API declarations |
| user/bin/{base-nexs,busybox,coreutils,doom,kilo} gitlinks | user/bin/* | Existing app/port dependencies remain attached to the relevant userland ports |
| root Makefile | integration coordinator | Delegates to module Makefiles; owns only orchestration and image assembly |

The upstream gitlinks are not owned by NX-VIOLA or NX-SDK merely because they
are mounted below user/. The userland repository may pin these dependencies
through nested gitlinks, while their source remains upstream-owned. The root
superproject coordinates recursive initialization and the exact module refs.

## Contract boundary

- NX-CORE has its own local legacy contract snapshot under kernel/include.
  It does not include top-level include/api or include/abi.
- NX-SDK gets a separately versioned SDK contract snapshot derived from the
  normative kernel/HAL specifications. It does not include kernel-private
  headers or kernel implementation sources.
- Compatibility is directional and executable: SDK declares the supported
  KERNEL-SPEC range; conformance fixtures compare SDK declarations, syscall
  numbers, object layouts, wire formats, feature status and runtime behavior
  with the selected kernel specification.
- The legacy root include/abi directory is transitional input only and will
  be removed after the SDK contract snapshot and kernel-local definitions are
  verified. Do not create a new shared header directory.

## Build independence

### NX-CORE

Clone kernel/ only. Run its local setup/check/build/boot targets. It must not
need system/, user/, or root Makefile.

### NX-SDK

Clone system/ only, plus explicitly pinned upstream dependencies required by
the enabled SDK feature profile. Build the SDK runtime, headers, startup
objects, library archives and a freestanding SDK sample for amd64/aarch64.
It must not need kernel/ or user/.

### NX-VIOLA

Clone user/ plus system/ and its declared upstream dependencies. Build
userland ELF programs against only the SDK export manifest. It must not need
kernel/ or root Makefile.

### NexsOS1 integration

The root Makefile calls the module entrypoints, stages each module's install
manifest into rootfs, then builds GRUB/EFI images, disks and release artifacts.
It must not compile private source files owned by a module.

## Git preservation requirements

Before moving any gitlink:
- Record its exact URL, commit SHA, branch metadata and nested submodule refs.
- Check and preserve local dirty/untracked state inside that submodule.
- Move the gitlink without updating it; never fetch/checkout a newer branch.
- Verify git submodule status before and after.
- Preserve the user's modified user/bin/coreutils gitlink and the staged
  user/sys/lib/lib-2.c deletion; do not fold either into unrelated changes.
- Do not touch root .gitignore.
