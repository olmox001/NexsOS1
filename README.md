# OS1 / NEXS

### Multi-architecture open-source hybrid-kernel operating system

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
> (a seL4-style, Plan 9-inspired HybridKernel MicroKernel ispired), see
> [`docs/PROJECT_CHARTER.md`](docs/PROJECT_CHARTER.md). This project is **GPL v2** (see
> [`LICENSE.md`](LICENSE.md)); earlier docs mislabeled it MIT.

---
<img width="373" height="670" alt="Screenshot 2026-06-07 alle 08 32 59" src="https://github.com/user-attachments/assets/2a948015-c28a-42e1-9dc3-1906bb170fe2" />


---

## Status (verified by building & running, 2026-06-12)

| Capability | AArch64 (`make run`) | amd64 (`make run`) |
|---|---|---|
| Builds clean (`-Werror -Wall -Wextra -Wpedantic -Wshadow`) | ✅ | ✅ |
| Boots to a **TTY shell in a composited window** | ✅ | ✅ |
| **Higher-half kernel** (PA/VA contract, direct map, **W^X**) | ✅ `0xFFFF0000...` | ✅ `0xFFFF8000...` |
| Dynamic RAM detection / full boot-protocol memory map | ✅ (DTB) | ✅\* (PVH/MB1/MB2) |
| **SMP** (multi-core bring-up) | ✅ (4/4 online) |  ✅ |
| VirtIO GPU / input / block · **Ext4 (extents)** mount · GPT+MBR | ✅ | ✅ |
| Userland: ELF loader, IPC, windows, registry, fonts | ✅ | ✅ |
| **Fault isolation**: user crash never kills the kernel; symbolized backtraces | ✅ | ✅ |
| Coherent syscall ABI (single numbering, negative `errno`) + first **capability checks** | ✅ | ✅ |

\* The amd64 `-kernel` path now parses the real PVH memory map (the old "1 GB fallback"
is fixed). One accounting defect remains: total RAM is derived from the highest region
end address, so the 3–4 GB PCI hole is counted (and treated) as RAM — see the epic
[#94 (amd64 boot parity)](https://github.com/olmox001/NexsOS1/issues/94).
**AArch64 is the reference "correct" platform.**

---

## Features (what actually runs)

**Kernel**
- Physical memory manager: per-zone (DMA ≤16 MB / Normal) bitmap allocator, dynamic RAM discovery.
- **Higher-half virtual memory** (2026-06): kernel image + direct map at `KERNEL_VIRT_BASE`
  (`0xFFFF000000000000` aarch64 via TTBR1, `0xFFFF800000000000` amd64), documented PA/VA
  contract (`phys_to_virt`/`virt_to_phys`), pure-user low half, **W^X enforced** (text RX,
  rodata RO+NX, everything else RW+NX), cross-CPU TLB shootdown.
- Preemptive **SMP scheduler**: per-CPU O(1) priority run-queues with work-stealing; ELF64
  loader; leak-free, race-free process teardown.
- Message-passing **IPC**; a system **registry** with per-key write ownership; growable
  kernel heap (`kmalloc`).
- **Coherent syscall ABI** (2026-06): single numbering (`include/api/syscall_nums.h`
  compiled into both the kernel dispatcher and the userland stubs), negative-`errno`
  returns, first **capability layer** (kill/focus/window/file-write/registry checks).
- **VirtIO** drivers: GPU (framebuffer), input (keyboard/mouse), block.
- Per-arch: GICv2 + ARM generic timer + PL011 (AArch64); LAPIC/IOAPIC + PIT + 16550 (amd64).
- **VFS** (mount table + `fs_ops` providers) over **Ext4** (extent-tree + legacy reads,
  INCOMPAT enforcement, extended write paths), **GPT** with **MBR** fallback, buffer cache.
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
- ✅ HybridKernel MicroKernel ispired architecture

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
make run ARCH=aarch64     # ARM64 — full path (SMP, graphical TTY)
make run ARCH=amd64       # x86-64 — same graphical shell (PVH memory map, see #94 for parity gaps)
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
  [tracking epic #19](https://github.com/olmox001/NexsOS1/issues/19).

## Roadmap (foundations, dependency-ordered)

> **Phase A (fault/trace foundation) is complete**; Phase B is under way — **B1
> (VFS/ext4-extents) and B2 (memory: W^X, teardown, allocators, higher-half — epic #92)
> are done**, **B3 (coherent ABI + capabilities, epic #93) is in progress**. See
> [`docs/review/REVIEW.md`](docs/review/REVIEW.md) §8 for the commit catalog and
> [`docs/PHASE-B-PLAN.md`](docs/PHASE-B-PLAN.md) for the plan; the architectural
> guidelines are in [`docs/ASTRA.md`](docs/ASTRA.md).

1. ~~A documented PA/VA model and **W^X**~~ — **done** (higher-half kernel, epic #92).
2. A coherent, **capability-checked** syscall ABI — **in progress** (numbering + errno +
   first capability layer landed; fd table, formal IPC and sandboxing remain — epic #93).
3. Real allocators (buddy PMM + growable slab) — kmalloc now grows; buddy PMM pending.
4. Stable boot on both arches via a GPLv2-compatible loader; amd64 parity (epic #94).
5. A thin (Plan 9-style) HAL; fuller drivers + device tree (epic #95).
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
specification, and the Linux/plan9/sel4kernel (design inspiration, e.g. intrusive lists).
