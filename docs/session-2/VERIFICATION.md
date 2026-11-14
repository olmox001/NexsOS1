# Session 2 Verification Record

## Compile-time and contract gates

Verified PASS:

- NX-CORE strict AArch64 and AMD64 builds.
- standalone HAL gates and existing host/unit/mutation/cross-link checks.
- NX-SDK strict/default dual-architecture builds.
- NX-USERKIT strict/default dual-architecture builds and USERKIT Power12 gate.
- NexsOS1-main integrated first-party userland build on both architectures.
- demo1 project checks on AArch64 and AMD64.
- kernel SDK-boundary check.
- NX-SDK contract snapshot check: ABI r2, API r3, USERKIT r2.
- external-program catalog consistency check.
- P2P protocol tests: 8/8.
- icon catalog audit: 131 dark + 131 light assets, all 64x64, no missing
  catalog paths, dark/light index parity.
- `git diff --check` during hardening cycles.

External-source diagnostics are captured separately instead of weakening
first-party Werror:
- AArch64 integrated summary: 558 warning lines / 123 files.
- AMD64 integrated summary: 517 warning lines / 125 files.

## Gnulib/Coreutils boundary

The common Gnulib compatibility layer moved to NX-SDK portability and consumes
the read-only `user/LIBEXTERNAL/gnulib` source provider. Generated headers are
published from the build tree and consumed by Coreutils through the same SDK
provider.

The previous `fts_.h` in NexsOS1-main was only a declaration shim and was not
a functional FTS implementation. Session 2 replaces that false surface with the
generated Gnulib contract plus an explicit `ENOSYS` FTS stub. Focused
Coreutils `chmod` link checks pass on both architectures. Real FTS is deferred
until the SDK fd-relative directory model is implemented and tested.

## NXPaX evidence

- NXPaX core production test: PASS.
- 1,000,000-event stress run: PASS.
- 2,000,000-event two-writer concurrent stress run: PASS.
- boot `11nxtest_nxpax`: PASS on AArch64 and AMD64.
- no NXPaX disable build flag/macro found.

## Clean AArch64 runtime

`make clean ARCH=aarch64 && make run ARCH=aarch64` completed the full image
graph, including NexsOS1-main and demo1.

Boot userland runner:
- 18/19 PASS
- `04nxtest_coreutils`: 42/50, same known baseline
- every other test PASS

nxadb `aarch64/iso-uefi`:
```
BOOT-NXTEST=DONE
SHELL-INTERACTION=PASS
SHELL-UP-HISTORY=PASS
TIMERDIAG=PASS
SHELL-AFTER-TIMER=PASS
COUNTER-HEARTBEAT=PASS
SHELL-AFTER-HEARTBEAT=PASS
KTEST=38 passed/0 failed
faults=0
```

Additional:
- `pwd` -> `/home`
- `nxproc` PASS
- screenshot: `build/runs/aarch64/iso-uefi/session2-aarch64.png`
- verified dimensions: 1280x800
- shell dock icon visually verified as the console/terminal icon

## Clean AMD64 runtime

`make clean ARCH=amd64 && make run ARCH=amd64` completed the full image graph,
including NexsOS1-main and demo1.

Boot userland runner:
- 18/19 PASS
- `04nxtest_coreutils`: 42/50, same known baseline
- every other test PASS

nxadb `amd64/iso-bios`:
```
BOOT-NXTEST=DONE
SHELL-INTERACTION=PASS
SHELL-UP-HISTORY=PASS
TIMERDIAG=PASS
SHELL-AFTER-TIMER=PASS
COUNTER-HEARTBEAT=PASS
SHELL-AFTER-HEARTBEAT=PASS
KTEST=37 passed/0 failed
faults=0
```

Additional:
- `pwd` -> `/home`
- `nxproc` PASS
- initial screenshot:
  `build/runs/amd64/iso-bios/session2-amd64.png` = 640x480

## AMD64 resize / zoom / theme diagnostic

The AMD64 size defect was reproduced and isolated without changing QEMU or
hardcoding a resolution.

Initial state:
- VirtIO-GPU log: `host display 640x480`
- graphics/compositor initialization: 640x480
- `nxres`: 640x480, minimal/dark/blue

Runtime tests:
1. `nxres 1280 800`
   - VirtIO-GPU: `mode set to 1280x800`
   - compositor bounds: 1280x800
   - screenshot: 1280x800
2. `nxres zoom 200`
   - scanout: 640x400
   - native reference: 1280x800
   - screenshot: 640x400
3. `nxres zoom 100`
   - scanout returns to 1280x800
   - screenshot: 1280x800
4. `nxres theme light` and `nxres theme dark`
   - both render successfully at 1280x800

Conclusion: resize, zoom and theme paths are internally coherent once a native
1280x800 mode is selected. The remaining defect is boot-time preferred-mode
selection under AMD64 BIOS: `GET_DISPLAY_INFO` exposes the current 640x480
scanout, not a preferred monitor mode.

## Accepted non-regression debt

Coreutils remains exactly 42/50 on both architectures. The accepted failures
remain:
- readlink
- users
- who
- nice
- nohup
- chmod
- chown
- chgrp

This checkpoint does not lower expectations or claim these cases fixed.

## Post-freeze display work intentionally excluded from this closure

After the verified freeze, an EDID preferred-mode parser/selection prototype was
implemented in NX-CORE and passed:
- dedicated host parser test;
- strict AArch64 kernel build;
- strict AMD64 kernel build.

The owner stopped the fresh QEMU qualification before the new path completed a
full dual-architecture runtime gate. Therefore the EDID/multi-output changes are
**not** part of the verified Session 2 code freeze or this release closure.
They remain unstaged work for the next qualification cycle.
