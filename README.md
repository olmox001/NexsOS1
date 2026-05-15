# 🚀 OS1TEST-DEV

### Production-Ready AArch64 Microkernel with Graphics & Multitasking

<div align="center">
<img width="1281" height="1007" alt="Screenshot 2026-05-14 alle 05 02 06" src="https://github.com/user-attachments/assets/3bbb8715-55ad-489d-b2e6-e8404014c6a4" />







[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Architecture](https://img.shields.io/badge/arch-AArch64-green.svg)](https://developer.arm.com/architectures/learn-the-architecture/armv8-a-instruction-set-architecture)
[![QEMU](https://img.shields.io/badge/platform-QEMU%20virt-orange.svg)](https://www.qemu.org/)
[![Language](https://img.shields.io/badge/language-C99-yellow.svg)](https://en.wikipedia.org/wiki/C99)

*A modern, lightweight operating system for ARM64 featuring real-time graphics, preemptive multitasking, and Ext4 filesystem support.*

[Features](#-features) • [Quick Start](#-quick-start) • [Architecture](#-architecture) • [Documentation](#-documentation) • [Contributing](#-contributing)

</div>

---

## 📋 Table of Contents

- [Overview](#-overview)
- [Features](#-features)
- [Screenshots](#-screenshots)
- [System Requirements](#-system-requirements)
- [Quick Start](#-quick-start)
- [Architecture](#-architecture)
- [Project Structure](#-project-structure)
- [Technical Details](#-technical-details)
- [Roadmap](#-roadmap)
- [Contributing](#-contributing)
- [License](#-license)
- [Acknowledgments](#-acknowledgments)

---

## 🎯 Overview

**OS1TEST-DEV** is a fully functional microkernel operating system designed for the AArch64 (ARM64) architecture. Built from scratch in C, it demonstrates modern OS concepts including virtual memory management, preemptive multitasking, filesystem support, and a graphical windowing system.

### Why OS1TEST-DEV?

- 🎓 **Educational**: Perfect for learning OS development and ARM64 architecture
- 🔧 **Production-Ready**: Clean, well-documented codebase following best practices
- 🖼️ **Graphics-First**: Native GUI with window compositor and terminal emulator
- ⚡ **Zero-Copy I/O**: Optimized disk access through intelligent buffer caching
- 🔒 **Memory Safe**: MMU-enforced process isolation with 4-level page tables

---

## ✨ Features

### Core System

- ✅ **Physical Memory Manager** (PMM)
  - Bitmap-based page frame allocator
  - Zone-based allocation (DMA/Normal)
  - Contiguous and aligned allocation support
  
- ✅ **Virtual Memory Manager** (VMM)
  - AArch64 4-level page tables (L0-L3)
  - MMU with instruction and data caching
  - Per-process address spaces
  - Identity-mapped kernel space

- ✅ **Process Management**
  - Preemptive multitasking (round-robin scheduler)
  - ELF64 binary loader with dynamic mapping
  - User/Kernel mode separation (EL0/EL1)
  - Hardware context switching (100Hz timer)

### Filesystem & Storage

- ✅ **Ext4 Read-Only Driver**
  - Direct and indirect block support (up to 4MB files)
  - Directory traversal and inode lookup
  - Zero-copy buffer cache with LRU eviction
  
- ✅ **GPT Partition Table**
  - Automatic partition detection
  - GUID-based partition identification

- ✅ **VirtIO Block Driver**
  - High-performance disk I/O
  - DMA-capable buffer management

### Graphics & User Interface

- ✅ **Window Compositor**
  - Multiple overlapping windows with Z-ordering
  - Alpha blending and transparency support
  - Window dragging and focus management
  - Anti-aliased rendering

- ✅ **Terminal Emulator**
  - ANSI escape sequence support (colors, cursor control)
  - Text scrolling with line wrapping
  - Per-window terminal state

- ✅ **2D Graphics Engine**
  - Bresenham line algorithm
  - Circle and triangle primitives
  - Gradient fills and rounded rectangles
  - Hardware-accelerated double buffering

- ✅ **3D Software Renderer**
  - Fixed-point mathematics (no FPU required)
  - Z-buffer depth testing
  - Matrix transformations and perspective projection
  - Wireframe cube rendering

### Device Drivers

- ✅ **VirtIO-GPU** - Hardware-accelerated framebuffer
- ✅ **VirtIO-Block** - High-speed disk access
- ✅ **GICv2** - Generic Interrupt Controller
- ✅ **ARM Generic Timer** - System tick generation
- ✅ **PL011 UART** - Serial console for debugging
- ✅ **PS/2 Keyboard** - Keyboard input handling

### Advanced Features

- 🔐 **Spinlocks** - SMP-safe synchronization primitives
- 🧮 **Fixed-Point Math** - Integer-only trigonometry for graphics
- 📝 **Kernel Heap** - Dynamic memory allocation (kmalloc/kfree)
- 🔍 **Intrusive Lists** - Linux-style container_of pattern
- 🖱️ **Mouse Support** - Cursor rendering and click handling

---

## 📸 Screenshots

### Desktop Environment

<img width="1301" height="1068" alt="Screenshot 2026-01-05 alle 08 30 41" src="https://github.com/user-attachments/assets/ebe7499f-1c5c-45ad-aa49-2580b733a8c2" />
---

## 💻 System Requirements

### Build Environment

- **Cross-Compiler**: `aarch64-linux-gnu-gcc` (GCC 11+ recommended)
- **Assembler**: `aarch64-linux-gnu-as`
- **Build Tools**: GNU Make, binutils
- **Optional**: `qemu-system-aarch64` for testing

### Runtime Environment

- **Emulator**: QEMU 6.0+ (`qemu-system-aarch64`)
- **Machine**: `virt` (QEMU ARM Virtual Machine)
- **Memory**: 1GB RAM minimum
- **Storage**: 512MB disk image with Ext4 partition

### Tested Platforms

- ✅ QEMU 7.2.0 on Linux x86_64
- ✅ QEMU 8.0.0 on macOS ARM64
- ⚠️ Real hardware support pending (requires device tree modifications)

---

## 🚀 Quick Start

### 1. Install Dependencies
**to change debug info**
int console_loglevel = warning (o info)

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu qemu-system-arm make git
```

**macOS (Homebrew):**
```bash
brew install aarch64-elf-gcc qemu make
```

**Arch Linux:**
```bash
sudo pacman -S aarch64-linux-gnu-gcc qemu-arch-extra make
```

### 2. Clone Repository

```bash
git clone https://github.com/olmox001/os1test-dev.git
cd os1test-dev
```

### 3. Build

```bash
make clean
make all
```

This will generate:
- `kernel.bin` - Kernel binary
- `bootloader.bin` - Boot loader
- `os.iso` (optional) - Bootable ISO image

### 4. Create Disk Image

```bash
# Create 512MB disk image
dd if=/dev/zero of=disk.img bs=1M count=512

# Partition with GPT
# (Use gdisk or parted to create partitions)

# Format partition 3 as Ext4
mkfs.ext4 -L "Userland" disk.img.p3

# Copy user binaries
sudo mount disk.img.p3 /mnt
sudo cp user/init user/shell /mnt/
sudo umount /mnt
```

### 5. Run

```bash
qemu-system-aarch64 \
  -machine virt \
  -cpu cortex-a57 \
  -m 1G \
  -kernel kernel.bin \
  -drive file=disk.img,if=none,id=hd0,format=raw \
  -device virtio-blk-device,drive=hd0 \
  -device virtio-gpu-pci \
  -serial stdio \
  -nographic  # Remove for graphical output
```

**Expected Output:**
```
========================================
  AArch64 Microkernel v0.1.0
  Production-Ready AArch64 Kernel
========================================

[INFO] Initializing CPU...
[INFO] Initializing GIC...
[INFO] Initializing timer...
[INFO] PMM: 1024 MB total, 1008 MB free
[INFO] VMM: MMU Enabled. Kernel PGD at 0x40001000
[INFO] Ext4: Mounted. Vol=Userland, Inodes=65536
[INFO] Scheduler: Loaded /init
[INFO] Scheduler: Loaded /shell (1)
[INFO] Scheduler: Loaded /shell (2)
```

---

## 🏗️ Architecture

### System Architecture Diagram

```
┌─────────────────────────────────────────────────────┐
│                  User Space (EL0)                   │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐          │
│  │  Shell   │  │   Init   │  │  Custom  │          │
│  │ Process  │  │ Process  │  │   Apps   │          │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘          │
└───────┼─────────────┼─────────────┼─────────────────┘
        │             │             │
        └─────────────┴─────────────┘ System Calls
                      │
┌─────────────────────┼─────────────────────────────┐
│              Kernel Space (EL1)                   │
│                                                   │
│  ┌─────────────────────────────────────────────┐ │
│  │         Process Scheduler (Round-Robin)     │ │
│  │   Context Switch • ELF Loader • pt_regs    │ │
│  └──────────────────┬──────────────────────────┘ │
│                     │                            │
│  ┌─────────────────────────────────────────────┐ │
│  │      Virtual Memory Manager (VMM)           │ │
│  │   4-Level Page Tables • TLB • ASID         │ │
│  └──────────────────┬──────────────────────────┘ │
│                     │                            │
│  ┌─────────────────────────────────────────────┐ │
│  │   Physical Memory Manager (PMM)             │ │
│  │   Bitmap Allocator • Zones • Page Frames   │ │
│  └─────────────────────────────────────────────┘ │
│                                                   │
│  ┌──────────────┐  ┌──────────────────────────┐ │
│  │  Filesystems │  │    Graphics Subsystem    │ │
│  │              │  │                          │ │
│  │ • Ext4 (RO) │  │ • Compositor             │ │
│  │ • GPT Parser│  │ • 2D/3D Renderer         │ │
│  │ • Buffer    │  │ • Double Buffering       │ │
│  │   Cache     │  │ • Alpha Blending         │ │
│  └──────┬───────┘  └───────────┬──────────────┘ │
│         │                      │                 │
│  ┌──────┴──────────────────────┴──────────────┐ │
│  │          Device Drivers                    │ │
│  │  VirtIO-Block • VirtIO-GPU • GIC • Timer  │ │
│  │  UART • Keyboard • Mouse                  │ │
│  └────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────┐
│      Hardware (QEMU virt)       │
│  Cortex-A57 • GICv2 • MMIO     │
└─────────────────────────────────┘
```

### Memory Layout

```
Virtual Address Space (48-bit):

User Space (TTBR0_EL1):
0x0000_0000_0000_0000 ┌──────────────────────┐
                      │   User Code & Data   │
0x0000_7FFF_FFFF_FFFF └──────────────────────┘

Kernel Space (TTBR1_EL1):
0xFFFF_0000_0000_0000 ┌──────────────────────┐
                      │  Physical Memory     │
                      │  Identity Map        │
0xFFFF_8000_0000_0000 ├──────────────────────┤
                      │  Kernel Image        │
                      │  .text .data .bss    │
0xFFFF_FFFF_FE00_0000 ├──────────────────────┤
                      │  MMIO Devices        │
                      │  (UART, GIC, etc.)   │
0xFFFF_FFFF_FFFF_FFFF └──────────────────────┘

Physical Memory (1GB):
0x4000_0000 ┌──────────────────────┐
            │  DMA Zone (16MB)     │
0x4100_0000 ├──────────────────────┤
            │  Kernel Image        │
0x4200_0000 ├──────────────────────┤
            │  Available Pages     │
            │  (Managed by PMM)    │
0x8000_0000 └──────────────────────┘
```

---

## 📁 Project Structure

```
os1test-dev/
├── boot/                      # Bootloader
│   ├── boot.s                # Assembly entry point
│   └── linker.ld            # Bootloader linker script
│
├── kernel/                    # Kernel core
│   ├── kernel.c              # Main initialization
│   ├── sched/                # Scheduler & processes
│   │   ├── process.c        # Process management
│   │   ├── elf.c            # ELF64 loader
│   │   └── schedule.c       # Context switching
│   ├── mm/                   # Memory management
│   │   ├── pmm.c            # Physical memory
│   │   ├── vmm.c            # Virtual memory
│   │   └── buffer.c         # Buffer cache
│   ├── fs/                   # Filesystems
│   │   ├── ext4.c           # Ext4 driver
│   │   └── gpt.c            # GPT parser
│   ├── graphics/             # Graphics subsystem
│   │   ├── graphics.c       # Core graphics
│   │   ├── compositor.c     # Window manager
│   │   ├── draw2d.c         # 2D primitives
│   │   ├── draw3d.c         # 3D renderer
│   │   └── font.c           # Bitmap fonts
│   └── lib/                  # Kernel library
│       ├── string.c         # String functions
│       ├── printk.c         # Kernel printf
│       ├── kmalloc.c        # Heap allocator
│       └── math.c           # Fixed-point math
│
├── drivers/                   # Device drivers
│   ├── uart.c                # Serial console
│   ├── gic.c                 # Interrupt controller
│   ├── timer.c               # System timer
│   ├── virtio_blk.c          # Block device
│   ├── virtio_gpu.c          # Graphics device
│   └── keyboard.c            # Keyboard input
│
├── include/kernel/            # Kernel headers
│   ├── types.h               # Base types
│   ├── pmm.h                 # PMM API
│   ├── vmm.h                 # VMM API
│   ├── sched.h               # Scheduler API
│   ├── graphics.h            # Graphics API
│   └── ...
│
├── user/                      # User-space programs
│   ├── init.c                # Init process
│   └── shell.c               # Shell program
│
├── tools/                     # Build utilities
│   └── mkdisk.sh            # Disk image creator
│
├── Makefile                   # Build system
├── grub.cfg                   # GRUB configuration
└── README.md                  # This file
```

---

## 🔬 Technical Details

### Interrupt Handling

The system uses the **ARM Generic Interrupt Controller (GICv2)** for interrupt management:

```c
Exception Vector Table (EL1):
  0x000 - Synchronous (EL1t)
  0x080 - IRQ (EL1t)
  0x100 - FIQ (EL1t)
  0x180 - SError (EL1t)
  0x200 - Synchronous (EL1h)  ← System Calls
  0x280 - IRQ (EL1h)           ← Timer & Devices
  0x300 - FIQ (EL1h)
  0x380 - SError (EL1h)
  0x400 - Synchronous (EL0)    ← User Exceptions
  0x480 - IRQ (EL0)
  0x500 - FIQ (EL0)
  0x580 - SError (EL0)
```

### Context Switch Flow

```c
1. Timer interrupt fires (100Hz)
2. CPU saves user context to stack (pt_regs)
3. Exception handler calls timer_handler()
4. timer_handler() calls schedule(pt_regs *)
5. schedule() picks next process (round-robin)
6. Switch TTBR0_EL1 to new process page table
7. Invalidate TLB (tlbi vmalle1is)
8. Return new process's pt_regs
9. Exception return restores registers
10. Process resumes execution
```

### ELF Loading

The ELF loader supports position-independent code and dynamically maps segments:

```c
For each PT_LOAD segment:
  1. Allocate virtual pages
  2. Map pages in process page table
  3. Set permissions (R/W/X)
  4. Copy data from filesystem
  5. Zero BSS section
  6. Flush instruction cache
```

### Zero-Copy Buffer Cache

The buffer cache uses a **hash table + LRU list** for efficient block caching:

```c
Cache Miss Flow:
  1. Allocate physical page
  2. DMA read directly into page
  3. Insert into hash table (O(1) lookup)
  4. Add to LRU list (eviction policy)
  5. Return page pointer (zero-copy!)
```

---

## 🗺️ Roadmap

### Version 0.2.0 (Q2 2026)
- [ ] SMP support (multi-core scheduling)
- [ ] VirtIO-Net driver (networking)
- [ ] TCP/IP stack (lwIP integration)
- [ ] Unix sockets (IPC)

### Version 0.3.0 (Q3 2026)
- [ ] Ext4 write support
- [ ] Journaling filesystem
- [ ] Virtual File System (VFS) layer
- [ ] procfs and sysfs

### Version 0.4.0 (Q4 2026)
- [ ] USB stack (XHCI/EHCI)
- [ ] Real hardware support (Raspberry Pi 4)
- [ ] Device Tree parsing
- [ ] Dynamic module loading

### Future
- [ ] Audio subsystem (ALSA-compatible)
- [ ] X11 protocol support
- [ ] POSIX compliance
- [ ] GCC/Clang self-hosting

---

## 🤝 Contributing

Contributions are welcome! Whether you're fixing bugs, adding features, or improving documentation, your help is appreciated.

### How to Contribute

1. **Fork** the repository
2. **Create** a feature branch (`git checkout -b feature/amazing-feature`)
3. **Commit** your changes (`git commit -m 'Add amazing feature'`)
4. **Push** to the branch (`git push origin feature/amazing-feature`)
5. **Open** a Pull Request

### Development Guidelines

- Follow the existing code style (K&R with 2-space indents)
- Add comments for complex algorithms
- Update documentation for new features
- Test on QEMU before submitting
- Keep commits atomic and well-described

### Areas for Contribution

- 🐛 **Bug Fixes** - Found a bug? Submit a patch!
- 📚 **Documentation** - Improve README, add tutorials
- ✨ **Features** - Implement items from the roadmap
- 🧪 **Testing** - Add test cases, improve coverage
- 🎨 **Graphics** - Better fonts, themes, icons
- 🔧 **Drivers** - Support more devices

---

## 📄 License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.

```
MIT License

Copyright (c) 2026 olmox001

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
```

---

## 🙏 Acknowledgments

- **ARM Holdings** - AArch64 Architecture Reference Manual
- **OSDev Wiki** - Invaluable resource for OS development
- **QEMU Project** - Excellent emulation platform
- **Linux Kernel** - Design inspiration (especially for list.h)
- **VirtIO Specification** - Clean device abstraction

### Resources Used

- [ARM Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest)
- [OSDev Wiki](https://wiki.osdev.org/)
- [Ext4 Disk Layout](https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout)
- [VirtIO 1.1 Specification](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)

---

## 📧 Contact

**Project Maintainer**: olmox001

- GitHub: [@olmox001](https://github.com/olmox001)
- Issues: [Report Bug](https://github.com/olmox001/os1test-dev/issues)
- Discussions: [Join Discussion](https://github.com/olmox001/os1test-dev/discussions)

---

<div align="center">

**⭐ Star this repository if you find it useful!**

Made with ❤️ and lots of ☕ by the OS1TEST-DEV team

</div>

