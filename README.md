# OS1 / NEXS

### A from-scratch graphical microkernel operating system supporting both AArch64 and x86-64, with SMP, VirtIO devices, Ext4, composited windows, ELF user-space processes and multiple interactive applications.

[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](LICENSE.md)
[![Arch](https://img.shields.io/badge/arch-aarch64%20%7C%20amd64-green.svg)](#)
[![Platform](https://img.shields.io/badge/platform-QEMU-orange.svg)](https://www.qemu.org/)
[![Language](https://img.shields.io/badge/language-C99-yellow.svg)](https://en.wikipedia.org/wiki/C99)

OS1 (codename **NEXS**) is a from-scratch operating system that boots on both ARM64 and
x86-64 under QEMU, brings up SMP, drives **VirtIO** GPU/input/block devices, mounts an
**Ext4** root, composites overlapping windows, and runs user-space ELF programs including
an interactive **TTY shell**.

> **Honesty note.** This README describes the *verified* state. For the complete,
> evidence-based picture — including bugs, gaps, and severity — see
> [`docs/review/REVIEW.md`](docs/review/REVIEW.md). For where the project is *going*
> (a seL4-style, Plan 9-inspired microkernel), see
> [`docs/PROJECT_CHARTER.md`](docs/PROJECT_CHARTER.md). This project is **GPL v2** (see
> [`LICENSE.md`](LICENSE.md)); earlier docs mislabeled it MIT.

---
<img width="373" height="670" alt="Screenshot 2026-06-07 alle 08 32 59" src="https://github.com/user-attachments/assets/2a948015-c28a-42e1-9dc3-1906bb170fe2" />


---

## Status (verified by building & running, 2026-06-11)

| Capability | AArch64 (`make run`) | amd64 (`make run`) |
|---|---|---|
| Builds clean (`-Werror -Wall -Wextra -Wpedantic -Wshadow`) | ✅ | ✅ |
| Boots to a **TTY shell in a composited window** | ✅ | ✅ |
| Dynamic RAM detection / MMU / **maps up to 4GB** | ✅  |  ✅\*   |
| **SMP** (multi-core bring-up) | ✅ (4/4 online) |  ✅ |
| VirtIO GPU / input / block · Ext4 mount · GPT+MBR | ✅ | ✅ |
| Userland: ELF loader, IPC, windows, registry, fonts | ✅ | ✅ |
| **Fault isolation**: user crash never kills the kernel; symbolized backtraces | ✅ | ✅ |

\* On amd64 the `make run` (`-kernel`) path does not receive a boot-protocol memory map and
falls back to a hardcoded 1 GB; the full memory map / 4 GB works via the **GRUB-ISO**
(`make release`) path. This is a known defect — see the epic
[#94 (amd64 boot/4GB)](https://github.com/olmox001/os1test-dev/issues/94).
**AArch64 is the reference "correct" platform.**

---

## Features (what actually runs)

**Kernel**
- Physical memory manager: per-zone (DMA ≤16 MB / Normal) bitmap allocator, dynamic RAM discovery.
- Virtual memory: two-phase MMU bring-up (bootstrap → RAM-aware remap), per-process page tables, 2 MB block mappings.
- Preemptive **SMP scheduler**: per-CPU O(1) priority run-queues with work-stealing; ELF64 loader.
- Message-passing **IPC**; a fixed-size system **registry**; kernel heap (`kmalloc`).
- **VirtIO** drivers: GPU (framebuffer), input (keyboard/mouse), block.
- Per-arch: GICv2 + ARM generic timer + PL011 (AArch64); LAPIC/IOAPIC + PIT + 16550 (amd64).
- **Ext4** (read + limited write), **GPT** with **MBR** fallback, buffer cache.
- **Fault/trace foundation** (2026-06): fault-safe reporting on dedicated fault stacks, total
  vector coverage on both arches, user/kernel fault isolation (a crashing app terminates
  cleanly, the kernel and shell survive), symbolized in-kernel backtraces (`.ksyms`).
- Leak-free process lifecycle: zombies auto-reaped by the scheduler, single-owner corpse
  freeing, deterministic service respawn by `init`.

**Graphics & userland**
- Window **compositor** (overlap, Z-order, drag, focus), TTF font rendering, 2D/3D fixed-point engines.
- Userland: `init`, `shell` (TTY), `notify_srv`, `regedit`, `fontman`, plus demo apps and a DOOM port.

> Many of these (compositor, fonts, VFS, registry) currently live **in the kernel**; the
> roadmap moves them into isolated userland services (see the charter).

## Highlights

- ✅ Native support for AMD64 and AArch64
- ✅ SMP (4+ cores)
- ✅ User-space ELF64 applications
- ✅ Ext4 filesystem
- ✅ GPT partition support
- ✅ VirtIO GPU / Input / Block
- ✅ Graphical compositor and window manager
- ✅ Multi-window desktop
- ✅ TTY shell
- ✅ Doom running as a user-space process
- ✅ 3D rendering demo
- ✅ Recoverable fault handling (user faults isolated, symbolized backtraces)
- ✅ Microkernel architecture

## Verified Runtime

Successfully tested on:

| Platform | Status |
|-----------|---------|
| QEMU AArch64 virt | ✅ |
| QEMU AMD64 q35 | ✅ |
| SMP (4 cores) | ✅ |
| VirtIO GPU | ✅ |
| VirtIO Keyboard | ✅ |
| VirtIO Mouse | ✅ |
| VirtIO Block | ✅ |
| GPT | ✅ |
| Ext4 | ✅ |

---

## Quick start

### 1. Toolchain (bare-metal cross compilers + QEMU) 
**NOTE** Compilation tested only on macOS Intel

**macOS (Homebrew):**
```bash
brew install aarch64-elf-gcc x86_64-elf-gcc qemu make
# (optional, for bootable ISOs) i686-elf-grub / grub
```
**Debian/Ubuntu:** install `gcc-aarch64-none-elf` (or build a cross-gcc), an `x86_64-elf`
cross toolchain, `qemu-system-arm`, `qemu-system-x86`, `make`, `binutils`.

Verify:
```bash
make check ARCH=aarch64
make check ARCH=amd64
```

### 2. Build & run
```bash
make run ARCH=aarch64     # ARM64 — full path (4GB, SMP, graphical TTY)
make run ARCH=amd64       # x86-64 — same graphical shell (1 GB map on the -kernel path, see #94)
```
`make run` builds the bootloader, kernel, userland, and a 96 MB Ext4 disk image, then
launches QEMU with VirtIO GPU/input/block and a graphical display. A window with a TTY
shell (`shell:/>`) appears once boot completes.

### 3. Other targets
```bash
make all ARCH=<arch>      # build only (no run)
make debug ARCH=<arch>    # run under QEMU with gdb stub (-s -S)
make release VERSION=x.y  # build distributable images (amd64 = bootable hybrid ISO via GRUB)
make clean
```

---

## Project layout (actual)

```
boot/{aarch64,amd64}/        # stage1/stage2 bootloaders + linker scripts
kernel/
  main.c                     # kernel_main orchestration (both arches)
  arch/{aarch64,amd64}/      # cpu, mmu, platform, hal, virtio, boot asm, syscall entry
  core/                      # hal_bus (device registry), syscall_dispatch, timer
  mm/                        # pmm.c, vmm.c, buffer.c   (+ lib/kmalloc.c)
  sched/                     # process.c (scheduler+IPC), elf.c (loader)
  fs/                        # vfs.c (path resolution), ext4.c, gpt.c
  graphics/                  # graphics.c, compositor.c, gl.c, font.c, region.c
  drivers/                   # virtio/, gpu/, uart/, gic/, timer/, keyboard/, pci/
  irq/, lib/                 # irq.c; string/printk/vsnprintf/crc32/math/registry/fdt/...
  include/                   # kernel + arch + drivers + graphics headers
include/api/                 # public userland ABI (os1.h, libc-ish headers; stb_* vendored)
user/
  sys/{bin,lib}/             # init, shell, notify_srv, regedit, fontman; lib.c, malloc.c
  bin/                       # counter, demo3d, ipc_*, input_test, writetest, doom/
  arch/{aarch64,amd64}/      # userland syscall stubs
tools/mkdisk.c               # builds the Ext4 disk image / rootfs
docs/                        # review/ (this audit), PROJECT_CHARTER.md, report/, screen/
```

---

## Documentation

- **[`docs/review/REVIEW.md`](docs/review/REVIEW.md)** — full code review: ~220 findings on a
  W0–W5 × kind taxonomy, with the verified runtime baseline and per-subsystem analyses
  ([`docs/review/analysis/`](docs/review/analysis/)).
- **[`docs/MANUAL.md`](docs/MANUAL.md)** — build/run, architecture, boot flow, memory model,
  syscall/ABI reference, drivers, filesystem, and how to add an app/driver/syscall.
- **[`docs/PROJECT_CHARTER.md`](docs/PROJECT_CHARTER.md)** — purpose, principles, and the
  seL4/Plan 9 target architecture.
- **Issues** — the actionable (W3+) findings are tracked as GitHub issues (labels
  `code-review`, `w3`/`w4`/`w5`, `area:*`); see the
  [tracking epic #19](https://github.com/olmox001/os1test-dev/issues/19).

## Roadmap (foundations, dependency-ordered)

> **Phase A (fault/trace foundation) is complete** — see
> [`docs/review/REVIEW.md`](docs/review/REVIEW.md) §8 for the commit catalog. The next
> phase (STUB demolition, epics #92–#96) is planned in
> [`docs/PHASE-B-PLAN.md`](docs/PHASE-B-PLAN.md); the architectural guidelines it follows
> are in [`docs/ASTRA.md`](docs/ASTRA.md).

1. A documented PA/VA model and **W^X**.
2. A coherent, **capability-checked** syscall ABI.
3. Real allocators (buddy PMM + growable slab).
4. Stable boot on both arches via a GPLv2-compatible loader; real-memory map on amd64.
5. A thin (Plan 9-style) HAL; fuller drivers + device tree.
6. **Service isolation** (seL4): move VFS/compositor/fonts to sandboxed userland.
7. Plan 9 file-namespace registry; then networking and real hardware.

## Contributing

Issues and PRs welcome. Style: K&R, 2-space indent; keep the strict warning set clean
(`-Werror -Wall -Wextra -Wpedantic -Wshadow`); test on QEMU (`make run`) before submitting.
Pick a `code-review` issue to start.

## License

**GNU General Public License v2** — see [`LICENSE.md`](LICENSE.md). Any third-party code
integrated (boot loader, libraries, drivers) must be GPLv2-compatible.

## Acknowledgments

ARM & Intel architecture references, the OSDev wiki, the QEMU project, the VirtIO
specification, and the Linux kernel (design inspiration, e.g. intrusive lists).
