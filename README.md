# NexsOS1 (OS1)

### Multi-architecture open-source hybrid-kernel operating system

[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](LICENSE.md)
[![Arch](https://img.shields.io/badge/arch-aarch64%20%7C%20amd64-green.svg)](#)
[![Platform](https://img.shields.io/badge/platform-QEMU-orange.svg)](https://www.qemu.org/)
[![Language](https://img.shields.io/badge/language-C99-yellow.svg)](https://en.wikipedia.org/wiki/C99)

NexsOS1 (codename **OS1**) is an educational and research hybrid-kernel operating system built from scratch. It boots on both ARM64 (AArch64) and x86-64 (AMD64) under QEMU, brings up Symmetric Multiprocessing (SMP), drives native **VirtIO** devices (GPU, input, block), mounts an **Ext4** root filesystem, composites overlapping windows in a custom graphical user interface, and executes user-space ELF binaries with an interactive **TTY shell**.

> **Honesty note.** The test matrix below records earlier verification and does not qualify the current local changes. The current incremental review and open failures are recorded in [`docs/legacy/review/INCREMENTAL-REVIEW-2026-11-12.md`](docs/legacy/review/INCREMENTAL-REVIEW-2026-11-12.md). For the complete, evidence-based picture — including bugs, gaps, and severity — see [`docs/legacy/review/REVIEW.md`](docs/legacy/review/REVIEW.md). For where the project is *going* (a seL4-style, Plan 9-inspired Microkernel), see [`docs/legacy/PROJECT_CHARTER.md`](docs/legacy/PROJECT_CHARTER.md). This project is licensed under **GPL v2** (see [`LICENSE.md`](LICENSE.md)).

---

## 📋 Table of Contents
1. [Key Features](#-key-features)
2. [Verified Runtime Status](#-verified-runtime-status)
3. [Screenshots & Themes](#-screenshots--themes)
4. [Prerequisites & Toolchain Setup](#-prerequisites--toolchain-setup)
5. [Compilation & Execution](#-compilation--execution)
6. [Project Layout](#-project-layout)
7. [The ASTRA Roadmap](#-the-astra-roadmap)
8. [Democratic Development & Contributing](#-democratic-development--contributing)
9. [Acknowledgments](#-acknowledgments)
10. [License](#-license)

---

## 🚀 Key Features

*   **Architectural Abstraction Layer (HAL):** The HAL provides architecture contracts and providers shared by the common kernel. Remaining architecture leakage and provider ownership are tracked in the architecture and review documents.
*   **Secure Memory Paging:** Physical (zone-based PMM bitmap allocator) and Virtual Memory Managers handle paging. On AMD64, the system enforces the strict **$W \oplus X$ (Write or Execute)** security protocol (executable text is RX, read-only data is RO+NX, all other regions are RW+NX) alongside Higher-Half kernel mappings (`0xFFFF000000000000` on AArch64 via TTBR1, `0xFFFF800000000000` on AMD64), with cross-CPU TLB shootdown.
*   **Preemptive SMP & Multiprocessing:** Preemptive O(1) SMP scheduler with per-CPU priority run-queues and work-stealing. Cores are initialized and managed by user-space idle tasks once primary boot completes. Multiple processes are cleanly distributed across cores, with full support for `child` creation, `kill` termination, and an `exec` utility managing terminal, graphical, and integrated terminal modes.
*   **Capability-Based Security:** Syscalls and system resources are checked and restricted through a capabilities security model mapped to abstract user levels (`Machine`, `Root`, `User`, `Guest`). The kernel starts `nxmanageDD` at `Machine` level; `nxinit` runs as its `Root` policy child. Kernel idle tasks also use machine authority.
*   **Graphics Stack & Window Compositor:** A supervised ring-3 `nxcompositor` owns frame pacing, while the legacy renderer, scene/window policy and VirtIO display drivers still run in the kernel through the v1 compatibility ABI. Userland includes initial ARGB8888 raster primitives; capability-backed shared surfaces and full compositor extraction remain in progress. Supports custom themes and styles; interface components are compiled as independent ELF binaries.
*   **Isolated Userland & Release System:** The userland environment resides in a dedicated disk image (`disk.img`), allowing independent loading of the kernel and filesystem in QEMU for rapid testing. At boot, the kernel loads `disk.img` into a temporary RAM disk, and launches the `init` supervisor. `nxinit` reads the boot manifest and registers services with `nxmanageDD`, which owns supervision and the declared restart policy. The release tool compiles a hybrid, bootable AMD64 ISO via GRUB.
*   **Fault & Trace Isolation:** Recoverable fault handling with total vector coverage on both architectures. A userland application crash is isolated and never kills the kernel, leaving the shell and compositor running. Provides symbolized in-kernel backtraces (`.ksyms`) on dedicated fault stacks.
*   **VFS & Ext4 File System:** Virtual File System (with mount tables and `fs_ops` providers) supporting Ext4 (extent-tree reads, legacy reads, INCOMPAT enforcement, and extended write paths) and GPT partition mapping with MBR fallback, and a buffer cache.
*   **Coherent Syscall ABI:** Single unified syscall numbering (`system/include/contract/syscall_nums.h`) compiled into both the kernel dispatcher and the userland stubs, returning negative `errno` values.

---

## 📊 Verified Runtime Status

### Capability Matrix
Successfully tested and verified by building and running:

| Capability | AArch64 (`make run`) | AMD64 (`make run`) |
|---|:---:|:---:|
| **Clean Build** (`-Werror -Wall -Wextra -Wpedantic -Wshadow`) | ✅ | ✅ |
| **Boots to TTY Shell** (In a composited window) | ✅ | ✅ |
| **Higher-Half Kernel** (PA/VA contract, direct map, $W \oplus X$) | ✅ `0xFFFF0000...` | ✅ `0xFFFF8000...` |
| **Dynamic RAM Detection** (Full boot-protocol memory map) | ✅ (DTB) | ✅* (PVH/MB1/MB2) |
| **Symmetric Multiprocessing** (SMP Core Bring-up) | ✅ (4/4 Online) | ✅ |
| **VirtIO Drivers** (GPU, Input/Keyboard/Mouse, Block) | ✅ | ✅ |
| **Ext4 Support** (Extent-tree reads, GPT/MBR fallback) | ✅ | ✅ |
| **Userland Environment** (ELF Loader, IPC, Registry, Fonts) | ✅ | ✅ |
| **Fault Isolation** (User crash isolated, Symbolized Backtrace) | ✅ | ✅ |
| **Coherent Syscall ABI** (Negative `errno`, Capability Layer) | ✅ | ✅ |

> [!NOTE]  
> \* The AMD64 boot pipeline parses the physical PVH memory map, resolving previous hardcoded fallback constraints. However, total RAM estimation currently treats the 3–4 GB PCI hole as RAM. AArch64 remains the reference, fully standard platform.

### Supported Environments
| Platform / Host OS | Status | Notes |
| :--- | :---: | :--- |
| **QEMU AArch64 virt** | ✅ | Fully supported reference platform |
| **QEMU AMD64 q35** | ✅ | Fully supported |
| **SMP (4 Cores)** | ✅ | Tested on both architectures |
| **VirtIO GPU / Keyboard / Mouse / Block** | ✅ | Verified working |
| **GPT & Ext4** | ✅ | Verified partition and filesystem support |
| **macOS (Intel/Apple Silicon)** | ✅ | Officially supported via `setup-toolchain-macos.sh` |
| **Linux (Ubuntu)** | ✅ | Officially supported via `setup-toolchain-linux.sh` |
| **Linux (Debian / Arch / Alpine)** | ✅ | Toolchain verified; environment features auto-detection |
| **BSD Derivatives** | ⬜ | Not yet supported |
| **UTM (AMD64 ISO release)** | ✅ | Verified test on UTM (virtio-pci-gpu / PS/2 input) |

---

## 🖼 Screenshots

### Variable Themes & Styling Support
Below are live captures demonstrating window compositor features, overlap handling, typography, and styling systems running inside QEMU:

<p align="center">
  <img width="90%" alt="Theme Showcase 1" src="https://github.com/user-attachments/assets/a1e41dea-feca-462c-a39e-18eb463bfe5f" />
</p>

<p align="center">
  <img width="90%" alt="Screenshot 2026-07-14 alle 15 57 39" src="https://github.com/user-attachments/assets/ee0fca9f-f2b2-4b19-a09d-92506e72d7c1" />
</p>

---

## 🛠 Prerequisites & Toolchain Setup

To compile and execute NexsOS1, use the host-detecting `tools/setup-project.sh` bootstrap. The intended build hosts are macOS Intel, macOS Apple Silicon and Linux (Ubuntu/Debian/Arch/Alpine); each host/target pair must pass the strict toolchain checks and both architecture builds before it is marked verified. BSD is not yet supported.

The cross-toolchain versions are owned by **NX-SDK** in `system/config/toolchain-versions.env`. The root setup scripts are thin integration wrappers around the SDK development-tooling entrypoints, and `make check` verifies the same lock: `x86_64-elf-gcc` 16.1.0 with Binutils 2.46.1, and `aarch64-none-elf-gcc` 7.2.0 with Binutils 2.29.1. Linux builds the GNU toolchains from the versioned source releases and verifies official SHA-512 checksums before extraction; macOS installs the matching Homebrew/osxct packages and rejects version drift. On Apple Silicon, the pinned osxct compiler currently requires Rosetta 2 and Intel Homebrew under `/usr/local`. The bootstrap also initializes the external userland repositories (SDL, Mesa, Wine/D3D, Lua, musl, Gnulib, BusyBox, Coreutils, DOOM, kilo, and base-nexs) at the exact Git revisions pinned by the superproject.

*(Note: On WSL2, the script automatically prints configuration guides for WSLg graphics and KVM permissions.)*

### 1. Clone the Repository
```bash
git clone https://github.com/olmox001/NexsOS1
cd NexsOS1
```

### 2. Reproducible Project Setup
Run the single host-detecting bootstrap entry point. It initializes every
recursive Git submodule at the exact commit recorded by the superproject,
then installs/checks the host build dependencies and cross-toolchains:

```bash
./tools/setup-project.sh
```

Run this bootstrap once after cloning. It initializes the pinned recursive
submodules and installs the complete host toolchain, including the GRUB ISO
build tools used by nxadb/nxrun. Build and run targets never invoke setup
scripts.

For a read-only dependency verification later (without reinitializing
submodules), use:

```bash
make check-deps
```

make setup-deps is a manual recovery command only if the initial submodule
checkout was skipped or incomplete. The submodule gitlinks, not the optional
branch hints in .gitmodules, are the source of truth for dependency versions.

### 3. Verify the Installation
Ensure the cross-compilers and emulators are available:
```bash
make check ARCH=aarch64
make check ARCH=amd64
```

---

## 💻 Compilation & Execution

NexsOS1's normal developer run uses the same GRUB boot media as the
interactive `nxadb`/`nxrun` tests and release path. This is the authoritative
boot-chain test: AMD64 uses GRUB BIOS + Multiboot2; AArch64 uses GRUB arm64-efi,
the project EFI handoff stub, and a bounded DTB handoff (`x0` address,
`x1` validated size).

```bash
make run ARCH=aarch64
make run ARCH=amd64
```

The raw QEMU kernel-loader path is retained for low-level diagnostics only:

```bash
make run ARCH=aarch64 BOOT_MODE=direct
make run ARCH=amd64 BOOT_MODE=direct
# Equivalent compatibility targets: make run-direct ARCH=<arch>
```

Direct AArch64 loading supplies the DTB pointer in `x0` but has no explicit
length register; it therefore exercises the documented legacy pointer-only
adapter (`x1=0`), not the sized EFI handoff. Direct AMD64 loading also bypasses
the GRUB Multiboot2 entry and must be treated as a separate diagnostic path,
not as bootable installation media. Neither direct mode replaces GRUB boot
verification.

For a headless GRUB ISO run, add `RUN_DISPLAY=none` and
`RUN_SERIAL=file:build/runs/run-ARCH.serial`.

### Additional Build Targets

| Target Command | Description |
| :--- | :--- |
| `make all ARCH=<arch>` | Compiles the operating system without launching QEMU. |
| `make debug ARCH=<arch>` | Boots QEMU with GDB stub debugging enabled (`-s -S`). |
| `make release VERSION=x.y` | Builds release-ready packages (e.g., hybrid, bootable AMD64 ISOs via GRUB). |
| `make clean` | Removes architecture/GRUB build outputs while preserving the pinned source cache and diagnostic/runtime logs. |

The public userspace API is owned by the pinned **NX-SDK** submodule under `system/`. Its primary surface is `system/include/api/` (especially `os1.h`) together with the versioned wire contracts under `system/include/contract/`. NX-CORE carries an independently checksummed kernel-side ABI snapshot; the superproject verifies the two repositories for drift.

NX-SDK also owns the versioned **development-tool plane** (cross-toolchain lock, setup/build tooling and SDK development headers). NX-CORE may resolve that tooling from the integrated superproject, an installed SDK, or its standalone SDK cache. This does **not** make the SDK userland API/runtime a kernel dependency: kernel/HAL translation units compile against NX-CORE-owned headers and the kernel image must not link `libnxsdk*.a`. `make -C kernel check-sdk-boundary` enforces this boundary.

SDK installation is componentized so tool updates do not require reinstalling the target runtime: `make -C system install-tools`, `install-headers`, and per-architecture `install-runtime`/`install-runtime-core` install under a versioned prefix with a `current` selector.

---

## 📁 Project Layout

```text
.
├── docs/legacy/                       # Architecture, reviews, reports and historical plans
├── kernel/                     # NX-CORE pinned Git submodule
│   ├── external/grub/          # Pinned external GRUB source used by NX-CORE boot media
│   ├── HAL/                    # ISA adapters and hardware abstraction contracts
│   ├── core/                   # Object manager, dispatch and kernel mechanisms
│   ├── sched/                  # Process scheduling and lifecycle
│   ├── mm/                     # Memory management
│   ├── fs/                     # Existing filesystem/VFS providers
│   ├── drivers/                # Device drivers (UART extraction from HAL pending)
│   ├── resources/              # Capability resource providers
│   ├── graphics/               # Remaining legacy window/terminal/font mechanisms
│   └── user/extension/         # Machine-owned user-mode bootstrap extension
├── system/                     # NX-SDK pinned Git submodule
│   ├── arch/                   # Architecture-specific SDK entry veneers
│   ├── include/api/            # Public SDK and C/POSIX personality declarations
│   ├── include/contract/       # SDK-facing versioned wire declarations
│   ├── tools/                  # Versioned cross-toolchain/build development tooling
│   └── lib/                    # Runtime, graphics, codecs and client extensions
├── user/
│   ├── sbin/                   # Root initialization policy
│   ├── sys/boot/               # Installed boot entry and service manifest
│   ├── sys/services/           # Supervised system daemons
│   ├── sys/bin/                # System commands and graphical interfaces
│   ├── bin/                    # Applications, tests and pinned upstream programs
│   ├── lib/                    # Pinned external libraries
│   ├── portability/            # Upstream adaptation layers
│   └── home/                   # Initial user files and assets
└── tools/                      # Integration orchestration, Power12 and nxadb acceptance
```

---

## 🗺 The ASTRA Roadmap

### Core Kernel Foundation
- [x] Clean-boot on AArch64 and AMD64
- [x] Architectural HAL with complete ISA insulation
- [x] Preemptive SMP scheduling with work-stealing queues
- [x] Unified Physical and Virtual Memory Managers
- [x] Higher Half Kernel Mapping
- [x] Rigid $W \oplus X$ memory protection
- [x] Thread/Context Isolation using ASID/PCID
- [x] Secure ELF64 loader
- [x] Zero-copy synchronous IPC primitives
- [x] Capability-based system security model
- [x] Object Manager abstraction
- [x] Unified VirtIO Drivers (MMIO & PCI)
- [x] Capability-based Virtual File System (VFS)
- [x] Ext4 filesystem layout support (Extent Trees)
- [x] System registry as a hierarchical VFS namespace
- [x] Native window graphics compositor
- [x] Intuitive Window Manager
- [x] Supervised initialization service (`init` supervisor)

### ASTRA Phase B — Architectural Maturation
- [x] **B1:** High-fidelity VFS integration as a core ASTRA provider
- [x] **B2:** Complete, architecture-agnostic memory management model
- [x] **B3:** Coherent Capability ABI validation & unified Object Manager
- [ ] **B4:** Complete AMD64 platform parity (native ACPI / MADT mapping)
- [ ] **B5:** Complete separation of the HAL and the Service Runtime Layer (SRL)
- [ ] **B6:** SMP refinement and asynchronous I/O event loops

### ASTRA Phase C — Service Demoting (Microkernel Boundary)
- [ ] Formalization of the low-level `OS1low` ABI
- [ ] Demoting system drivers to supervised userland tasks
- [ ] User-space GPU Service
- [ ] User-space Network Service Stack
- [ ] User-space Audio Service
- [ ] User-space Input Handling Daemon
- [ ] User-space System Registry daemon
- [ ] Migrating the Compositor into a dedicated user-space application
- [ ] Separating the Window Server completely from kernel authority

### ASTRA Phase D — Standard Userland Porting
- [ ] Development of the official native `libOS1` userland library
- [ ] Upstream port of the `musl` C library to the native OS1 ABI
- [ ] Full POSIX standards personality layer
- [ ] Lua runtime engine integration
- [ ] Full integration of standard `BusyBox` utilities

### ASTRA Phase E — Optimization & Hardening
- [ ] Extensive optimization of the userland runtimes
- [ ] Minimization of boot-time and RAM footprint
- [ ] High-performance low-latency scheduler and ultra-fast IPC paths
- [ ] Core System Auditing and sandboxing capabilities
- [ ] Kernel hardening, fault recovery, and DWARF backtrace debugging

### Hardware support targets
- [ ] Networking
- [ ] Audio
- [ ] USB complete support
- [ ] Hardware graphics acceleration
- [ ] Advanced storage drivers

### File System targets
- [ ] Journaling support
- [ ] Dynamic mounts
- [ ] Support for multiple filesystems
- [ ] Fully capability-based VFS

### Security targets
- [ ] Full syscall auditing
- [ ] Hardening kernel
- [ ] Recovery mode
- [ ] Backtrace DWARF parser
- [ ] Advanced sandboxing capabilities

---

## 🤝 Democratic Development & Contributing

NexsOS1 is, first and foremost, a free and open-source project. While it began as a personal research initiative, the long-term goal is to build an operating system that belongs to its community. Everyone is welcome to participate, whether by writing code, reporting bugs, reviewing the architecture, improving the documentation, testing on new hardware, or simply sharing ideas.

Technical choices are made democratically:
*   **Democratic Architecture:** Major architectural decisions, API layouts, and roadmap adjustments should reflect community consensus rather than unilateral design.
*   **GitHub Discussions:** We use Discussions as our town square. We highly encourage developers, researchers, students, and enthusiasts to raise technical inquiries, debate architectural improvements, propose new subsystems, and host community polls! All requests will be taken into consideration.

### How to Contribute
1. Fork the repository.
2. Create a dedicated feature branch (`feature/your-feature`, `fix/your-fix`, `refactor/your-refactor`).
3. Keep the coding style consistent: **K&R Style, 2-space indentation**, and a warnings-clean build with strict compiler flags (`-Werror -Wall -Wextra -Wpedantic -Wshadow`).
4. Avoid regressions on both supported architectures (AArch64 and AMD64) whenever possible.
5. Update documentation when introducing significant architectural changes.
6. For large changes, please open a Discussion or an Issue before starting the implementation.

---

## 🎨 Graphics Portability

The OpenGL, D3D9, and SDL2 port program, its ASTRA boundary, and validation log reside in [`docs/legacy/graphics-port/`](docs/legacy/graphics-port/README.md).

---

## 🙏 Acknowledgments

NexsOS1 stands on the shoulders of the incredible open-source community. Key portions of the userland system are maintained as custom forks specifically optimized for the native OS1 APIs.

Special thanks to the authors and maintainers of:
*   **BusyBox:** The foundational utility provider for Unix-style userlands.
*   **Kilo:** The incredibly lightweight text editor adapted to OS1.
*   **Doom Generic:** Providing the core codebase for our graphics validation.
*   **SDL2:** Serving as our multimedia abstraction target.
*   **Mesa:** The baseline foundation for our future OpenGL compatibility.
*   **Wine:** The core source of inspiration and architectural reference for our Direct3D 9 compatibility layer.
*   **musl libc:** The lightweight standard C library being adapted to our system ABI.
*   **Lua:** The ultra-fast scripting engine scheduled for system-wide integration.
*   **base-nexs:** Portable script languages.

We also draw technical inspiration from pioneers in operating system research, notably **Linux**, **Plan 9 from Bell Labs**, **seL4**, **Fuchsia**, **Windows NT**, **Darwin (XNU)**, and **TempleOS**. These projects are sources of ideas and architectural inspiration only; NexsOS1 is an independent implementation developed from scratch.

Finally, thanks to everyone who tests the project, reports bugs, reviews the architecture, proposes improvements, or simply takes the time to explore the code. Every contribution helps make NexsOS1 a better operating system.

---

## 📄 License

This project is distributed under the **GNU General Public License v2 (GPL-2.0)**. See the [LICENSE](LICENSE.md) file for the complete terms and conditions.

---

> _**A Quick Dev Note:** If I had to stop to document everything I do I would go crazy, excuse me if some descriptions are dated! Sorry for my spaghetti-eating English too!_ 🍝
