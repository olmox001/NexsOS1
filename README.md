# NexsOS1 (OS1)

### Multi-architecture open-source hybrid-kernel operating system

[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](LICENSE.md)
[![Arch](https://img.shields.io/badge/arch-aarch64%20%7C%20amd64-green.svg)](#)
[![Platform](https://img.shields.io/badge/platform-QEMU-orange.svg)](https://www.qemu.org/)
[![Language](https://img.shields.io/badge/language-C99-yellow.svg)](https://en.wikipedia.org/wiki/C99)

NexsOS1 (codename **OS1**) is an educational and research hybrid-kernel operating system built from scratch. It boots on both ARM64 (AArch64) and x86-64 (AMD64) under QEMU, brings up Symmetric Multiprocessing (SMP), drives native **VirtIO** devices (GPU, input, block), mounts an **Ext4** root filesystem, composites overlapping windows in a custom graphical user interface, and executes user-space ELF binaries with an interactive **TTY shell**.

> **Honesty note.** This README describes the *verified* state. For the complete, evidence-based picture — including bugs, gaps, and severity — see [`docs/review/REVIEW.md`](docs/review/REVIEW.md). For where the project is *going* (a seL4-style, Plan 9-inspired Microkernel), see [`docs/PROJECT_CHARTER.md`](docs/PROJECT_CHARTER.md). This project is licensed under **GPL v2** (see [`LICENSE.md`](LICENSE.md)).

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

*   **Architectural Abstraction Layer (HAL):** The HAL provides complete transparency to the kernel. There are no architecture-specific `#define` statements within the common kernel codebase, ensuring easy portability to other architectures.
*   **Secure Memory Paging:** Physical (zone-based PMM bitmap allocator) and Virtual Memory Managers handle paging. On AMD64, the system enforces the strict **$W \oplus X$ (Write or Execute)** security protocol (executable text is RX, read-only data is RO+NX, all other regions are RW+NX) alongside Higher-Half kernel mappings (`0xFFFF000000000000` on AArch64 via TTBR1, `0xFFFF800000000000` on AMD64), with cross-CPU TLB shootdown.
*   **Preemptive SMP & Multiprocessing:** Preemptive O(1) SMP scheduler with per-CPU priority run-queues and work-stealing. Cores are initialized and managed by user-space idle tasks once primary boot completes. Multiple processes are cleanly distributed across cores, with full support for `child` creation, `kill` termination, and an `exec` utility managing terminal, graphical, and integrated terminal modes.
*   **Capability-Based Security:** Syscalls and system resources are checked and restricted through a capabilities security model mapped to abstract user levels (`Machine`, `Root`, `User`, `Guest`). The only processes allowed to run at the highly privileged `Machine` level are `nxinit` (early init) and the idle tasks.
*   **Graphics Stack & Window Compositor:** GPU abstraction, display drivers (`virtio-pci-device` and `virtio-gpu-device`), and the compositor are currently inside the kernel (rendering overlapping windows, drag, focus, and Z-order). The compositor processes individual application framebuffers rendered in userland. Supports custom themes and styles; all interface components are compiled as independent ELF binaries.
*   **Isolated Userland & Release System:** The userland environment resides in a dedicated disk image (`disk.img`), allowing independent loading of the kernel and filesystem in QEMU for rapid testing. At boot, the kernel loads `disk.img` into a temporary RAM disk, and launches the `init` supervisor. `init` manages core services, respawning them automatically if they terminate. The release tool compiles a hybrid, bootable AMD64 ISO via GRUB.
*   **Fault & Trace Isolation:** Recoverable fault handling with total vector coverage on both architectures. A userland application crash is isolated and never kills the kernel, leaving the shell and compositor running. Provides symbolized in-kernel backtraces (`.ksyms`) on dedicated fault stacks.
*   **VFS & Ext4 File System:** Virtual File System (with mount tables and `fs_ops` providers) supporting Ext4 (extent-tree reads, legacy reads, INCOMPAT enforcement, and extended write paths) and GPT partition mapping with MBR fallback, and a buffer cache.
*   **Coherent Syscall ABI:** Single unified syscall numbering (`include/api/syscall_nums.h`) compiled into both the kernel dispatcher and the userland stubs, returning negative `errno` values.

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
  <img width="90%" alt="Theme Showcase 2" src="https://github.com/user-attachments/assets/35f8dfc8-069a-44fa-9aaa-58ad80edb1e3" />
</p>

<p align="center">
  <img width="90%" alt="TTY Shell and Desktop" src="https://github.com/user-attachments/assets/218a4ebf-b848-4f3f-9f4e-4d93a49cd307" />
</p>

<p align="center">
  <img width="90%" alt="Compositor Window Stacking" src="https://github.com/user-attachments/assets/137020f5-9ca3-45a5-a330-ac891b4dc0e1" />
</p>

---

## 🛠 Prerequisites & Toolchain Setup

To compile and execute NexsOS1, you will need the official cross-compiler toolchain. The official toolchain can be installed via `setup-toolchain-macos.sh` and `setup-toolchain-linux.sh`. The build system has been fully verified on macOS (Intel/Apple Silicon) and Linux (Ubuntu, Debian, and Arch). BSD is not yet supported.

The setup scripts automate the builds of pinned GNU compilers directly from source (`x86_64-elf-gcc` 13.2.0, `aarch64-none-elf-gcc` 7.2.0), taking about 10-30 minutes. It also auto-downloads the required userland repositories from GitHub, including ported libraries and applications such as `kilo`, `freedoom`, `sdl`, `musl`, `busybox`, `lua`, `opengl`, and `direct3d9`.

*(Note: On WSL2, the script automatically prints configuration guides for WSLg graphics and KVM permissions.)*

### 1. Clone the Repository
```bash
git clone https://github.com/olmox001/NexsOS1
cd NexsOS1
```

### 2. Configure Your Build Environment
Run the corresponding script for your operating system:

*   **For Linux (Ubuntu, Debian, Arch, Alpine):**
    ```bash
    ./tools/setup-toolchain-linux.sh
    ```

*   **For macOS:**
    ```bash
    ./tools/setup-toolchain-macos.sh
    ```

### 3. Verify the Installation
Ensure the cross-compilers are correctly linked and ready:
```bash
make check ARCH=aarch64
make check ARCH=amd64
```

---

## 💻 Compilation & Execution

NexsOS1 uses a streamlined `Makefile` interface. To compile the bootloader, kernel, and userland disk image, and immediately boot them in QEMU, run:

```bash
# Build and run the ARM64 (AArch64) graphical system (Reference)
make run ARCH=aarch64

# Build and run the x86_64 (AMD64) graphical system
make run ARCH=amd64
```

### Additional Build Targets

| Target Command | Description |
| :--- | :--- |
| `make all ARCH=<arch>` | Compiles the operating system without launching QEMU. |
| `make debug ARCH=<arch>` | Boots QEMU with GDB stub debugging enabled (`-s -S`). |
| `make release VERSION=x.y` | Builds release-ready packages (e.g., hybrid, bootable AMD64 ISOs via GRUB). |
| `make clean` | Wipes build outputs. Strongly recommended after userland changes in `user/` to make modifications effective. |

The official kernel API for userspace can be found inside `include/api/`, focusing primarily on `os1.h` and `object.h`.

---

## 📁 Project Layout

```text
.
├── boot/                        # Stage 1 & Stage 2 bootloaders and linker scripts
│   ├── aarch64/
│   └── amd64/
├── docs/                        # Technical documentation, specifications, and reports
│   ├── LOGO/                    # Graphical assets and project logotypes
│   ├── direction/               # Architectural plans, design mandates, and directions
│   ├── graphics-port/           # Graphic system porting and validation logs
│   ├── man/                     # System manuals and user references
│   ├── report/                  # Performance, debugging, and analytical reports
│   ├── review/
│   │   └── analysis/            # Code reviews, bug taxonomy, and findings
│   ├── screen/                  # Architectural screenshots and system captures
│   └── userland-port/           # Guides and porting updates for the userspace
├── include/
│   └── api/                     # Public system API definitions for userland
│       └── sys/                 # Native C library definitions (os1.h, object.h)
├── kernel/
│   ├── arch/                    # Architecture-specific abstraction layers
│   │   ├── aarch64/             # ARM64 CPU setup, MMU page tables, GIC, PL011 drivers
│   │   └── amd64/               # AMD64 CPU setup, APIC, IOAPIC, PIT, serial drivers
│   ├── core/                    # Core kernel engine (Syscall dispatcher, device bus)
│   ├── drivers/                 # Unified hardware drivers
│   │   ├── block/               # Mass storage block drivers
│   │   ├── gic/                 # ARM Interrupt Controllers
│   │   ├── gpu/                 # GPU framebuffers and VirtIO screen devices
│   │   ├── keyboard/            # Key input devices
│   │   ├── pci/                 # Peripheral Component Interconnect bus driver
│   │   ├── ps2/                 # Traditional PS/2 mouse and keyboard
│   │   ├── timer/               # System clocks
│   │   ├── uart/                # Serial logging interfaces
│   │   ├── usb/                 # Universal Serial Bus (USB) driver stub
│   │   └── virtio/              # Virtual I/O drivers (GPU, Block, Input)
│   ├── fs/                      # Virtual File System (VFS) and Ext4 disk system
│   ├── graphics/                # Compositor engine, TTF fonts, and window servers
│   │   └── logo/
│   ├── include/                 # Internal, private kernel headers
│   ├── irq/                     # Global interrupt request dispatchers
│   ├── lib/                     # In-kernel shared helper libraries (printf, kmalloc)
│   ├── mm/                      # Memory management (Bitmap PMM, Virtual MM, Heap)
│   └── sched/                   # Preemptive priority scheduler and ELF loader
├── tools/
│   ├── kernel_doctor/           # Technical debugging, tracing, and analysis suite
│   └── mkdisk.c                 # Tool for packaging userland into an Ext4 disk image
└── user/
    ├── arch/                    # Architecture-specific runtime startup routines
    │   ├── aarch64/
    │   └── amd64/
    ├── bin/                     # Standalone user applications and demos
    │   ├── base-nexs/           # Native testing and compatibility tools
    │   ├── busybox/             # Unix terminal utilities
    │   ├── doom/                # Ported Doom engine
    │   └── kilo/                # Ported ultra-lightweight text editor
    ├── home/                    # System root home directory structure
    │   └── Pictures/
    └── sys/                     # Operating system critical subsystems
        ├── bin/                 # Userland base services (init, shell, panel, dock)
        └── lib/                 # Core shared library runtimes (libos1, libc-stubs)
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

The OpenGL, D3D9, and SDL2 port program, its ASTRA boundary, and validation log reside in [`docs/graphics-port/`](docs/graphics-port/README.md).

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
*   **base-nexs:** Native test suite and compatibility applications for the OS1 API.

We also draw technical inspiration from pioneers in operating system research, notably **Linux**, **Plan 9 from Bell Labs**, **seL4**, **Fuchsia**, **Windows NT**, **Darwin (XNU)**, and **TempleOS**. These projects are sources of ideas and architectural inspiration only; NexsOS1 is an independent implementation developed from scratch.

Finally, thanks to everyone who tests the project, reports bugs, reviews the architecture, proposes improvements, or simply takes the time to explore the code. Every contribution helps make NexsOS1 a better operating system.

---

## 📄 License

This project is distributed under the **GNU General Public License v2 (GPL-2.0)**. See the [LICENSE](LICENSE.md) file for the complete terms and conditions.

---

> _**A Quick Dev Note:** If I had to stop to document everything I do I would go crazy, excuse me if some descriptions are dated! Sorry for my spaghetti-eating English too!_ 🍝