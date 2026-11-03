# NEXS-NG Microkernel Core Design

Status: design specification; no NG kernel source has been implemented yet.  
Depends on:
- ABI v1: docs/architecture/ABI-COMPATIBILITY-MATRIX.md
- Hardware and isolation: docs/architecture/NEXS-NG-HARDWARE-AND-ISOLATION.md
- Build and backup baseline: docs/architecture/BUILD-BASELINE-2026-11.md

## 1. Design objectives

NEXS-NG replaces the current monolithic kernel with a small, auditable,
multicore microkernel. The kernel provides isolation and mechanisms; userland
services provide policy and complex subsystems.

The design must:
1. preserve the existing userland ABI v1 without changing its source;
2. boot x86-64 and AArch64 from the existing QEMU targets before adding real
   hardware boot paths;
3. keep the trusted computing base small and separately testable;
4. support multiprocess and multithread execution, SMP, capabilities, IPC,
   sandboxed drivers and restartable services;
5. support optional hardware virtualization without requiring it to boot;
6. make every software-controlled operation bounded by explicit limits;
7. report unknown hardware honestly and fall back safely;
8. preserve the GPLv2 project license and audit every imported source file.

## 2. Repository and build isolation

Until the future repository split, the new kernel lives beside the legacy one:

- kernel/: original kernel, frozen and retained.
- kernel-ng/: clean-room replacement kernel.
- include/abi/: frozen legacy UAPI during transition.
- include/api/: current userland API; unchanged for v1.
- user/: original userland; unchanged for v1.
- external/: future landing area for separately pinned external libraries.
- sdk/: future landing area for stable UAPI, headers, startup objects and libos1.
- services/: new service binaries and service-specific libraries.

The first NG target is explicit (for example, make kernel-ng ARCH=amd64); it is
not part of make all until it can boot independently. The default legacy build
continues to work throughout bring-up.

The eventual repositories are:
1. nexs-kernel — kernel core, architecture providers, boot protocol and tests;
2. nexs-userland — applications, init and userland services;
3. nexs-sdk — UAPI headers, libos1, startup objects, ABI tests and toolchain SDK;
4. nexs-external — individually pinned third-party libraries and compatibility
   sources.

The superproject remains the reproducible integration manifest, pinning exact
commits of those repositories and external libraries. Splitting repositories
must not change ABI or source semantics.

## 3. Trusted core and service boundary

### 3.1 Kernel mechanisms only

The kernel core owns:
- CPU exception entry/return and context switching;
- thread TCB and process address-space identity;
- per-CPU scheduler and wait queues;
- user/kernel address-space isolation and page tables;
- physical frame allocation required for boot, kernel objects and mappings;
- capability table and object reference lifecycle;
- bounded IPC, notification, semaphore and shared-memory mapping primitives;
- interrupt routing, IPI and minimal timer/clocksource;
- safe user-memory copy, pin/unpin and DMA mapping;
- the v1 syscall compatibility dispatcher.

The kernel does not own:
- filesystem policy, Ext4, path resolution, file cache or mount policy;
- registry contents or namespace policy;
- compositor, windows, fonts, terminal UI or desktop style;
- network stack, audio policy or service discovery;
- device matching and driver policy;
- general user heap allocation policy;
- RTC synchronization, thermal policy or voltage/frequency policy;
- generic USB, PCI, storage, network, GPU or audio drivers.

### 3.2 Services

Initial service processes:
- system_init: bootstrap and supervise early services;
- devmgr: normalized device graph and driver matching;
- blockd: block device queue and block I/O interface;
- vfsd: namespace, mounts, file descriptions and filesystem providers;
- registryd: key/value and service registry;
- processd: process creation policy and legacy ELF spawn adapter;
- compositord: windows, drawing, framebuffer presentation;
- displayd: modes, display discovery and display power;
- inputd: HID/input event aggregation;
- fontd: font resources and raster data;
- powerd: thermal, power and voltage providers;
- timed: wall clock, RTC synchronization and higher-level timers;
- driver services: one isolated process per device/function or safe group.

Services use only the new kernel primitives and their granted capabilities.
They must not call their own legacy compatibility syscall in a way that causes
a recursive kernel-to-service RPC.

## 4. Boot protocol and service bootstrap

### 4.1 BootInfo contract

Bootloader-specific formats terminate at a small adapter that produces a
versioned BootInfo structure containing:
- ABI/version and structure size;
- memory map, reserved ranges and framebuffer;
- ACPI RSDP and/or DTB physical address and validated length;
- initramfs/module list with hashes and bounds;
- boot CPU identity, topology hints and firmware mode;
- entropy seed if supplied by a trusted source;
- command line, boot mode and selected root device;
- optional EFI memory map and runtime-service policy.

No bootloader pointer is trusted without range, alignment and ownership checks.
The kernel copies or pins BootInfo data before the bootloader's memory can be
reclaimed.

### 4.2 Bootstrap order

1. Enter architecture baseline mode, establish exception vectors and per-CPU
   boot stack.
2. Parse only memory, CPU, interrupt-controller and timer bootstrap data.
3. Initialize frame allocator, kernel page tables, W^X, per-CPU structures,
   console and panic logging.
4. Bring up the boot CPU scheduler and timer.
5. Discover and start secondary CPUs; each CPU reaches ONLINE only after MMU,
   interrupt, timer and scheduler state are ready.
6. Create the first service address space and start system_init from initramfs.
7. Start devmgr and boot-critical drivers (console, block and timer providers).
8. Start vfsd from initramfs; discover and mount the installed root filesystem.
9. Start registryd, processd, inputd, displayd, compositord, fontd, powerd and
   timed according to available hardware.
10. Start the unchanged legacy /sys/bin/init from the mounted rootfs. It then
    starts the existing notification server, UI, launcher and shell unchanged.

### 4.3 Installer versus installed mode

The boot supervisor distinguishes:
- installer/rescue: read-only or ephemeral initramfs root, no persistent
  configuration assumed, explicit target-disk capabilities;
- installed: mount the selected persistent root, restore service configuration,
  and start normal supervision.

The kernel itself does not change architecture based on installer mode. The
difference is service policy, root namespace and persistence.

## 5. Thread, process and scheduler model

### 5.1 Separate process and thread

A process owns:
- address space;
- namespace root and cwd reference;
- capability table;
- credentials/security level and quotas;
- process-wide signal/event state (future);
- process accounting and service identity.

A thread owns:
- saved architectural register state;
- kernel stack and user stack pointer;
- scheduling class, priority and affinity;
- wait state, deadline and wake reason;
- per-thread vector/FP state descriptor;
- CPU-time accounting.

Legacy ABI v1 creates one initial thread per process. New thread APIs are
additive; old process and wait syscalls continue to operate on process IDs.

### 5.2 Thread state machine

Allowed transitions:

NEW -> READY -> RUNNING -> READY  
RUNNING -> BLOCKED -> READY  
RUNNING -> DYING -> DEAD  
BLOCKED -> DYING -> DEAD

Only scheduler-owned transition functions may mutate run-queue state. A thread
cannot be READY on two CPUs. A wakeup racing with cancellation is resolved by a
single atomic state transition and a sequence number.

### 5.3 SMP scheduling

- Per-CPU run queues; no global run queue in the hot path.
- Per-CPU idle thread and deadline timer.
- Explicit CPU affinity and CPU-feature class.
- Work stealing is bounded by a configured number of candidate queues per
  attempt; failure falls back to local scheduling.
- Priority inheritance or bounded priority donation for service IPC.
- Scheduler locks have a documented global order; no service or driver call
  occurs while holding a scheduler spinlock.
- CPU hotplug follows DISCOVERED -> BOOTING -> MMU_READY -> IRQ_READY ->
  TIMER_READY -> ONLINE -> DRAINING -> OFFLINE.

## 6. Memory and address-space design

### 6.1 Physical memory

A minimal kernel frame allocator is unavoidable for boot, page tables, kernel
stacks, TCBs, capability tables and mapping operations. Its policy is limited:
- early boot bitmap/reservation allocator;
- buddy frame allocator after the full memory map is known;
- per-CPU frame caches to reduce global contention;
- NUMA-local zones and memory attributes when discovered;
- explicit DMA32/low-address and device-private zones where required;
- zero-on-allocation for user pages and zero-on-free for sensitive objects.

No general-purpose filesystem, graphics or application allocator lives in the
kernel. Userland memory manager and service-specific allocators own those
policies.

### 6.2 Kernel object allocation

Kernel object allocation uses bounded slab pools with per-CPU magazines.
Interrupt context may allocate only from pre-reserved non-blocking pools.
Pool exhaustion returns a defined error; no hidden unbounded retry or recursive
allocator path is allowed.

Object destruction is deferred until references and in-flight operations reach
zero. Destructors never execute while holding a global allocator or capability
table lock.

### 6.3 Virtual memory

- Separate kernel and user page-table roots/regions.
- W^X enforced on every mapping transition.
- User mappings cannot alias writable kernel mappings.
- Guard pages for kernel/user stacks and critical buffers.
- Per-address-space ASID/PCID when supported.
- TLB shootdown uses per-CPU request queues, generation/sequence numbers and
  bounded acknowledgements.
- Page faults distinguish absent, permission, copyin/copyout and device-mapping
  faults; faults in a user service never become kernel faults.
- Legacy v1 user virtual addresses and 4 KiB user page behavior are preserved
  by the compatibility VM layer.

### 6.4 Legacy sbrk

SYS_SBRK remains available. The compatibility layer updates the process break
and maps/unmaps pages using the minimal VM core. User malloc remains in the
unchanged userland library. New userland uses explicit VM objects and shared
memory capabilities rather than adding more sbrk-dependent APIs.

## 7. Capability and object runtime

### 7.1 Object model

Every kernel-visible resource is an object with:
- immutable type;
- generation and lifetime state;
- atomic reference count;
- rights mask;
- owner/security domain;
- backend operations;
- optional service endpoint and remote object ID;
- diagnostic ID and creation trace.

Kernel object pointers never cross the user ABI. Userland sees opaque handles.

### 7.2 Handle table

Legacy v1 accepts 32-bit integer handles. New internal capability references
are wider and generation tagged. The v1 adapter maps legacy integers to
internal references without exposing pointers.

The exact v1 token bit allocation is frozen only after the stale-handle threat
model and all existing fd assumptions are tested. Required invariants:
- standard descriptors 0, 1 and 2 keep their meaning;
- all returned handles are non-negative;
- slot index and generation are checked on every use;
- generation wrap cannot resurrect a stale handle (retire exhausted slots or
  use a stronger per-process epoch/tag);
- rights are stored in the table and can only be attenuated;
- duplicate handles share open-file-description state where POSIX requires it;
- handle close racing with read/write/wait cannot free an in-flight object.

New ABI v2 may expose 64-bit opaque capability tokens. A hardware pointer is
never used as a user-visible capability.

### 7.3 Lookup and synchronization

- Indexed slot lookup is O(1); no linked-list walk in the syscall hot path.
- Per-process handle pages are allocated in fixed-size chunks and bounded by a
  configured process limit.
- Read-only lookup uses generation validation and safe reference acquisition.
- Mutations use per-table/per-object locks or atomic state transitions.
- Global object registry lookup uses a bounded hash/probe policy and per-bucket
  locks; no single global spinlock protects all object operations.
- Per-CPU descriptor caches are cache-line aligned. Cache entries contain
  indexes/generations and references, not untrusted raw pointers.
- Internal pointer encoding may be added as defense-in-depth, but it is not a
  substitute for capability validation, lifetime safety or memory isolation.

### 7.4 Rights

The v1 rights map remains available: READ, WRITE, WAIT, DESTROY, DUPLICATE,
TRANSFER and CREATE. Coarse CAP_* checks remain in the v1 adapter.

ABI v2 adds narrower rights families (process create/kill, display mode,
window create/draw, filesystem mount/write, device MMIO, IRQ, DMA, power,
clock and reset). In particular, display-wide control must not rely on the
legacy CAP_WINDOW bit, which is also present in the current guest privilege
preset. The v1 compatibility policy and v2 hardened policy are separate.

## 8. IPC, semaphores and zero-copy

### 8.1 Inline IPC

Inline messages have a fixed maximum payload and a bounded capability-transfer
count. The legacy 88-byte message is supported by the v1 adapter. New IPC uses
versioned messages with explicit length and flags.

A port is a kernel queue object. A service endpoint is a capability to a port
plus an authenticated process identity. Send and receive are separate
operations; no user pointer is followed by another process.

### 8.2 Blocking and wakeup

A blocking operation:
1. validates handle and rights;
2. pins/copies any small user input;
3. records a wait sequence and request ID;
4. enqueues a bounded request;
5. atomically marks the caller BLOCKED;
6. schedules another thread;
7. validates the matching response and wakes the caller.

The caller never spins waiting for a service. Timeout, cancellation, service
death and process exit all resolve the request exactly once.

Semaphores and wait queues use explicit maximum waiter counts, FIFO or
priority-aware ordering, sequence numbers and bounded cancellation. No random
timer or busy-wait is used as a synchronization primitive.

### 8.3 Shared memory

Large buffers use a shared-memory object:
- immutable size and cache attributes after creation;
- explicit READ/WRITE/MAP/DMA rights;
- page pinning while any service/device mapping exists;
- offset/length descriptors with overflow checks;
- per-CPU descriptor rings aligned to cache lines;
- release/acquire ordering and sequence numbers;
- revocation and deferred free only after all mappings and DMA references end.

The kernel does not accept raw user pointers in service IPC. The compatibility
dispatcher performs bounded copyin/copyout for legacy APIs or translates a
legacy buffer into a temporary shared-memory object.

### 8.4 Zero-copy service RPC

The common path passes a capability and descriptor, not data bytes. VFS, block,
graphics, audio and network services can map the same pages. Device DMA requires
a separate IOMMU mapping and cache-coherency contract. A service crash triggers
mapping revocation, request cancellation and safe buffer reclamation.

## 9. Legacy syscall compatibility broker

The v1 syscall dispatcher is a stable adapter, not the home of legacy services.

- Kernel primitives (exit, yield, PID, VM break, time, process wait and
  capability operations) remain kernel calls.
- VFS/registry/graphics/font/input calls are translated into authenticated
  service requests.
- File handles are kernel proxy capabilities whose remote object ID belongs to
  vfsd. Duplicated descriptors share the same remote open-file-description.
- Window and registry handles are proxies to compositord/registryd objects.
- Legacy PID IPC and port/cap transfer remain kernel primitives during v1.
- The v1 error codes and POSIX errno conversion remain unchanged.

No synchronous service RPC is issued while holding a spinlock, object-table lock
or scheduler lock. A service response is accepted only from its endpoint
capability and for the outstanding request sequence.

If a service dies:
- all pending requests complete with a deterministic error;
- new requests fail with ENODEV/EIO until restart;
- proxy handles become stale/revoked;
- unrelated services and processes continue;
- the supervisor restarts the service with a fresh capability set.

## 10. Driver sandbox and DDI

The kernel grants device capabilities, not unrestricted bus access.

A native DDI describes:
- probe/attach/detach/suspend/resume;
- register regions and access widths;
- IRQ setup, mask, EOI and affinity;
- DMA constraints, map/unmap and IOMMU domain;
- device reset, clock, regulator and power-domain dependencies;
- asynchronous request/completion and error recovery.

devmgr matches normalized device graph nodes to signed/pinned driver metadata.
The driver service receives only the resources for its device. The IOMMU domain
prevents DMA outside the driver's assigned buffers. Without IOMMU, use bounce
buffers or restrict the device to a trusted driver profile.

Linux and BSD compatibility adapters are separate libraries. They translate
common driver idioms into the native DDI; they are not kernel emulators and do
not expose arbitrary kernel memory.

## 11. Optional hypervisor interface

The microkernel exports a provider-neutral virtualization interface:
- capability-gated VM create/destroy;
- guest physical memory slots backed by VM objects;
- vCPU create/run/stop and register-state operations;
- interrupt injection and timer virtualization;
- MMIO/PIO exit dispatch;
- second-stage translation and IOMMU binding;
- snapshot/debug/trace operations.

Vendor providers implement VMX, SVM and EL2 separately. Each reports the
features it actually supports (EPT/NPT, interrupt virtualization, nested
paging, virtual timer, guest debug, memory encryption). Missing features are
reported; no provider may silently emulate an unsupported security guarantee.

The hypervisor module is not required for native userland, service or driver
sandbox operation.

## 12. Deterministic operation budget model

Every core operation has a budget descriptor:
- maximum bytes/pages processed;
- maximum loop iterations;
- maximum retries and queue probes;
- maximum capabilities transferred;
- maximum lock acquisitions and critical-section work;
- maximum CPUs participating in an IPI/TLB operation;
- absolute deadline and cancellation action;
- counters for observed cycles, time and failures.

Examples of algorithmic bounds:
- capability lookup: one indexed table lookup plus one generation/rights check;
- IPC enqueue/dequeue: one bounded ring operation and a fixed maximum cap loop;
- scheduler queue update: constant number of intrusive queue operations;
- page-table map: no more than the configured paging levels plus bounded
  allocator/page-fault fallback;
- TLB shootdown: at most one request per online target CPU, sequence-numbered
  acknowledgement and timeout/offline recovery;
- driver RPC: fixed request descriptor and deadline; hardware completion is
  asynchronous and not represented as a CPU-cycle guarantee.

Operations that cannot complete inside a bounded fast path return a pending
request, block on a wait queue or fail with a defined error. No hidden retry
loop is allowed.

Use PMU core-cycle events when available. Use invariant TSC or the Arm Generic
Timer for elapsed time only; do not label their ticks as actual core cycles.
Record the exact CPU, core class, counter source, frequency, build ID and power
state with every WCET profile.

A measured maximum is valid only for its documented CPU/platform/build profile.
Firmware SMIs, cache/DRAM contention, device latency and thermal events prevent
a universal WCET claim for arbitrary hardware.

## 13. NASA/JPL Power-of-Ten engineering rules

The core and all trusted HAL code follow these rules:
1. Restrict control flow: no recursion, setjmp/longjmp or unstructured jumps
   in trusted core/HAL code.
2. Every loop has a statically visible, mechanically checkable upper bound.
3. No dynamic allocation after subsystem initialization in the trusted core;
   use preallocated bounded pools and fail explicitly on exhaustion.
4. Limit functions to 60 source lines; split longer functions and document
   reviewed exceptions.
5. Use assertions for invariants: target at least two side-effect-free
   assertions in functions longer than 20 lines, and at least two per function
   on average.
6. Declare data in the smallest possible scope.
7. Check every non-void return value and validate every function parameter.
8. Limit preprocessor use to file inclusion and simple macros/configuration.
9. Limit pointer use and dereference depth; validate user pointers and avoid
   unsafe pointer arithmetic, casts and aliasing.
10. Compile with all warnings enabled and use source analyzers; no warning
    suppression in trusted core/HAL builds.

Additional project rules:
- no allocation in interrupt context except reserved fixed pools;
- no lock held across IPC, filesystem, driver or firmware calls;
- no raw user pointer retained beyond a syscall;
- no unbounded string operation on external data;
- all external blobs (ELF, DTB, ACPI, firmware, font, image) are length checked;
- all error paths release references, pins, mappings and locks;
- every new syscall has ABI tests, a documented budget and a failure policy.

Reference: Gerard J. Holzmann, NASA/JPL, “The Power of Ten: Rules for
Developing Safety-Critical Code” (2006):
https://spinroot.com/gerard/pdf/Power_of_Ten.pdf

## 14. Diagnostics and verification

### Diagnostics
- per-CPU fixed-size trace rings with monotonic sequence numbers;
- structured event IDs, timestamps and CPU IDs;
- build ID, compiler/toolchain manifest and source revision in boot log;
- panic console and crash record without dynamic allocation;
- symbolized stack traces for kernel and service processes;
- service restart history, resource ownership and capability audit logs;
- optional hardware PMU counters and cache/TLB/interrupt metrics;
- fault injection for memory exhaustion, service death, stale handles,
  interrupt storms, DMA timeout and CPU hotplug.

### Test layers
1. Host unit tests for pure core algorithms and ABI structures.
2. Cross-compiled static layout tests on x86-64 and AArch64.
3. Fuzz tests for ELF, DTB, ACPI table, namespace and IPC parsers.
4. Model/property tests for capability lifetime, wait queues and scheduler
   transitions.
5. QEMU dual-architecture boot and unchanged-userland tests.
6. SMP stress with 1, 2, 4 and available maximum vCPU counts.
7. Service crash/restart and blocked-syscall cancellation tests.
8. IOMMU/DMA isolation and no-IOMMU bounce-buffer tests.
9. Per-platform WCET profiling and static loop-bound review.
10. Real-hardware matrix with ACPI, FDT, PCIe, NVMe, VirtIO, APIC/GIC and
    power-management coverage.

## 15. Non-goals for the first bootable NG kernel

- Full POSIX or Linux kernel ABI.
- Running arbitrary Linux kernel modules.
- Replacing every device driver before the service framework exists.
- Mandatory VMX/SVM/EL2 support.
- Mandatory AVX-512, SVE or SME.
- Universal voltage control.
- Claiming cycle-perfect WCET on unknown commodity hardware.
- Deleting the legacy kernel before all v1 acceptance gates pass.

The first bootable NG milestone is deliberately smaller: both architectures
boot, SMP works, address spaces and W^X work, capability IPC works, the v1
syscall broker can start the unchanged init and shell, and the legacy source
remains available for comparison.
