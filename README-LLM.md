# NexsOS1 LLM Development Governance

## Authority and provenance

This file separates **user-authored directives** from any later **assistant-authored proposals, interpretations, plans, or implementation decisions**.

- Anything under **User Directive** is a semantic English translation of instructions given directly by the project owner and must not be silently weakened, reinterpreted, or replaced.
- Any assistant-added rule, plan, architectural proposal, or implementation choice must be placed in a clearly separate section and must cite the evidence in project documentation/code that motivated it.
- When a later instruction conflicts with an earlier one, the newer explicit user instruction wins. The conflict must be documented rather than hidden.
- No implementation claim may be treated as verified until the required build/runtime/test gate has actually passed.

---

# Session 1

## User Directive — 2026-11-14

Connect to the NexsOS1 project and verify all unstaged modifications in every repository.

The previous agent was performing transition work, but the chat ended because it became too long, the documentation was not updated, and the NexsOS1 graphics system is now broken.

### Immediate observed regressions

On AMD64:
- The GRUB boot manager is displayed incorrectly.
- The display then appears black.
- When switching to the GPU output, only a full-screen shell is shown.
- The previous agent appears to have confused the old kernel graphics implementation with the new userland implementation.
- The old kernel path was supposed to be used only as a transitional path toward surface management moved into the resource layer.
- Windows are now effectively full-screen, without a proper distinction between hardware-display resizing and virtual-window resizing, and without a per-window buffer model.
- `nxcompositor` has effectively been ignored.
- Mouse input and rendering are blocked.

On ARM64:
- The system no longer compiles.

### Required validation workflow

Tests must be run with a clean tree/build state using:
- `make clean`
- `make run` for the default architecture
- `make run ARCH=amd64` for AMD64
- the corresponding ARM64 target as defined by the project build system

When an architecture successfully boots to the shell:
- switch to interactive testing through `nxadb`;
- execute the test suite;
- execute at least two meaningful commands per architecture through `nxadb`.

The UART shell is **debug-only**. NexsOS1 is a graphical operating system and the graphical path is the acceptance path.

Compilation logs must be inspected, not merely checked for a final exit code. Warnings, errors, suspicious fallbacks, disabled subsystems, and architectural violations found during the build must be catalogued even if they are not directly related to the current task.

### Safety before modifying anything

Before invasive work:
1. inspect all repositories;
2. verify staged changes first;
3. preserve all unstaged work in a ZIP archive;
4. catalogue every file included in the archive and classify the purpose/risk of its changes;
5. only then clean the working state needed for reproducible testing;
6. do not discard work without first preserving and documenting it.

There appear to be two repositories whose most recent commit is around 23:17 and two whose most recent commit is around 23:21. The two repositories with the additional commit must be identified and the extra commit must be reviewed carefully.

Verified staged work may be pushed and synchronized, but only after the required build/runtime/test gate passes.

### Repository discipline

Work in small, bounded, verifiable blocks:
1. plan;
2. task;
3. verify modifications;
4. build/test;
5. stage only when the block is complete and verified.

Do not leave partially implemented code behind between blocks.

When more room is needed for invasive work:
- first save the previous verified work;
- ensure the verified block is staged;
- only then start the invasive block.

Never change branch unless explicitly authorized.

Use the project rules and protocols already established in documentation and source code. Do not invent replacements for existing infrastructure before searching for and understanding the existing interfaces, protocols, libraries, services, subsystems, and tests.

When context is missing, retrieve it from documentation, source code, Git history, or references instead of guessing.

### Documentation discipline

From now on:
- every task, objective, discovered issue, architectural decision, implementation gap, verification result, and deferred issue must be catalogued;
- this methodology is mandatory for this and future chats and must not be bypassed;
- documentation must be updated as work progresses, not reconstructed after the fact.

Group the existing documentation under a clearly identified legacy area and begin a new Session 1 documentation track. This restructuring must itself be planned and verified before destructive moves occur.

This `README-LLM.md` file is the persistent governance entry point. This user directive must remain distinguishable from future assistant-authored choices.

### Architecture corrections to verify and preserve where valid

Some transition changes were directionally correct and must not be discarded blindly. They must be reconstructed correctly where needed.

Examples that were considered valid in principle:
- remove hard-coded limits and hard-coded device/display assumptions;
- do not hard-code registry sets inside the kernel;
- registry management arrays/catalogues belong in the SDK/system library layer, while the selected system/project (NexsOS1, demo, installer, or another consumer) chooses the registry sets to use;
- remove VFS entries from the kernel and SDK where they incorrectly encode system policy there;
- configuration belongs to the project/system integration layer rather than being hard-coded into low-level libraries;
- keyboard support belongs in the SDK, with parameters selected through configuration files;
- the kernel-side system-image changes may be correct, but must be verified against the documented architecture and real code before being accepted;
- hardware resolution discovery must not be replaced by fixed resolution assumptions;
- user-selected/rendering resolution is a separate state from the physical device mode and must remain configurable.

### Kernel, HAL, platform, and driver model

The previous transition wrongly included SDK material in the kernel. This is unacceptable and must be identified and removed through a correct reconstruction rather than by deleting functionality.

Also audit:
- external libraries accidentally or improperly pulled into the kernel;
- Power12 compliance;
- missing, deleted, weakened, or falsified tests;
- tests that were not cleaned up when an implementation moved.

When an implementation is moved, its tests must move or be reconstructed cleanly around the new ownership boundary. No stale tests may remain that accidentally validate the wrong layer.

The HAL must not know UART as a product/system policy. UART is a debug transport and must be abstracted at the correct layer.

The previous HAL/platform work assumed that QEMU or currently available hardware was the only target. This is the wrong model. A proper driver model must be defined first, with platform-specific drivers integrated through that model.

Platform drivers currently living in the HAL should be evaluated for movement into the kernel-side driver model, while the longer-term architecture moves ordinary drivers into userland.

### Graphics/compositor transition — first major reconstruction phase

The compositor transition should be the first major functional reconstruction.

Target direction:
- the entire `kernel/graphics` ownership must be eliminated from the kernel once all useful logic has been correctly relocated;
- useful code must not simply be deleted;
- retain only the kernel mechanisms that truly belong there, such as controlled framebuffer/surface/resource capability mechanisms and hardware-facing primitives;
- the compositor itself must become a userland service;
- theme management must be removed from the kernel/HAL;
- window effects, borders, shadows, buttons, themes, window layout, virtual resolution, and display policy belong in the interchangeable `nxcompositor` service;
- per-application/window buffers belong in userland applications and are connected to `nxcompositor` through SDK protocols;
- surface/protocol ownership must be explicit and capability-safe;
- hardware display resizing/mode changes must be distinct from virtual-window resizing and rendering resolution;
- do not confuse framebuffer ownership with compositor/window ownership.

ARM64 and AMD64 previously appeared scaled to a lower resolution because of a hard-coded scaling rule. Remove the hard-coded scaling, but do not remove legitimate resolution-setting functionality. Device-supported modes and the selected rendering/virtual resolution are different concepts.

### SDK restructuring direction

The SDK currently contains the system libc, scattered expansions, portability layers, and `extension/` including UserKit headers.

The long-term target is to separate the SDK into coherent layers rather than allowing unrelated responsibilities to accumulate in one library.

#### 1. Userland HAL / sandbox primitive layer

All kernel-exported primitives and system-call/object primitives available to userland, empirically verified, form a userland hardware/primitive abstraction boundary.

Above that, build a safe sandbox interface consumed by the system libc.

This sandbox layer supplies safe primitives to libc. Applications use the library to request verified access through the userland HAL/sandbox.

Capability authorization itself remains a kernel responsibility. Approximate global roles such as user/root/machine must not remain as informal policy shortcuts in application libraries. Authority should instead be represented by the hosts/controllers of per-process sandbox servers and capability/object relationships.

Idle/control tasks should evolve into masked controllers, and operations should become object packets representing operations on the registry.

Two distinct sandbox models are required:
- application/process sandbox;
- driver sandbox.

#### 2. System libc

The system libc is a consumer of the sandbox/idle-task primitives.

It must not know about extensions or portability layers.

A previous plan treated POSIX mainly as a portability library. The updated direction is to integrate the mature POSIX-facing behavior at the libc/system layer where it naturally consumes the userland HAL/sandbox functionality, while keeping the kernel free of POSIX policy.

#### 3. Portability

The portability area contains libraries/adapters for external software.

Extend the plan to include future binary/software compatibility layers for:
- Linux;
- Windows;
- Android;
- additional systems where justified.

Separate external source trees from NexsOS1 compatibility/runtime implementation.

#### 4. Extensions

`extension/` contains library expansions. Existing logic is currently scattered and incoherent.

Extensions may build on libc or on DriverKit, depending on their purpose.

#### 5. UserKit

UserKit currently lives under `lib/extension` and should be evaluated for movement to a first-class `lib/UserKit`-style location.

UserKit exposes SDK-side APIs made available by system services.

Service-local implementation remains inside each service; only the public SDK contract belongs in UserKit.

UserKit may build on libc and/or DriverKit as justified.

#### 6. DriverKit

DriverKit does not yet exist as a proper subsystem.

The architecture must move ordinary drivers out of the kernel into userland and move platform-specific device drivers out of the HAL into the proper kernel/user driver architecture.

Create a driver sandbox distinct from the application sandbox.

Study the Linux driver model and, where technically and legally appropriate, design for source-level compatibility or reusable abstractions rather than blindly copying Linux structure.

Each driver should become a separate userland project/module.

Driver-local logic belongs in user-side DriverKit/driver projects; globally consumable DriverKit headers/contracts belong in the SDK/system DriverKit area.

Drivers may use the system libc/sandbox. For performance-critical operations, provide a capability-gated driver-only library/path that avoids unnecessary userland IPC.

### IPC, MMIO, zero-copy, registry-object execution model

IPC and MMIO must be restructured and standardized.

Goals:
- use IPC only where necessary;
- do not make the system architecture depend on chatty IPC;
- avoid unnecessary performance loss;
- study correct buffer ownership and lifetime;
- study masked pointer arrays/direct resource access patterns where safe;
- prefer efficient algorithms and capability-verified direct access where appropriate;
- reduce subsystem-specific one-off implementations.

The long-term direction is a large catalogue of reusable algorithms exposed through a large registry/object array with layered access control.

Operations should be represented as pull/request-style object messages over variable-size packets, with zero-copy operation where possible and with synchronization designed to avoid wasting clock cycles.

Every such mechanism must be justified by measured behavior, ownership/lifetime rules, concurrency analysis, and security boundaries rather than by slogans.

### VFS, storage, images, installer, and multi-image system

The VFS must move to userland together with the relevant drivers.

The target VFS should be an extended Android/Linux-style model:
- Linux VFS concepts as a baseline;
- Nexs extensions inspired by Linux/iOS/Android sandboxing and multi-image system layout;
- support multiple system images and sandboxed images;
- make compositor/system image handling compatible with that model where appropriate.

Preboot loaders must load the appropriate images from devices.

The project requires at least:
- installer mode;
- normal user mode.

The current distinction between `demo1` and the complete NexsOS1 boot should be studied as a basis for installer/demo vs full-system behavior rather than duplicated blindly.

The ext4 implementation requires revision. The NXOLMO repository is an approved reference/source candidate for ext4 and other reusable components already identified by project documentation.

Storage/VFS work should exploit available CPU cores/threads where useful and must be verified with real concurrency tests.

### Reference systems

Use Linux, seL4, and BSD as reference implementations.

Add them under `references/` as optional Git submodules, not fetched by default by the normal setup script.

These references are for study and comparison. They must not silently become required build dependencies.

When there is doubt:
- inspect project documentation;
- inspect current source;
- inspect Git history;
- inspect approved references;
- research externally when needed;
- do not assume.

### Power12 and testing

Restructure the kernel according to documented architecture and the Power12 gate.

Power12 is mandatory for kernel/HAL work.

Do not falsify tests, weaken tests to make them pass, or introduce empty scaffolding that merely satisfies a test target.

Every moved or rewritten kernel/HAL implementation must have tests that verify the actual implementation boundary.

Missing implementations must first be catalogued, then compared against:
- existing similar interfaces;
- existing protocols;
- existing libraries;
- existing services;
- existing subsystems;
- approved reference implementations.

Only after that analysis should code be implemented.

### Development target

Current project version: **0.0.6.4**.

The objective is to establish a reliable method for reaching **0.1.0.0** without repeatedly destabilizing the tree.

The system target is a general-purpose operating system without architectural shortcuts.

Every important design choice must be studied in detail, compared with the best relevant reference designs, measured where performance matters, documented, implemented in bounded blocks, tested, and staged only after verification.

When the current technique provides insufficient visibility or confidence, improve the test/debug/instrumentation methodology before continuing.

---

## Assistant-authored material

No assistant-authored architectural decision has been added yet for Session 1.

Future assistant-authored content must be placed below this line, clearly labelled, and must not be presented as part of the user directive.
