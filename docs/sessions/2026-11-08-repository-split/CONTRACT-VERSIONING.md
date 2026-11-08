# NexsOS1 — versioning and conformance contracts

Date: 2026-11-08
Status: normative design for the repository/build separation; executable manifests and gates are implemented in subsequent build increments.

## 1. Scope and ownership

The system has three separately versioned contracts:

1. **HAL specification**: architecture/platform-facing operations and guarantees required by the kernel.
2. **Kernel specification / kernel-user ABI**: boot handoff, syscall entry and return, object/capability semantics, memory and process guarantees, and user-visible behavior.
3. **NX-SDK contract**: public native API, C/POSIX compatibility, runtime/startup objects, library interfaces, and declared compatibility with exact HAL/kernel specification versions.

NX-SDK is logically part of the NX-VIOLA userland platform and its public development environment. It remains separately versioned so NX-CORE can consume only its explicitly kernel-safe contract subset without depending on the NX-VIOLA repository or userland runtime. In the superproject the three checkouts are peers; this is a dependency rule, not a requirement that one Git repository be nested inside another.

The kernel and HAL publish requirements. The SDK does not redefine kernel behavior: it implements the user-facing contract against a declared kernel specification version. The HAL implementation does not self-certify: it declares a conformance profile and must pass the kernel's HAL contract suite.

## 2. Version identifiers

Use independent semantic versions for specifications, not the product/release version:

- `HAL-SPEC major.minor.patch`
- `KERNEL-SPEC major.minor.patch` (includes the user-visible kernel ABI)
- `NX-SDK major.minor.patch`

Rules:

- **MAJOR**: incompatible normative change, removed/renamed operation, changed calling convention, layout, error semantics, ownership/lifetime, ordering, or required guarantee.
- **MINOR**: additive operation/capability/field/feature that preserves all prior normative behavior. Optional fields must have defined defaults and unknown-field handling.
- **PATCH**: clarification, typo or test correction that does not alter observable behavior or conformance interpretation. If behavior changes, it is not a patch.
- Spec version and implementation version are distinct. A kernel binary may have its own release version while implementing one or more exact spec versions.
- Never use a floating label such as `latest` or an unbounded compatibility claim in a release manifest.

Each normative requirement receives a stable identifier (for example `HAL-CPU-001`, `KABI-SYSCALL-017`, `SDK-POSIX-042`). IDs are never reused. A moved or rewritten requirement retains its ID when its semantics are unchanged; a semantic break creates a new ID and an explicit supersedes relation.

## 3. Compatibility declaration

Every implementation manifest must state:

- exact specification ID and version;
- exact implementation revision/commit and build profile;
- supported architecture, platform/board, firmware/boot mode, and feature configuration;
- required and optional capabilities;
- compatibility relation (exact, backward-compatible range, or explicit adapter);
- conformance result for every applicable requirement;
- test suite revision, test IDs, logs, and artifact hashes.

Compatibility is a tested relation between two concrete versions, not a guess based on matching major numbers. A consumer may declare a range only if the complete compatibility matrix for that range has passed. Otherwise it pins an exact version.

The release resolver must reject:
- a missing specification;
- a consumer outside the provider's declared compatibility range;
- an unsupported required feature;
- any applicable requirement without a passing result;
- a test report that refers to a different source revision, spec hash, architecture, or configuration.

## 4. Requirement support classification

Every specification requirement and feature is classified independently from its test result.

| Classification | Meaning | Required behavior |
|---|---|---|
| `FULL` | Fully implemented and conformant on the declared target/profile | All normative behavior and applicable tests pass |
| `SOFTWARE_ONLY` | Fully available through software implementation; no hardware acceleration/device dependency is claimed | Software path passes the same functional contract; performance is reported separately |
| `CONDITIONAL` | Supported only when named conditions are satisfied | Conditions are machine-readable; unsupported conditions fail capability negotiation |
| `REQUIRES` | Depends on a named prerequisite (feature, firmware, service, device, capability, or minimum spec) | Prerequisite is explicit and verified before use |
| `PARTIAL` | Only a defined subset is implemented | Exact supported subset is listed; never advertised as full support |
| `EXPERIMENTAL` | Implementation exists but is not release-qualified | Excluded from stable compatibility promises |
| `DEPRECATED` | Supported for a stated compatibility window but must not be used by new consumers | Removal version/date and replacement are declared |
| `UNSUPPORTED` | Not implemented or not available on this profile | Capability is absent; calls fail deterministically |

Do not use `FULL` to mean merely “the code exists”. Do not use `SOFTWARE_ONLY` as a euphemism for partial support: it means the entire functional specification is met by software.

Each requirement also has a separate verification state:

- `PASS`: all required tests passed on the exact profile;
- `FAIL`: at least one required test failed;
- `NOT_RUN`: no valid evidence exists;
- `BLOCKED`: a prerequisite prevented execution;
- `NOT_APPLICABLE`: the requirement is excluded by a formally declared profile condition.

A requirement cannot be release-qualified as `FULL`, `SOFTWARE_ONLY`, or `CONDITIONAL` while its verification state is `NOT_RUN`, `FAIL`, or `BLOCKED`. `NOT_APPLICABLE` requires a machine-readable rationale and must not conceal a missing implementation.

## 5. HAL ↔ Kernel contract

The HAL spec is the provider contract for each ISA/platform. It covers at least:

- CPU feature discovery and mandatory execution-state assumptions;
- interrupt/trap entry, exception frame, return and register preservation;
- timer source, frequency, monotonicity, interrupt acknowledgement and deadlines;
- physical/virtual address translation, page-table format, TLB invalidation and barriers;
- cache maintenance and memory ordering;
- SMP startup, inter-processor interrupts and per-CPU state;
- MMIO mapping/access ordering, IRQ routing, DMA/IOMMU constraints;
- firmware/boot handoff, memory map, device-tree/ACPI inputs;
- reset, panic, fault and unsupported-hardware behavior.

For each HAL operation the kernel spec declares: required/optional, preconditions, postconditions, clobbers, ordering, bounds/timeouts, error behavior, ownership, and whether software fallback is permitted.

The kernel manifest pins a HAL-SPEC version/range and required feature set. Each existing `kernel/arch/<isa>/hal.c` implementation declares the HAL-SPEC version it implements. Do not add a parallel HAL. Existing implementations are adapted in place and tested against the contract.

HAL conformance tests must include host-independent unit tests where possible, QEMU/platform integration tests, architecture-specific assembly probes, fault/negative tests, and hardware tests when the feature classification requires physical hardware. QEMU success does not certify physical-device support.

## 6. Kernel ↔ Userland / SDK contract

The kernel specification is the authority for the user-visible ABI. It includes:

- boot protocol and versioned BootInfo;
- syscall number, register, stack, clobber and return/error conventions;
- object types, handle generations, rights, transfer and lifetime semantics;
- message layouts, alignment, size limits and endianness;
- process, memory, filesystem, IPC, timer, input and graphics behavior;
- capability negotiation and unsupported-operation behavior;
- service bootstrap ordering and failure/restart semantics.

The SDK manifest declares which exact KERNEL-SPEC versions it supports. The kernel exports its spec identity and supported feature/capability set through a minimal bootstrap discovery interface. Discovery itself is part of the versioned ABI and must remain backward-readable.

The SDK must not assume that a service exists merely because a function is declared. Service clients negotiate/discover endpoints and report unavailable services according to the API contract. A service-backed extension is distinct from ISO C/POSIX functionality and must be independently linkable/testable.

For legacy ABI v1, existing syscall numbers, calling conventions, structure layouts and observable error behavior remain frozen unless a reviewed ABI-major transition is explicitly approved. A v2 implementation may coexist with v1 compatibility; it must not silently reinterpret v1.

## 7. Test completeness and traceability

The specification is executable documentation. Every normative requirement maps to one or more test cases or to a justified static/structural verification method.

Required test layers:

1. schema and manifest validation;
2. requirement-ID uniqueness and traceability completeness;
3. header self-containment and generated ABI layout checks for amd64/aarch64;
4. syscall/HAL conformance unit tests, including boundary and negative cases;
5. cross-architecture compile/link and symbol/relocation checks;
6. emulator/platform integration tests;
7. service discovery, startup ordering, death, restart and stale-capability tests;
8. userland API tests, including POSIX error conversion and service-unavailable behavior;
9. compatibility matrix tests for every declared provider-consumer version pair;
10. end-to-end boot to shell and interactive nxadb acceptance.

A test report is valid only when it records the tested commit, spec content hash, test-suite commit, toolchain identity, host OS/CPU, target ISA, firmware/boot profile, QEMU/device model, and command line. Preserve complete serial/UEFI/QEMU logs and test transcripts. Never truncate boot diagnostics to make a test appear successful.

Release gates require zero failed applicable tests, zero unclassified normative requirements, zero undeclared compatibility claims, and no `NOT_RUN` requirement in a feature advertised as supported.

## 8. Compatibility across versions

Maintain a generated matrix with rows = provider specification versions and columns = consumer implementation versions, per architecture/profile. Each cell is one of:

- `PASS`: complete conformance suite passed;
- `FAIL`: a compatibility test failed;
- `UNTESTED`: no evidence; cannot be used for release;
- `ADAPTED`: a named, versioned adapter passed the complete suite;
- `N/A`: formally inapplicable for that profile.

Compatibility is directional. “SDK 2 works with Kernel 1” does not imply “SDK 1 works with Kernel 2”. Test both directions whenever both are claimed.

A new minor spec version must rerun the previous supported compatibility set. A major version requires explicit migration tests and a declared compatibility bridge or a deliberate break. A patch release reruns the affected requirement tests plus the full release regression suite.

## 9. Build and repository sequencing

The build system must consume the manifests, not hardcode independent version assumptions in each module. The root superproject resolves pinned module commits and spec versions; NX-CORE builds against HAL/kernel contract inputs; NX-SDK builds its public API/runtime and emits the compatibility report; NX-VIOLA builds against the SDK manifest. Rootfs, GRUB, mkdisk, release and nxadb remain integration-superproject responsibilities.

Implementation order:
1. establish independent, reproducible kernel and userland build entrypoints without moving source files;
2. isolate host tools and integration/image/release orchestration;
3. add machine-readable spec/implementation manifests and conformance runners;
4. extract SDK-owned libraries and portability components with symbol ownership and no duplicate definitions;
5. create/pin NX-CORE, NX-SDK and NX-VIOLA repositories, preserving upstream submodules;
6. run the complete host × target × boot-to-shell matrix.

## 10. lib.c and service-extension boundary

Do not split `user/sys/lib/lib.c` by arbitrary line ranges or move all of it into one new library. First classify every exported symbol and its callers into:

- ISO C runtime;
- POSIX compatibility implementation;
- NexsOS native low-level ABI veneer (`OS1low_*`);
- NexsOS native high-level API (`OS1_*`);
- service client/extension (requires a boot-started service or a discovered endpoint);
- graphics/input/font/video portability implementation;
- compatibility-only symbol with a named removal condition.

The current file mixes all these layers. Existing evidence identifies `notify_send`/`OS1_notify_*` as a client of the boot-started notification service; `execsvc_client.c` is already separate; window/video and graphics paths currently include direct kernel syscalls and must not be mislabeled as userland-service clients without checking their actual call graph. Registry/environment and VFS calls also need to be classified against current kernel implementations and future serverization docs.

For each symbol record prototype, definition, callers, dependencies, exported archive/object, service prerequisite, failure semantics, and tests. Split one coherent layer at a time, preserving one canonical implementation and testing all affected binaries on both ISAs after each increment. The existing R0 symbol census is historical input, not an authoritative current map; regenerate it from the current 4,985-line file and verify against nm/link maps.


## 11. Existing version markers and missing identities (source audit)

Existing local version markers:
- kernel/include/kernel/boot_info.h: BOOT_INFO_VERSION 1.
- include/abi/sysstats.h: OS1_SYSSTATS_VERSION 1.
- include/api/execsvc.h: EXECSVC_VERSION 2, checked by the nxexec server.
- user/sys/lib/portability/os1_video_platform.c: private OS1_VIDEO_ABI_VERSION 1; returned in the public info structure but not yet a public header-level contract.
- include/abi/nx_abi.h: compile-time offset/size assertion macros, but no global kernel ABI identity.

Gaps confirmed in the current tree:
- No global HAL-SPEC version identity is declared by the HAL headers/implementations.
- No global kernel-user ABI version identity is declared in the ABI headers.
- The notification IPC client/server protocol has no explicit protocol version.
- No executable compatibility-matrix runner currently binds spec hashes, test IDs, architecture, toolchain and source revision.
- The existing docs/architecture/ABI-COMPATIBILITY-MATRIX.md is a legacy ABI v1 inventory whose source baseline is an older commit. It is the historical compatibility source to reconcile against the current tree, not proof that the current kernel implements every listed behavior.
- docs/direction/DIR-06-hal-conformance.md is the HAL design direction and audit baseline; its listed direct-ISA and HAL layering findings must be rechecked against current source before assigning a current support state.

Do not simply add version macros and call the system versioned. The next implementation must add machine-readable requirement records, provider/consumer manifests, static layout checks, HAL behavior tests, runtime ABI/service tests and a generated directional matrix. Every version claim remains unqualified until that complete test evidence exists.
