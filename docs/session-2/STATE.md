# Session 2 State — 2026-11-14

## Closing checkpoint

Release candidate: **0.0.6.4**.

Branches remain:
- umbrella: `DEV-Resurrection`
- NX-CORE: `main`
- NX-SDK: `main`
- NX-USERKIT: `main`

Session 1 remains the historical split-repository baseline. Session 2 hardened the
compiler policy, SDK/userland portability, external-source boundaries and
runtime qualification without changing the object/capability architecture.

The verified implementation freeze was pushed in dependency order before this
documentation/version closure:
- NX-SDK hardening: `e9895c6`, followed by release metadata `534aa0a`.
- NX-USERKIT hardening: `116fbfb`.
- NX-CORE machine-extension hardening: `47096a9`.

The umbrella closure records those gitlinks together with the verified userland
hardening, external catalogs, release metadata and English documentation. The
post-freeze EDID/multi-output work remains outside this closure until its own
runtime qualification completes.

## Repository ownership

The verified user tree is now split into explicit ownership domains:

- `user/NexsOS1-main`: first-party NexsOS user programs and project-specific
  portability glue.
- `user/BINEXTERNAL`: read-only imported external **program** repositories.
  Current gitlinks are base-nexs, BusyBox, Coreutils, DOOM and Kilo. Programs
  selected for the NexsOS image are installed in `/bin`, never `/sys/bin`.
- `user/LIBEXTERNAL`: read-only imported external **library/source** repositories.
- `user/USERKIT`: system services and privileged bootstrap projects.
- `user/demo1`: independent alternate user project used to prove common
  kernel/SDK/USERKIT project selection.

`user/BINEXTERNAL/catalog.mk` declares source paths, image participation,
VFS placement and library dependencies. `user/LIBEXTERNAL/catalog.mk`
provides the matching library-source catalog. Imported repositories remain
clean; NexsOS adaptations live in portability layers.

Common Gnulib portability is owned by NX-SDK under
`system/lib/portability/gnulib`. Coreutils-specific glue remains under
`user/NexsOS1-main/portability/coreutils`. Kilo hardening is applied from the
project portability layer to a build-tree copy, leaving the imported Kilo
repository untouched.

## Compiler hardening

NX-CORE and HAL retain strict warnings-as-errors by default. Session 2 extended
the same default posture through NX-SDK and NX-USERKIT and removed first-party
warning debt encountered by the integrated NexsOS1-main build.

External source warnings are not hidden as if they did not exist. Vendor
translation units compile through the external-warning wrapper: a failing
external compile prints its full diagnostics; successful vendor warnings are
captured and summarized separately from first-party Werror diagnostics.

Latest integrated summaries:
- AArch64: 558 warning lines across 123 external-source files.
- AMD64: 517 warning lines across 125 external-source files.

These counts are external debt, not first-party warnings accepted by weakening
the strict build.

## SDK contracts and portability

NX-SDK contract snapshots pass at:
- ABI revision r2
- API revision r3
- USERKIT revision r2

The API r3 snapshot includes the hardened string declarations. STB is classified
under the SDK external-source tree instead of a first-party include directory.

The shared Gnulib provider now generates target headers from the imported
Gnulib source and owns the common NexsOS compatibility layer. FTS is explicitly
**not** claimed functional in this checkpoint: `fts_stub.c` returns `ENOSYS`
for traversal operations. A real FTS implementation requires completion of the
SDK fd-relative directory model and is deferred to the next portability gate.

## NXPaX

NXPaX remains enabled across HAL, kernel and userland. No disable macro or build
switch was found.

Verified production-linked tests:
- NXPaX core test: PASS.
- stress: 1 writer / 1,000,000 events: PASS.
- stress: 2 writers / 2,000,000 concurrent events: PASS.
- boot userland `11nxtest_nxpax`: PASS on both architectures.

Further NXPaX development is intentionally deferred to the next gate.

## Runtime qualification

### AArch64 UEFI

Clean `make run ARCH=aarch64`:
- boot runner: 18/19
- only failure: the known Coreutils 42/50 portability baseline
- all other boot tests PASS

nxadb display gate:
- `BOOT-NXTEST=DONE`
- shell interaction/history PASS
- timer diagnostic PASS
- counter heartbeat PASS
- KTEST 38/0
- faults 0
- `pwd` -> `/home`
- `nxproc` PASS
- screenshot: 1280x800

The shell icon is now correctly classified as the console/terminal icon even
when the graphical TTY window is owned by `nxttyd`.

### AMD64 BIOS

Clean `make run ARCH=amd64`:
- boot runner: 18/19
- only failure: the same Coreutils 42/50 baseline
- all other boot tests PASS

nxadb display gate:
- `BOOT-NXTEST=DONE`
- shell interaction/history PASS
- timer diagnostic PASS
- counter heartbeat PASS
- KTEST 37/0
- faults 0
- `pwd` -> `/home`
- `nxproc` PASS

The initial AMD64 BIOS scanout is currently 640x480. This is not a compositor
resize failure: VirtIO-GPU reports a 640x480 initial scanout through
`GET_DISPLAY_INFO`. Manual `nxres 1280 800` succeeds, zoom 200% produces
640x400 with native 1280x800, zoom 100% restores 1280x800, and both light/dark
themes render at 1280x800. Preferred-mode discovery at boot is tracked as an
open display issue.

## Power12 next gate

Full-project Power12 remains reporting/non-blocking in this checkpoint. The next
hardening phase starts only after review of the current test methodology and
missing suites. Strict-rule activation then proceeds progressively:
1. HAL
2. kernel

No Power12 gate is silently promoted by this Session 2 closure.
