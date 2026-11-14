# Session 1 Verification Record

## Recovery

Pre-clean recovery ZIP:
`.git/session1-recovery/2026-11-14-session1-preclean.zip`

SHA-256:
`585cc2934c7fceebe188085faa6d06c2c6d0aae6ff47dde7ad28147c24352817`

The archive contains per-repository binary patches, index baselines, working-tree copies, untracked copies, status/manifests and checksums.

## Staged baseline tests

Passed:
- `python3 tools/test_nxmanage_boot_manifest.py`
- `python3 tools/test_nxservice_layout.py`
- `python3 user/demo1/tests/project_test.py`
- `make test-keyboard-layout`
- NX-CORE `check-object-boot-provider` on amd64 and aarch64
- NX-CORE `check-sdk-boundary` on amd64 and aarch64
- NX-USERKIT prebuild project tests
- NX-USERKIT TTY model host test

## Contract gates

After Session 1 NX-CORE snapshot alignment:
- `make -C kernel ARCH=amd64 ... check-contracts`: PASS, ABI r2
- `make -C kernel ARCH=aarch64 ... check-contracts`: PASS, ABI r2

Root `make check-kernel-contracts`:
- TIME layout: PASS
- NX-CORE snapshot: PASS
- stops at NX-SDK historical USERKIT revision error: previous HEAD changed public headers without revision bump r1 -> r1.

## Power12

Verifier mutation tests: 17/17 PASS.

Full `make power12-strict`: FAIL.
Observed:
- R10 stale alternate source artifacts: 3
- R11 ID/limit provenance findings: 375
- AST findings: 5143

Scoped `make power12-changed` for current verified deltas: PASS with 0 findings.
This is not full-project qualification.

## Clean architecture builds and runtime

### AArch64

A clean build with the VirtIO input split-ring cache fix completed successfully:
- kernel
- userland
- rootfs
- demo1
- DTB
- GRUB UEFI ISO

Runtime observations:
- KTEST: 35 passed / 0 failed
- VirtIO input devices discovered: 2
- zero kernel faults in tested sessions
- long QMP input string arrived byte-for-byte correctly:
  `inputfix0123456789abcdefghijklmnopqrstuvwxyz`
- repeated `pwd`: `/home`
- `/sys/bin/nxwins`: successful
- `ls /sys/services`: successful
- stress timer-kill lane: 111 tests, 94 killed, 17 completed, 0 errors, 1/1 PASS

A first command immediately after boot can still be lost; tracked as S1-INPUT-001.

### AMD64

A clean BIOS/GRUB build completed successfully with the same shared VirtIO helper changes.

Runtime observations:
- shell reached
- zero kernel faults in tested session
- first immediate `pwd` was observed once as `ppwd`
- subsequent `/sys/bin/nxwins`: successful
- subsequent `ls /sys/services`: successful
- subsequent `pwd`: `/home`

This supports keeping the initial-input problem separate from the ARM-specific non-coherent split-ring bug.

## VirtIO input task gates

Host test `check-virtio-ring-layout`:
- amd64: PASS
- aarch64: PASS

Expected split-ring spans tested:
- avail qsize 1: 8 bytes
- avail qsize 16: 38 bytes
- used qsize 1: 14 bytes
- used qsize 16: 134 bytes

Task recovery patch:
`.git/session1-recovery/verified-input-dma/verified-input-dma.patch`

SHA-256:
`ab175596905dc139acb5b0f9c126f4b420aa3eb500a97f5cd5ca339258d441e3`

## Documentation migration gate

Executed:

- full reference/path census before moving files;
- migration of every pre-Session-1 `docs/` entry to `docs/legacy/` while
  retaining the relative tree;
- restoration/verification of each historical file from its original Git
  `HEAD` blob;
- update of current root/index/source references to the legacy namespace;
- `python3 tools/test_docs_layout.py`.

Result: **PASS**.

The gate verifies:
- active `docs/` root shape;
- all 169 historical tracked files exist at the mapped legacy path and are
  byte-identical to their original blobs;
- every project-document path indexed by `.presstoai.json` exists;
- active Markdown links resolve;
- required Session 1 documents exist;
- current tracked files outside the archive do not retain exact references to
  moved historical document paths.

The migration intentionally does not rewrite historical documents. A
pre-existing broken reference from `tools/rea/README.md` to
`docs/primary/TOOLS_REA.md` is catalogued as S1-DOC-002 rather than repaired
with an invented file.

## Generic PCI DMA-claim safety block

Focused verification:

- HAL PCI host unit test: PASS;
- HAL PCI ASan/UBSan host test: PASS;
- freestanding PCI core compile for x86_64-elf: PASS;
- freestanding PCI core compile for aarch64-none-elf: PASS;
- `make -C kernel ARCH=aarch64 check-driver-dma-claim`: PASS;
- `make -C kernel ARCH=amd64 check-driver-dma-claim`: PASS;
- scoped Power12 audit over the changed PCI/driver/USB/test files: 0 findings;
- scoped `git diff --check`: PASS.

AArch64 clean integration:

- `make clean`: PASS;
- clean graphical `make ARCH=aarch64 ... run`: PASS to NXShell;
- KTEST: 35 passed / 0 failed;
- zero kernel faults;
- VirtIO block/GPU/input initialized;
- GPU mode: 1280x800;
- nxadb stable command `pwd`: PASS (`/home`);
- nxadb stable command `ls /sys/services`: PASS;
- screenshot: PASS, 1280x800.

AMD64 clean integration:

- `make clean`: PASS;
- clean graphical `make ARCH=amd64 ... run`: PASS to NXShell;
- KTEST: 34 passed / 0 failed;
- zero kernel faults;
- PCI/VirtIO block/GPU/input initialized;
- GPU mode: 1280x800;
- nxadb serial evidence: `pwd` -> `/home`;
- nxadb serial evidence: `ls /sys/services` -> expected service set;
- screenshot: PASS, 1280x800.

Logs:

- `.git/session1-recovery/logs/pci-generic-aarch64-make-run.log`;
- `.git/session1-recovery/logs/pci-generic-aarch64-nxadb-test.log`;
- `.git/session1-recovery/logs/pci-generic-amd64-make-run.log`;
- `.git/session1-recovery/logs/pci-generic-amd64-nxadb-test.log`.

The standard initial `SHELL-INTERACTION` probe still reports intermittent
FAIL on both architectures. It is not ignored: S1-INPUT-001 records the actual
input/readiness defect and requires a real end-to-end test. The focused PCI
block did not modify the input path and later required commands were executed
successfully on both architectures.

## Execution / TTY / boot-suite checkpoint

Focused/static gates:

- boot-manifest, service-name, demo1 and nxinit project tests: PASS;
- `tools/test_nxtimerdiag_sampling.py`: PASS;
- `tools/test_nxadb_boot_state.py`: PASS, including absent/running/done
  boot-suite states;
- repository `diff --check` for root, NX-CORE, NX-SDK and NX-USERKIT: PASS;
- NX-SDK core cross-build: PASS on AArch64 and AMD64;
- changed `libctest.c` consumer object: PASS on AArch64 and AMD64.

AArch64 end-to-end gate on the final runtime model:

- graphical boot and NXShell: PASS;
- dedicated boot-test TTY: visually verified by QMP screenshot;
- boot suite: 18/19, with only the pre-existing coreutils 42/50 case red;
- `12nxtest_timerqueue`: PASS;
- `18nxtest_timerprobe`: PASS on the clean no-interaction run;
- nxadb result:
  `BOOT-NXTEST=DONE SHELL-INTERACTION=PASS SHELL-UP-HISTORY=PASS
  TIMERDIAG=PASS SHELL-AFTER-TIMER=PASS COUNTER-HEARTBEAT=PASS
  SHELL-AFTER-HEARTBEAT=PASS KTEST=37 passed/0 failed faults=0`;
- GUI child from NXShell: counter pixels remain clean while counter diagnostics
  continue in the parent TTY and UART mirror;
- hosted GUI child: visible GUI remains clean, hidden `nxttyd` supervisor and
  `NX_TTY_PORT` history remain alive, parent shell is not polluted;
- shell job control: `sleep 8 &` -> `jobs` reports Running -> `fg 1`
  waits for completion -> `jobs` reports no background jobs.

One timing run produced severe late wakes while QEMU was manually interacted
with; it is intentionally excluded from qualification.  The subsequent clean,
non-interactive gate passed both boot timerprobe and independent TIMERDIAG.
Timing qualification continues later as its own block.

The exact final tree still needs a fresh AMD64 runtime gate.  AMD64 compile
coverage is green; no final-runtime claim is made here.

## Known stale test expectation

Historical health checks expected a focused window title matching `nxshell`.
The current nxadb flow instead waits for the boot-test TTY lifecycle, dismisses
the completed TTY through its normal Enter path, and only then drives NXShell.
The AArch64 clean gate above demonstrates this focus handoff end-to-end.
