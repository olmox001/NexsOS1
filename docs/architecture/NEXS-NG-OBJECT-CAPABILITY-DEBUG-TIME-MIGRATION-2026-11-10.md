# NexsOS1 NG — Object/Capability, Debug, Time and Service Migration Plan

Date: 2026-11-10
Status: normative execution plan derived from the current DEV-Resurrection source tree.
Branch inspected: DEV-Resurrection.
Baseline commit inspected: 8d8f8a1.
Scope: NX-CORE, NX-HAL, NX-SDK/system library, first-party userland services/tools, build verification and the compositor migration that depends on them.

## 1. Authority and method

This plan is derived from the current source tree and current Git state, not from discarded NXPAX work or Desktop Commander history.

When documents conflict, the more recent explicit architecture decision wins. In particular:

- ASTRA's ring-3 service direction and the 2026-11-03 compositor correction supersede older text that places compositor policy in HAL.
- The current maintainer requirement of one generation-bearing CSpace supersedes the old fd-is-handle/slot-only model.
- POSIX is an NX-SDK/userland personality. NX-CORE must not implement POSIX fd numbering, stdin/stdout/stderr slot conventions, lseek/pipe semantics, errno mapping or redirection policy.
- No compatibility layer may preserve a retired kernel ABI in parallel. A family is migrated completely and its old dispatcher case, assembly veneer and public API are removed in the same verified cutover.
- The project Power12 profile in this document supersedes older project text that requested assertion density. Assertions are forbidden in first-party production and test code. Preconditions, invariants and error paths use explicit checked control flow. ABI layout is verified by generated-contract tests rather than _Static_assert.
- The project must not claim literal NASA/JPL Power-of-Ten conformance where this stricter project profile intentionally changes a rule. The build gate is named NEXS Power12 and records its exact rules.

Every implementation step follows:

1. inspect source, callers and current tests;
2. update this plan if the dependency graph changed;
3. implement one coherent cutover;
4. run semantic/static gates;
5. build both architectures;
6. run the relevant host tests;
7. boot and interact through nxadb where the change is runtime-visible;
8. only then mark the step complete.

No source rename, token-count test, disabled checker, warning suppression, ignored return value or test-specific bypass may convert a failure into a pass.

## 2. Current source-derived baseline

### 2.1 Call boundary

The current public contract defines 60 semantic SYS_* numbers. Both architecture assembly files contain dedicated _sys_* veneers. The kernel dispatcher is a large semantic switch that directly implements filesystem, process, IPC, registry, time, object, display and window operations.

There is currently no 0x4e58 gateway or equivalent unique object gateway in the source tree.

Target:

- one architecture trap selector: NX_GATEWAY_SELECTOR = 0x4e58;
- one assembly entry veneer: _nx_gateway;
- stable operation IDs carried as gateway data, never new syscall numbers;
- object/capability operations are the normal path;
- bootstrap operations are the minimal irreducible set and use the same gateway;
- old semantic SYS_* definitions and stubs are deleted family by family.

The CPU trap instruction remains an implementation mechanism. “Remove syscalls” means remove the parallel semantic syscall ABI and dedicated syscall-number surface, not remove the CPU privilege transition instruction.

### 2.2 Object and capability model

Current defects:

- NPROC_HANDLES is 32;
- a handle is a bare slot index;
- no generation is encoded in a handle;
- close/reuse permits stale integer aliasing;
- one global object_lock covers unrelated operations;
- object allocation, pipes, ports and I/O bounce buffers allocate dynamically at runtime;
- OBJ_TYPE_WINDOW calls kernel compositor policy;
- process handles and ambient PID operations coexist;
- port names use a separate global table rather than the authoritative registry namespace;
- fd 0/1/2 and POSIX redirection are kernel concepts;
- process creation installs stdio into fixed kernel handle slots.

Target:

- one per-process CSpace, with generation-bearing capability handles;
- handle token = slot identity + generation, validated on every lookup;
- no legacy/native partition;
- no reserved fd slots;
- capability entry contains object identity, rights, constraints and generation;
- object lifetime and capability lifetime are separate and explicitly accounted;
- namespace discovery returns capabilities, never ambient PID authority;
- all cross-process authority transfer occurs by capability transfer;
- object operations carry stable Power12 operation IDs and bounded work accounting.

### 2.3 Authority model

Current CAP_* is a five-bit ambient mask. MACHINE/ROOT/USER all receive CAP_ALL; path presets and scattered checks provide much of the actual policy. This is insufficient for service isolation and future multiuser support.

Target:

- a process has a principal/credential identity and a CSpace, not a broad ambient CAP_ALL;
- namespace roots, process factories, registry mutation, display, input, debug, timer-control and service-publication authority are explicit capabilities;
- init receives bootstrap authority and delegates a minimal set to each service;
- service binaries do not gain authority merely because their path begins with /sys/bin or /sys/services;
- user identity and authorization are represented by credential/capability objects that can later support multiple named users without changing the kernel object model;
- nxauth/session policy remains userland; NX-CORE only enforces credential/capability constraints.

### 2.4 Registry and service discovery

Current registry is reachable through direct SYS_REGISTRY, OBJ_TYPE_REGKEY, and regfs; named ports use a separate table. Several services still publish PID registry keys.

Target:

- registry/object namespace is authoritative;
- registry nodes may reference named service endpoints/capabilities;
- direct registry syscall is removed;
- PID service discovery is removed;
- a service publishes a named endpoint object;
- clients acquire a send capability by name;
- endpoint receiver ownership can be handed over during a supervised service restart without changing client authority.

### 2.5 Service layout

Current services and command-line tools are mixed under /sys/bin. nxtimerdiag combines daemon and diagnostics. nxexec combines service and command behavior. Current planning already requires service standardization.

Target source/install split:

- user/sys/services/ — service implementations;
- /sys/services/ — supervised service executables;
- user/sys/bin/ and /sys/bin/ — interactive/system command clients only;
- system/lib/extensions/services/ — modular client libraries;
- libc contains only native base API + POSIX personality, not service protocol implementations.

Initial service set to normalize:

- execution;
- time;
- debug;
- notification;
- compositor/window;
- keyboard/input;
- pointer/input.

No /sys/bin compatibility aliases remain after a service is moved; all callers are updated in the same cutover.

### 2.6 Time subsystem

Current strengths:

- HAL exposes a validated clocksource and clockevent contract;
- Core monotonic time is based on the hardware counter, not jiffies;
- AMD64 and AArch64 have per-CPU clockevents;
- per-CPU snapshots already record counter/tick/late-tick state.

Current defects:

- every user os1_mono_ns() performs a dedicated semantic syscall;
- software timers are an unsorted per-CPU linked list scanned on every 1 kHz tick;
- due callbacks execute while timer_lock is held;
- timer APIs use panic as parameter/invariant handling in places that should return explicit status;
- timer_protocol.c still publishes bind state through volatile/legacy sync primitives;
- nxtimerdiag contains an explicitly unbounded resident loop;
- time service replies through a POSIX pipe;
- service and diagnostic CLI are the same executable;
- ABI headers use _Static_assert, forbidden by the current Power12 policy.

Target:

1. HAL keeps raw clocksource/clockevent facts only.
2. Core preserves the generic intrusive timer abstraction (deadline + callback + opaque data + per-CPU ownership). The queue data structure is selected only after measured comparison of the current intrusive list, an expiry-ordered intrusive list, a cached intrusive red-black timerqueue in the Linux hrtimer style, a binary heap, and a hierarchical wheel. No candidate may narrow timer semantics to process-only wakeups.
3. Queue selection must measure insertion, cancellation, next-expiry lookup, expiry batching, cache behavior and lock hold time at realistic and stress queue depths on both architectures. Power12 bounded-work proofs and zero-allocation runtime operation are acceptance criteria, not reasons to choose a simpler structure.
4. Timer callbacks are detached from the queue under the timer lock and executed after releasing the lock.
5. Clockevents become deadline-driven where the provider implements a genuine one-shot/deadline facility: AArch64 uses CNTV_CVAL_EL0; amd64 must first implement and verify LAPIC one-shot/TSC-deadline capability instead of treating its current periodic LAPIC provider as absolute. A periodic provider remains a verified fallback until then.
6. A read-only time page exports sequence, base counter, base monotonic ns, conversion parameters and clocksource identity. It is updated with a sequence protocol.
7. NX-SDK reads the hardware counter directly only when HAL explicitly declares and configures a user-readable invariant/global source. AArch64 therefore requires verified CNTKCTL_EL1 EL0 counter access; amd64 requires TSC stability and ordering validation.
8. If direct user counter access is unavailable or runtime verification fails, the SDK uses the TIME object gateway path. The fallback is the same semantic clock, not a second implementation.
9. Process CPU time remains authority-mediated because it is scheduler accounting.
10. /sys/services/nxtimerd owns privileged timer telemetry/control and publishes OS1nx_time.
11. /sys/bin/nxtimerdiag is only a test/diagnostic client.
12. Timer service RPC uses capability messages/reply endpoints, not kernel POSIX pipes.

Linux reference material to study/adapt conceptually:

- kernel/time/timekeeping.c;
- kernel/time/hrtimer.c;
- kernel/time/clocksource.c;
- architecture clocksource/clockevent providers;
- vDSO time conversion/seqlock patterns.

Linux is a behavioral reference, not a source-tree dependency.

### 2.7 Debug/fault infrastructure

The discarded NXPAX implementation is not the architecture. Print statements are not a debug infrastructure.

Target three-layer ownership:

NX-HAL:
- architecture trap/debug-register decoding;
- normalized exception/debug event facts;
- safe register-frame capture;
- hardware breakpoint/single-step capabilities where supported;
- no process policy.

NX-CORE:
- per-process/thread debug state;
- bounded event transport with sequence/drop accounting;
- attach/detach authority;
- stop/continue/single-step state transitions;
- register read/write through normalized frames;
- bounded target memory read/write after capability/VM validation;
- breakpoint/watchpoint lifecycle;
- process-exit/fault/debug-stop events;
- no UI or CLI policy.

Userland:
- /sys/services/nxdebugd: privileged broker holding debug-root authority;
- /sys/bin/nxdebug: client/CLI;
- modular system/lib/extensions/services/debug_client.c;
- clients receive an attenuated debug-session capability for one target;
- ordinary clients never gain global process authority or raw kernel addresses.

A debug session is keyed by a process capability plus generation, never a PID alone.

Required operations:
- enumerate authorized processes/threads;
- attach/detach;
- stop/continue;
- single-step;
- read/write registers;
- read/write bounded user virtual memory;
- set/remove software or hardware breakpoints where the target architecture supports them;
- wait/read debug events;
- query process CPU/time/scheduling state;
- obtain a bounded backtrace using user memory validation and architecture unwind policy;
- detect target death and stale capability generation.

Debug event records are binary typed records, not logs. Logs may describe failures but are never the data transport.

### 2.8 Scheduler/process coordination

Current process state mixes process identity, ambient privilege, object/fd table, scheduler state, IPC, environment and window-related state.

Target:

- CSpace is a distinct kernel subsystem referenced by process;
- credentials/principal are a distinct subsystem;
- process scheduling state contains no POSIX fd semantics;
- process creation accepts a capability manifest/startup object, not fd redirection;
- userland nxexec constructs the child startup manifest and transfers capabilities;
- PID remains a diagnostic label, not authority;
- idle is a scheduler CPU state rather than a privileged user-like process where practical;
- scheduler and timer telemetry expose per-CPU run, idle, wake, steal and latency counters for performance qualification.

### 2.9 Compositor

Current migration is incomplete:

- kernel still owns a large compositor/window policy implementation;
- ring-3 nxcompositor snapshots every visible window, copies pixels from kernel, composites, then copies a complete frame back;
- kernel display.c allocates staging memory and performs another user copy;
- object/window paths still call kernel compositor policy;
- user compositor has an unbounded resident loop and dynamic resize allocation.

Target after object/time/service foundations:

- application allocates a BUFFER capability;
- window service receives an attenuated buffer capability plus surface metadata;
- window service owns z-order, focus policy, decorations, geometry and scene graph;
- compositor maps client buffers read-only and composes directly from shared storage;
- output is a compositor-owned shared buffer submitted by capability;
- kernel validates ownership/format/range and presents; no scene policy/rasterization;
- no window snapshot pixel copies;
- no full-frame user-to-kernel staging copy when the backend can scan/present shared pages;
- kernel window/compositor policy and dedicated graphics syscall family are deleted when all clients use the service.

### 2.10 Performance counters

Instrument at minimum:

- gateway calls by operation ID;
- gateway measured cycles/counts where a stable counter exists;
- CSpace lookup hit/miss/stale-generation;
- object ref acquire/release;
- capability transfer count/failure/rollback;
- scheduler context switches, voluntary/involuntary switches, wakeups, steals, idle duration;
- timer queue depth/high-water, insert/cancel/fire, lateness histogram/max;
- per-CPU timer interrupts;
- service requests and queue high-water;
- registry lookups/mutations and traversal work;
- compositor surface count, dirty area, bytes copied/mapped/presented, frames and frame latency.

Counters are bounded integers and never allocate or print from hot paths.

## 3. NEXS Power12 mandatory profile

### 3.1 Scope

The mandatory gate covers all first-party code:

- kernel/, excluding kernel/HAL/references/;
- system/;
- user/sbin/;
- user/sys/;
- first-party files directly under user/bin/;
- project build/tooling that determines trusted artifacts.

Pinned upstream submodules are not rewritten by the first-party checker. Project-owned ports, overlays and adapters around them are covered. Upstream binaries still pass functional integration gates.

### 3.2 Rules

P12-R1. No goto, setjmp/longjmp or direct/indirect recursion in first-party trusted code.

P12-R2. Every loop has a statically or machine-verifiably bounded maximum. Resident services use bounded request epochs and explicit endpoint handover/restart; an unbounded service loop is not an exception.

P12-R3. No general-purpose dynamic allocation after subsystem initialization. Runtime storage uses fixed pools, preallocated arenas, bounded page catalogs or caller-provided buffers. Bootstrap allocation roots are explicit and callgraph-verified.

P12-R4. Production functions are at most 50 physical source lines. Generated code is verified separately and identified as generated.

P12-R5. Assertions are forbidden. This includes assert, KASSERT, NX_ASSERT, _Static_assert and decorative panic-as-assert use. Recoverable validation returns typed errors. Boot-fatal/unrecoverable hardware loss uses a dedicated fatal path. ABI/layout invariants use generated contract comparison and executable layout tests.

P12-R6. Variables and mutable state use the narrowest viable scope. Global mutable state requires ownership documentation and synchronization.

P12-R7. Public boundaries validate parameters; every fallible result is consumed. No discarded fallible call and no result-discard macro.

P12-R8. Preprocessor use is limited to includes, guards and simple object-like build constants. No hidden control flow, token pasting or variadic behavioral macros in trusted code.

P12-R9. Authority is never represented by a raw pointer. Pointer depth and function-pointer use are constrained at explicit HAL/provider boundaries. User/wire pointers are copied/validated before use.

P12-R10. Warning-clean builds and static analysis are mandatory. First-party code may not use -Wno-error to pass. The strict warning profile includes conversion/sign-conversion after each owned domain is corrected. clang/cppcheck findings must be zero.

P12-R11. Material public operations have stable operation IDs and checked work accounting. Measured cycles are reported as measured cycles; no fabricated accounting.

P12-R12. Every finite capacity/limit has machine-readable provenance: hardware limit, resource budget, security policy or scheduling quantum. Larger logical work uses cursors/chunking/scatter-gather rather than an arbitrary private ceiling.

### 3.3 Gate implementation

Replace the NXPAX-only checker with tools/power12/:

- policy.json: owned roots, external exclusions, bootstrap allocation roots, provider function-pointer boundaries, normative limits and operation IDs;
- audit_ast.py: clang AST/callgraph rules R1-R9 and function spans;
- audit_limits.py: R11/R12 operation/limit provenance;
- audit_build.py: warning flags and forbidden suppressions;
- audit_asm.py: architecture entry surfaces and unique gateway selector;
- test_auditors.py: mutation tests proving each checker detects structural violations.

make power12 becomes a prerequisite of normal first-party build targets, not only release/check.

The checker is never relaxed to accommodate a failing source file. Source is fixed until the gate passes.

## 4. Execution phases

### Phase A — truthful hard gate

A1. Replace the NXPAX-only Power12 checker with repository/domain-aware tooling.
A2. Update Power12 documentation to the rules above.
A3. Remove assertion infrastructure from first-party production/tests and replace it with explicit error/test return paths.
A4. Remove first-party -Wno-error; separate upstream build flags from first-party flags.
A5. Generate a complete failure inventory and resolve it domain by domain.
A6. Make power12 mandatory in normal builds.

Acceptance:
- mutation tests for every automated rule pass;
- a deliberate real-C violation makes make power12 fail;
- first-party source passes with no file-by-file bypass.

### Phase B — timer and observability foundation

B1. Refactor HAL timer protocol publication to atomics and typed status.
B2. Replace Core timer panic-as-validation with explicit status/fatal separation.
B3. Instrument the current per-CPU intrusive-list timer and benchmark it against expiry-ordered intrusive list, cached intrusive RB timerqueue, heap and wheel candidates without changing timer semantics.
B4. Select and implement the queue only from measured evidence; preserve generic callbacks and remove callback-under-lock behavior.
B5. Add scheduler/timer counters and latency/lateness accounting.
B6. Implement clockevent next-deadline programming only after provider capability is real and verified.
B7. Implement read-only time conversion page and architecture user-counter support with runtime verification.
B8. Update NX-SDK monotonic/clock_gettime/SDL/Lua time paths to use the common fast path.
B9. Split nxtimerdiag into /sys/services/nxtimerd + /sys/bin/nxtimerdiag; remove pipe RPC.
B10. Add timer stress, cross-CPU monotonic, migration, lateness, overflow and fallback tests.

2026-11-10 measured checkpoint:
- B3 instrumentation is implemented on the existing generic intrusive list:
  allocation-free per-CPU arm/cancel/fire/scan/depth/high-water accounting is
  exported through a privileged OS1_NS_TIME `stats` capability.
- The TIME kernel/SDK wire contract is generated from
  `kernel/contracts/time-v1.json`, hash-checked, and layout-checked for amd64
  and AArch64.
- amd64/iso-bios passes nxadb with KTEST 26/26, TIMERDIAG PASS and faults=0.
- aarch64/iso-uefi passes nxadb with KTEST 27/27, TIMERDIAG PASS and faults=0.
- observed queue high-water is 5-7 timers and average active-CPU scan work is
  roughly 2.3-3.2 nodes per expiry pass in the current diagnostic workload.
- This evidence rejects the previous process-only heap cutover. B4 remains
  OPEN: no queue replacement is justified yet, and callback-under-lock cannot
  be removed until TIMER-UAF-01 quiescence is replaced by an explicit proven
  FIRING/cancellation state protocol.
- Remaining B3 work is comparative stress measurement of ordered-list,
  timerqueue/RB, heap and wheel candidates; no candidate is allowed to narrow
  the generic callback/data timer contract.

Acceptance:
- monotonic never regresses across CPU migration;
- fast path and gateway fallback agree inside measured read brackets;
- no timer callback executes under timer queue lock;
- idle interrupt/context-switch rate improves or measured evidence identifies the remaining source;
- both architectures pass clean rebuild + nxadb timer diagnostics.

### Phase C — one generation-bearing CSpace and object core

C1. Introduce neutral internal object/capability/CSpace types; no POSIX or wire-version names in object core.
C2. Implement generation-bearing handles and stale-handle tests.
C3. Replace global object lock with CSpace/object-local synchronization and explicit lock ordering.
C4. Replace runtime object heap allocation with bounded pools/arenas derived from process/resource budgets.
C5. Add typed capability constraints and transfer attenuation.
C6. Introduce the single gateway trap/adapter and _nx_gateway.
C7. Migrate handle/object primitives first; delete their dedicated SYS_* numbers/stubs in the same change.
C8. Add capability startup manifests for new processes.
C9. Move fd table/stdin/out/err/lseek/pipe/redirection entirely to NX-SDK/userland.
C10. Remove fixed kernel stdio slots and kernel POSIX fd vocabulary.

Acceptance:
- stale handle cannot resolve after close/reuse;
- transfer rollback and concurrent close/transfer stress pass;
- object hot path contains no general heap allocation;
- no fd/POSIX identifier remains in NX-CORE object/scheduler interfaces;
- only the gateway selector enters semantic kernel operations from userland.

### Phase D — registry, services and authority

D1. Make registry/object namespace authoritative; remove direct registry syscall.
D2. Replace separate named-port table and PID service registry keys with endpoint objects discoverable in registry.
D3. Move supervised services to /sys/services and clients/tools to /sys/bin.
D4. Introduce service manifests specifying exact delegated capabilities.
D5. Replace broad CAP_ALL/path-based privilege with credential + capability authority.
D6. Update nxexec to create children through process-factory authority and startup capability manifests.
D7. Update all service callers and remove legacy discovery.

Acceptance:
- service restart preserves/rebinds endpoint discovery without PID lookup;
- an unprivileged process cannot replace another service endpoint;
- each service has the minimum capability manifest needed by tests;
- no /sys/bin path grants implicit root authority.

### Phase E — nxdebug

E1. Define neutral HAL debug event/register primitives for amd64/aarch64.
E2. Add Core debug-session objects and debug rights.
E3. Add per-process/thread stop/continue/register/memory/event operations.
E4. Add architecture single-step and breakpoint/watchpoint support.
E5. Implement /sys/services/nxdebugd.
E6. Implement modular debug client library and /sys/bin/nxdebug.
E7. Add fault, breakpoint, stale-target, unauthorized attach, concurrent exit and memory-boundary tests.

Acceptance:
- attach uses process capability, not PID authority;
- register and memory reads correspond to the stopped target;
- target exit yields a typed event and invalidates session generation;
- unauthorized debug is rejected;
- intentional user fault is captured and the system stays interactive on both architectures.

### Phase F — remaining semantic syscall families

Migrate in dependency order and delete the old family in the same verified pass:

1. process/wait/exit/yield;
2. IPC/port/capability transfer;
3. registry;
4. time/sleep;
5. filesystem/path;
6. VM/memory;
7. input;
8. graphics/window/display;
9. statistics/debug.

Every family receives operation IDs, capability requirements, bounded work semantics and NX-SDK wrappers before the old number/stub is removed.

Acceptance:
- system/include/contract/syscall_nums.h is deleted;
- architecture user assembly exports _nx_gateway plus architecture runtime primitives only;
- dispatcher no longer contains a semantic syscall switch.

### Phase G — compositor separation and zero-copy

G1. Replace legacy window snapshots with shared buffer/surface capabilities.
G2. Move remaining window policy/state from kernel compositor to service.
G3. Give compositor read-only mappings of client buffers and a compositor-owned output buffer.
G4. Replace display staging copy with capability-backed present.
G5. Move focus/z-order/decoration/terminal policy to userland services/libraries.
G6. Delete kernel compositor raster/policy code and graphics semantic syscall family.
G7. Add damage/present/resize/window-lifetime stress and performance tests.

Acceptance:
- no scene rendering or window policy remains in NX-CORE/HAL;
- normal frame has zero window snapshot copies and no full-frame user-to-kernel staging copy;
- resize does not allocate in IRQ/fault path;
- compositor crash/restart preserves kernel-owned buffers safely.

## 5. Required test matrix

For every architecture-affecting milestone:

1. make clean;
2. mandatory make power12;
3. full first-party build with strict warnings;
4. HAL host tests;
5. HAL sanitizer tests;
6. architecture freestanding link;
7. rootfs/image build;
8. boot with nxadb;
9. wait for NXShell;
10. execute a real shell command and verify output/exit status;
11. run milestone-specific service client tests;
12. inspect complete serial/QEMU/build logs for warnings, faults and unexpected fallback;
13. preserve measured before/after performance counters.

Final gates require both:
- amd64 GRUB/BIOS default lane;
- AArch64 GRUB/UEFI default lane.

Additional lanes remain required where the existing release plan marks them required.

## 6. Immediate next work

1. Phase A: replace the false NXPAX-only Power12 gate and generate the real first-party inventory.
2. Resolve Power12/build failures in the timer + object + scheduler dependency slice needed by Phase B/C.
3. Phase B timer refinement and service split.
4. Phase C generation-bearing CSpace + single gateway.
5. Phase D registry/service/capability authority.
6. Phase E nxdebug.
7. Continue family cutover through Phase F.
8. Phase G compositor.

No compositor rewrite begins before the object/buffer/time/service foundations it depends on are verified.

## 7. Baseline evidence — 2026-11-10

make ARCH=amd64 check: FAIL.
make ARCH=aarch64 check: FAIL.

Both fail in the staged NXPAX-only Power12 gate. Mutation tests pass, but the checker reports an unproved resident service loop, a pointer-model finding and eight deliberately unresolved proof classes. That is not a valid mandatory project gate.

Other baseline findings:

- 60 semantic SYS_* definitions exist;
- no unique 0x4e58 gateway exists;
- object handles are slot-only and non-generational;
- kernel object code implements POSIX fd/pipe/lseek/redirection semantics;
- user time reads use semantic syscalls;
- timer callback execution occurs under the per-CPU timer lock;
- service sources and command clients are mixed under user/sys/bin;
- registry has parallel syscall/object/regfs access and separate port-name discovery;
- kernel compositor and ring-3 compositor overlap responsibility and copy surfaces/frames across the boundary;
- first-party build flags still contain warning demotion;
- assertions/static assertions remain in first-party code and tests.

These are baseline failures to fix. They are not accepted exceptions.
