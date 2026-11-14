# HAL / NXOLMO / NXEC ISA-state convergence — implementation plan

Date: 2026-11-10. Branch: DEV-Resurrection. This plan is based on the live
source manifests and provider/header dependencies, the NXOLMO CMake component
graph, and the current ASTRA, HAL/STATE and NXOLMO primary/secondary documents.
This is an execution plan, not evidence of completed migration or qualification.

## Objective and authority

Make one coherent resource/state/array/registry execution model, preserving the
working kernel, HAL, drivers, VFS, ABI and services while replacing existing
implementations function by function. Do NOT compile a second independent
NXOLMO runtime, capability engine, block cache or registry in parallel with
the authoritative path. All twelve NXOLMO source areas are in scope:
authority, base, block, buffer, crypto, filesystem, hardware, imports,
journal, protocol, registry, runtime. Reference code is studied and adapted,
not blindly linked: imports includes upstream and prior NexsOS1 copies.

Kernel code consumes ISA-neutral HAL public contracts. HAL provider code owns
native register layouts, assembly, MMU, context switch, trap, IRQ hardware
details. A single typed operation/state/resource protocol joins these
contracts to NXOLMO's bounded semantics, NXEC HCD and actual HW authority.
Shared registry object IDs/generations are not themselves capabilities.
Kernel grants and validates authority on admission and on side effects.
A logical canonical state service and a small privileged authority index must
not become competing registries. Do not put POSIX or filesystem path policy
in hardware providers. The complete filesystem and 9P projections consume
the same canonical objects and transactions.

## Existing NexsOS1 architecture (live implementation)

* HAL/arch/{amd64,aarch64}: boot stages, native frames, traps/syscalls,
  context, MMU/TLB, hardware timer/IRQ, UART and VirtIO transport.
* HAL/core: diagnostics, IRQ, CPU/SMP, time, fault, memory, resource grants,
  PCI, frame and NXEC/HCD; compiled into one selected libnxhal.a.
* HAL/capability, HAL/common: NXEC preboot ISA discovery, HCD validation,
  boot memory and hardware-access policy.
* kernel/{core,sched,mm,lib,drivers}: object tables/capability handles,
  process/scheduler, PMM/VMM, block buffer, registry, ext4 VFS, I/O.
* system/: SDK, cross-ISA user entry/assembly and public contracts;
  user/: service and program personalities; boot/: GRUB packaging.
* HAL is not independent yet: HAL/Makefile and kernel/Makefile consume private
  kernel and provider includes, HAL/core/frame.c and core/core.c select arch,
  providers include kernel internals, and device drivers still live in HAL.
  HAL/core/resource.c duplicates semantics of NXOLMO hardware/capability.c
  but the latter additionally needs authenticated grant derivation. Never
  equate either raw descriptor validation with enforced MMIO/IRQ/DMA authority.
* Existing kernel/lib/registry.c, kernel/core/object.c, kernel/mm/buffer.c,
  kernel/fs/vfs.c and kernel/fs/ext4.c are actual runtime owners; replacing
  their functionality requires call-site and guest-boot equivalence tests.

## NXOLMO source graph and migration ownership

| Module | Existing NXOLMO function family | Integration and replacement obligation |
| --- | --- | --- |
| base | checked math, spans, status, IDs | unify with HAL/kernel error and ID contracts; remove duplicate arithmetic |
| crypto | CRC32C, CRC64, SHA256 | compare kernel/lib and NXEC HCD crypto; maintain wire/disk vectors |
| runtime | budgets, metrics, clocks, op catalog | use actual HAL clocksource; count real work/cycles; integrate Power12 R11/R12 |
| authority | capability HMAC, issue/verify/attenuate | converge with kernel CSpace and revocation, not a second credential engine |
| hardware | resource descriptor, verified grant/range/DMA | replace HAL/core/resource.c by semantic equivalence, preserve stricter checks |
| registry | State Store tree, indexes, NXREG, schema, snapshots | converge kernel/lib/registry.c and object authority with one canonical state |
| buffer | arena, integrity, leases, ring | reconcile kernel/mm/buffer.c, graphics surfaces, DMA lifetime; no double cache |
| block | block device, memory device, backend | integrate kernel/drivers/block and VirtIO; preserve real read/write and flush |
| journal | WAL and recovery | integrate real ext4 write/sync and authoritative state commit |
| filesystem | ext4 + Linux 9P algorithm snapshot | reconcile existing ext4/VFS providers; full semantics and real-image tests |
| protocol | native 9P envelope | extend to complete 9P and NXEC REG bridge; independent NXREG clients |
| imports | Redis, base-nexs, NXEC, prior NexsOS1, Linux | provenance, algorithm comparison, license and binary behavior; never ship clones |

Source state: NXOLMO's native ext4 has tested metadata/parser components,
not a complete VFS. Its 9P frame code covers an envelope; Linux 9P sources
are not a compiled native provider. The H0 State Store still requires
namespace/path, transaction and complete authority integration. Its reference
worktree currently has local changes and must remain untouched.

## Required execution order: real cutovers only

0. Source census: every native C/ASM/header symbol, caller, owner, ABI,
   tests, alias, allocation, lock and failure path. Record binary provenance,
   licensing and source hashes before importing any module. Never modify the
   NXOLMO/NXEC/base-nexs/Linux reference trees.
1. ISA boundary: normalize per-arch frame layouts and assembly entry,
   remove provider selections from HAL/core, derive checked assembly offsets,
   retain all syscall/fault/context behavior. Move low-level driver policy out
   of architecture providers; resource transport stays in HAL.
2. HAL-independent build: remove private kernel headers and Core callback
   reverse edges from HAL. One typed inward event protocol and outward
   resource/ISA operations; no generic kernel raw register access. Prove the
   archive's unresolved symbol policy and both freestanding links.
3. NXOLMO base/crypto/runtime convergence: replace existing arithmetic,
   hashing and operation reports only after vectors and overflow tests.
   Reuse NXEC discovery/HCD, not a second ISA detector.
4. Authority/hardware/resource convergence: one authenticated capability,
   generation, scope, lease, revocation and resource-grant chain for device
   enumeration -> mapping -> IRQ -> DMA -> completion. Convert the current
   HAL resource operations and kernel CSpace atomically per operation family.
5. Canonical registry/state model: adapt NXOLMO State Store/index, NXEC REG
   and base-nexs typed values/bitcode; retain a minimal privileged authority
   index, eliminate duplicated mutable registry owners only after migrations.
   Develop one bounded brace-delimited typed instruction/transition language,
   with explicit verifier, versioned wire format, fuel, capability requirements
   and rollback; no native executable code from untrusted input.
6. Buffers/block/WAL: one shared lease/extent and transaction model, real
   flush/barrier/replay, cancellation, per-device capability validation, no
   independent copies of cache or allocator; preserve existing disk boot path.
7. Filesystem / VFS / 9P: align current working ext4 provider with NXOLMO
   verified parsers and Linux conformance; put filesystem policy in services
   as described by ASTRA/NXEC, not in ISA providers. 9P wire codec and VFS
   algorithms are independent NXREG consumers; no alias-only switchers.
8. SDK/app and build cutover: all call sites migrate to the one shared object
   operation interface, no legacy semantic syscall family retained in
   parallel. Keep real shell, timers, compositor, input and diagnostics.
9. Full hardening/qualification: R1-R12 including operation and limit
   provenance, 2 meaningful tests per kernel function, concurrency/revocation
   cases, negative/adversarial cases, full boot and image interoperability.

## For EACH source/function migration

Inspect reference and live definition, all callers, ABI/types, locks, ownership,
allocation, error semantics, tests, performance, Power12 obligations. Record
{current path, reference path, authoritative destination, equivalent behavior,
missing semantics, test IDs}. Implement all missing semantics before dropping
old symbols. One tested link-visible implementation per operation. Do not use
compatibility macros as a substitute for conversion. Restore no deleted code
without checking dependencies and current behavior.

Minimum development gate: make check-kernel-contracts; changed-file Power12
and full Power12 strict (genuine failures reported, not suppressed); HAL host,
cross, sanitizer and static tests; isolated AMD64 and AArch64 compilations;
real make run / NXADB boot, interactive shell, /bin/crash recovery, NXPAX,
notifications, timer diagnostics, VFS/Ext4 read/write and GUI input as
appropriate. Full clean and five-lane boot gates before milestone closure.
Keep true log/exit status and performance baseline; stage only verified units.

## Current verified state at plan creation

Git DEV-Resurrection HEAD 6442d0e was clean before this migration.
Existing HAL/Core baseline has build evidence, but full strict Power12
qualification is RED. No NXOLMO source module has yet been rehomed or
substituted by this plan. Existing NXOLMO reference has dirty private files.
This first change extracts an existing native frame implementation into the
selected provider without altering the public frame ABI. This is ONLY the
first ISA-boundary increment, not completion of steps 1-9.

## Progress log

* [x] Initial existing build/source architecture census and dependency review.
* [x] NXOLMO module/source/target graph and major scope/restriction review.
* [ ] Exhaustive function-level equivalence inventory across all components.
* [x] ISA provider frame migration compile and ISO guest acceptance (AMD64 and AArch64); direct/other firmware lanes remain open.
* [ ] Full HAL independent build and symbol boundary proof.
* [ ] NXOLMO module integration (twelve families).
* [ ] Kernel/SDK/VFS/9P/Ext4 cutover and end-to-end qualification.
* [ ] Full Power12 and dual-ISA acceptance.


## ISA frame migration acceptance — 2026-11-10

kernel/HAL/core/frame.c was retired. The selected provider-native
arch/<isa>/cpu/frame.c now implements the existing hal/frame.h entrypoints.
HAL/sources.mk selects exactly one native frame object, with no parallel
generic version. Original register mapping and task initialization preserved.

Two new HAL/tests/unit/frame_{amd64,aarch64}_test.c tests cover user/kernel
frames, args/results, retries, PC/SP, snapshots and NULL. Actual native frame
code is linked to each host and ASan/UBSan binary. The AST driver was amended
to parse host tests with the same true kernel/ISA header set as the compiler:
no rule is exempted. Checked initialization uses the memset return value.

Verified evidence:
- Kernel build: AMD64 PASS, AArch64 PASS. Conversion warning debt persists.
- Full isolated images: AMD64 GRUB BIOS ISO PASS, AArch64 GRUB UEFI ISO PASS.
- HAL host suite PASS; HAL ASan/UBSan suite PASS.
- check-kernel-contracts PASS.
- changed Power12 gate PASS, 0 scoped findings; changed gate mutation PASS.
  Full strict project Power12 remains RED.
- NXADB AMD64 ISO BIOS: shell/history/timer/heartbeat/post-heartbeat/NXPAX
  passed, 30/30 kernel tests and zero unexpected faults.
- NXADB AArch64 ISO UEFI: same functional checks passed, 31/31 kernel tests
  and zero unexpected faults.
- A failed initial guest start was infrastructure-only: the long QMP pathname
  under build/halnx-migration exceeded macOS Unix socket length (109 bytes).
  The isolated root was relocated to build/h (95/97 bytes) and both boots
  then passed.
- The initial host test make/shell-variable and header parsing errors were
  corrected; full host, sanitizer and changed Power12 tests were rerun.

Logs (not Git-tracked): /tmp/halnx-frame-{host,san,changed}-tests-20261110.log,
 /tmp/halnx-guest-{amd64,aarch64}-20261110.log, build/h/runtime/nxadb.
This is not full HAL independence, NXOLMO integration, full firmware matrix,
complete fault-matrix acceptance or Power12 qualification.

## NXOLMO base/checked → HAL authoritative resource path — 2026-11-10

This is the first real NXOLMO runtime component cutover. The project-owned
HAL/base/checked.c contains the complete eight-operation NXOLMO checked-arithmetic,
range, pointer-overlap and span validation family with the canonical NXOLMO
status/span/checked API in HAL/include/nxolmo/base. The HAL owns one compiled
implementation; the NXOLMO reference worktree is not linked or modified.
The C++-conditional declarations were removed from HAL's C-only headers to
comply with Power12 R8; no rule was exempted.

HAL/core/resource.c now uses canonical nx_u64_add for overflow-safe resource
extent formation, nx_range_validate for grant-relative offsets, and
nx_pointer_ranges_overlap for the actual resource/grant/report alias checks.
The previous hand-rolled nxhal_spans_overlap function and hand-rolled range
math are removed, not retained behind duplicate adapters. Existing HAL
status/error semantics (notably E_OVERFLOW, E_LIMIT, E_INVALID) are preserved;
negative or unknown overlaps fail closed.

New HAL/tests/unit/checked_test.c exercises the actual base source, positive
and negative arithmetic, range overflow/capacity, null pointers, buffer overlap
and disjoint buffers. HAL host resource and mutation tests now link that actual
same source. The freestanding HAL resource relocation links the source in
both ISAs, and HAL host, sanitizer, cross and static analysis targets exercise
this exact implementation. No mocks or artificial PASS markers.

Verified:
- HAL host+cross PASS, including genuine 100,000 resource mutation tests and
  x86_64-elf / aarch64-none-elf freestanding resource cross-link.
- HAL ASan/UBSan PASS with actual checked tests; clang-tidy/cppcheck PASS
  after fixing direct include ownership in the HAL-owned files.
- Both kernel freestanding builds PASS, conversion warning debt remains.
- NXADB AMD64 GRUB BIOS ISO PASS: 30/30 KTEST, 0 faults, shell/history,
  timerdiag/heartbeat, NXPAX.
- NXADB AArch64 GRUB UEFI ISO PASS: 31/31 KTEST, 0 faults, same full suite.
- Changed Power12 gate PASS on 14 scoped source/test files with zero findings,
  and changed-gate mutation controls PASS. Full project-wide strict Power12
  remains unqualified and RED.

Evidence: /tmp/halnx-base-host-cross-20261110.log,
 /tmp/halnx-base-sanitize-20261110.log,
 /tmp/halnx-base-static-20261110.log,
 /tmp/halnx-base-delta-20261110.log,
 /tmp/halnx-base-guest-{amd64,aarch64}-20261110.log,
 and full UART archives under build/h/runtime/nxadb.

Not completed: NXOLMO base/types/status/result as full module, crypto,
runtime/budget, hardware token authority, canonical registry state,
transaction WAL, buffers, block I/O, VFS/Ext4, 9P and ISA assembly rewrite.
The six stage migration obligations remain open until their actual callers,
security boundaries and system tests have been converted and verified.

## NXOLMO base/types/result/status cutover — 2026-11-10

HAL/base/{types,result,status}.c and the corresponding C-only
HAL/include/nxolmo/base headers are now actual HAL link inputs. The
reference worktree is unchanged; reference content hashes were
recorded during source review. The typed status/result schema keeps
the NXOLMO wire/ABI numeric values. Flags use explicitly unsigned
bit masks after genuine clang-tidy signed-bitwise failures were fixed;
no warnings, assertions or policy checks were disabled.

Resource identity now converts only the unchanged public HAL
resource-ref fields into a canonical nx_object_ref at the ABI seam.
Validity, equality, object ID zero and generation validation are
delegated to real nxolmo/base/types.c functions, replacing the
hand-coded duplicate identity comparisons. There is one generation
validation algorithm, not a parallel CSpace or registry.

The complete base result/status functions are present and tested,
but their public use by the future canonical operation/metrics
runtime is a separate OPEN dependency. Do not claim that kernel
process CSpace, encrypted tokens or registry ownership has migrated.

Acceptance:
* Both cross-ISA kernel compilation: PASS.
* HAL full host suite PASS, including actual resource mutation tests,
  base round-trip/version/invalid-flag/ABI cases, and zero fake cases.
* HAL freestanding resource/base cross-link x86_64-elf/AArch64-none-elf:
  PASS; HAL ASan/UBSan PASS; full clang-tidy and cppcheck PASS.
* Incremental Power12: PASS 20 scoped files and zero findings; test
  suite for changed gate PASS. Project-wide strict gate remains RED.
* NXADB AMD64 ISO BIOS: PASS, 30/30 kernel tests, 0 faults,
  shell/history/timerdiag/heartbeat and NXPAX probe.
* NXADB AArch64 ISO UEFI: PASS, 31/31 kernel tests, 0 faults,
  same guest probes. No other boot lane claimed.

Logs: /tmp/halnx-base2-host-cross.log,
 /tmp/halnx-base2-sanitize.log,
 /tmp/halnx-base2-static.log,
 /tmp/halnx-base2-delta.log,
 /tmp/halnx-base2-guest-{amd64,aarch64}.log,
 and build/h/runtime/nxadb/ serialized QEMU/UART logs.

### Runtime integration prerequisite: one authoritative HAL clocksource

NXOLMO's runtime/clock.c currently uses Mach/host CLOCK_MONOTONIC,
while runtime/cycle_counter.c directly uses host X86 CPUID/RDTSCP.
These are unsuitable as-is inside freestanding HAL or for AArch64.
HAL/core/time.c already validates provider clocksource metadata and
exposes nxhal_time_clocksource_info/nxhal_time_counter_read. Its
not-ready/error semantics and frequency/mask/continuity must be
preserved. Any NXOLMO runtime clock, cycle, budget or metrics cutover
must derive genuine counters from that HAL provider and explicitly
represent unknown cycles, unavailable time and wrap/frequency limits.
No synthetic timestamps, extra clocksource, or unbounded polling.

NXOLMO runtime/operation_catalog.c labels hardware validation op IDs
0x00070001, 0x00070003, 0x00070004, 0x00070005 as OPEN contracts.
They are not proven fixed formulas. The existing HAL resource report
has real per-operation work steps with bounds 11/6/8/3 respectively.
A full cutover must reconcile these actual report operations and
R11/R12 provenance with the native catalog, then integrate actual
budget status, work and cycle measurements. Do not claim an OPEN
operation contract has a proven bound.

Next: compare every runtime source/caller/ABI and HAL clock provider;
implement an ISA-neutral validated counter and budget path with tests
for source unavailable, CPU migration, wrap and timeout, then replace
the report policy only after behavior tests on both guests. A
metrics-only duplicate installed without real consumers is forbidden.
