# OS1 / NEXS — Developer & User Manual

> Companion to [`review/REVIEW.md`](review/REVIEW.md) (current defects) and
> [`PROJECT_CHARTER.md`](PROJECT_CHARTER.md) (target architecture). This manual documents
> the system **as it is today**. Where behaviour is a known defect, it links the relevant
> finding ID. AArch64 is the reference platform; amd64 differences are called out inline.

## Table of contents
1. [Overview](#1-overview)
2. [Building](#2-building)
3. [Running & debugging](#3-running--debugging)
4. [Boot flow](#4-boot-flow)
5. [Memory model](#5-memory-model)
6. [Processes, scheduling & IPC](#6-processes-scheduling--ipc)
7. [Syscall ABI reference](#7-syscall-abi-reference)
8. [Drivers & the HAL](#8-drivers--the-hal)
9. [Filesystem & disk image](#9-filesystem--disk-image)
10. [Graphics & windowing](#10-graphics--windowing)
11. [Userland](#11-userland)
12. [How to extend](#12-how-to-extend)
13. [Logging & diagnostics](#13-logging--diagnostics)
14. [Known limitations](#14-known-limitations)

---

## 1. Overview

OS1/NEXS is a dual-arch (AArch64 + x86-64) kernel plus a small graphical userland. The
kernel today is monolithic-leaning: alongside scheduling/memory/IPC it also hosts the VFS,
filesystem, graphics compositor and font engine. The userland runs ELF programs (shell,
services, apps) that talk to the kernel through a syscall ABI (`include/api/os1.h`).

Reference platform: QEMU `virt` (AArch64, Cortex-A57) and QEMU `q35`-class (x86-64) with
VirtIO devices. Real hardware is not yet supported.

## 2. Building

Toolchains (bare-metal cross compilers) and QEMU are required:

| ARCH | CC prefix | QEMU |
|---|---|---|
| `aarch64` | `aarch64-none-elf-` | `qemu-system-aarch64` |
| `amd64` | `x86_64-elf-` | `qemu-system-x86_64` |

```bash
make check ARCH=aarch64      # verify toolchain presence
make all   ARCH=aarch64      # build bootloader + kernel + userland + disk image
```
The build uses a strict warning set (`-Wall -Wextra -Werror -Wpedantic -Wshadow
-Wmissing-prototypes …`, `-ffreestanding -nostdlib -O2 -g`). Output lands in
`build/<arch>/` (kernel ELF/bin, userland ELFs, `disk.img`).

Key make targets: `all`, `run`, `debug`, `release`, `test-release`, `clean`, `check`, `help`.
`ARCH` defaults to `aarch64`. `VERSION` controls `release` output naming.

> Note: `build/` is an artifact directory; some environments clean it between steps. Rebuild
> with `make all` if `build/<arch>/disk.img` is missing.

## 3. Running & debugging

```bash
make run ARCH=aarch64        # graphical; window with TTY shell appears after boot
make run ARCH=amd64          # boots to shell at the default -m 3G
make debug ARCH=<arch>       # same, plus QEMU gdb stub (-s -S): connect with gdb, target remote :1234
```

Headless serial capture (useful for CI / boot logs), AArch64 example:
```bash
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 4G -smp 4 -display none \
  -serial file:/tmp/serial.log \
  -device virtio-gpu-device -device virtio-keyboard-device -device virtio-mouse-device \
  -drive if=none,file=build/aarch64/disk.img,id=hd0,format=raw -device virtio-blk-device,drive=hd0 \
  -kernel build/aarch64/kernel.elf
```

**amd64 caveat:** the `-kernel` path (what `make run` uses) does not deliver a boot-protocol
memory map; the platform falls back to **1 GB** and **crashes at `-m 4G`**. Use `-m 3G` (the
default), or the GRUB-ISO path (`make release ARCH=amd64`) for the full memory map. See
`REVIEW.md` BOOT-01/02 and DRV-VIRTIO-01.

## 4. Boot flow

Entry: `kernel_main` (`kernel/main.c`). AArch64 receives the DTB pointer in `x0`; amd64
receives the (multiboot/PVH) magic+info in registers, saved by `arch/amd64/boot/start.S`.

```
driver_console_init            (UART up for early logs)
fdt_init                       (AArch64: parse DTB; amd64: stubbed)
print_banner
ktest_run_all                  (boot-time unit tests; see §13 / LIB-KTEST-01)
cpu_init                       (exception vectors / IDT-GDT, per-CPU data)
arch_platform_early_init       (IRQ controller registration, mem/CPU discovery)
driver_irq_init / irq_init     (GIC or PIC/APIC)
driver_timer_init              (generic timer or LAPIC/PIT @ 100 Hz)
init_memory():
    pmm_early_init / pmm_init   (zones, reserve kernel+metadata)
    vmm_init                    (bootstrap MMU, map 128 MB)
    vmm_dynamic_remap           (map all detected RAM)
    hal_bus_init                (PCI/MMIO scan -> device registry; virtio discovery)
    virtio_blk_init / virtio_gpu_init / graphics_init
    gpt_init / buffer_init / ext4_init / keyboard_init / registry_init
process_init / init_scheduler  (compositor_init; spawn /sys/bin/init as PID 1)
arch_smp_init                  (wake secondary CPUs)
local_irq_enable               (then idle loop)
```

`init` (PID 1) spawns `notify_srv` and `shell`; the compositor creates the shell's window.

## 5. Memory model

**Physical (PMM, `kernel/mm/pmm.c`).** Two zones: DMA (≤16 MB) and Normal, each a bitmap
over a `struct page` array placed in early RAM. `pmm_alloc_page` (next-fit) /
`pmm_alloc_pages` (contiguous) / `pmm_alloc_aligned`; `pmm_free_page` poisons freed pages.
Frees and double-frees are checked.

**Virtual (VMM, `kernel/mm/vmm.c` + `arch/*/mm/mmu.c`).** 4-level tables (identical index
math on both arches). Two-phase: `vmm_init` maps a 128 MB bootstrap window and enables the
MMU; `vmm_dynamic_remap` rebuilds the map for all discovered RAM using 2 MB blocks. Per
process: `vmm_create_pgd` clones the kernel half by reference; user space is the low half.

> **Central invariant:** the kernel currently runs **identity-mapped** (kernel VA == PA for
> the RAM window) and maps **all RAM executable** (no W^X). This is the root assumption
> behind several findings (REVIEW: MM-VMM-01/02, AMMU-01/02) and the first thing the
> refactor addresses.

**Kernel heap (`kernel/lib/kmalloc.c`).** Power-of-two buckets (16 B–4 KB) over a 32 MB
pool; larger requests go straight to the PMM. Note: freed memory is not returned to the PMM
and the pool does not grow (MM-KM-01).

**User heap.** `sys_sbrk` grows/shrinks a per-process heap; `user/sys/lib/malloc.c` is a
first-fit allocator with forward coalescing on top of `sbrk`.

## 6. Processes, scheduling & IPC

`kernel/sched/process.c`. Fixed pool of `MAX_PROCESSES` (64). Each `struct process` holds
its PID (monotonic, never reused), page table, kernel stack, saved `pt_regs` context, CWD,
priority, and an IPC message queue.

- **Scheduler:** per-CPU priority run-queues with an `O(1)` bitmap pick and **work-stealing**
  across CPUs; 100 Hz preemption. (It also consults the compositor for focus-based boosting —
  a coupling the refactor removes: SCHED-01.)
- **Lifecycle:** `process_create` → `process_load_elf` → `enqueue_task`; `process_terminate`
  (zombie → reaped by `process_wait`, which is **non-blocking**); deferred free in `schedule`.
- **ELF loading (`elf.c`):** maps each `PT_LOAD` segment, a 1 MB stack at `0xC0000000`, sets
  the entry point. (No `p_vaddr` validation yet — ELF-01.)
- **IPC:** `kernel_ipc_send` copies a message into the target's queue and wakes it;
  `sys_ipc_recv` pops or sleeps. (Has a lost-wakeup race and unbounded queue — IPC-01,
  SCHED-05.)

## 7. Syscall ABI reference

Userland calls go through `user/arch/<arch>/syscall.S` (SVC on AArch64 in `x8`+`x0..x5`;
`SYSCALL` on amd64 in `rax`+`rdi,rsi,rdx,r10,r8,r9`) into
`kernel/core/syscall_dispatch.c`. Numbers and wrappers are declared in `include/api/os1.h`.

| # | Name | Wrapper | Notes |
|---|---|---|---|
| 63 | READ | `read(fd,buf,n)` | fd 0 = stdin (keyboard IPC); blocks |
| 64 | WRITE | `write(fd,buf,n)` | fd 1/2 → focused window; fd≥100 → window id; **truncates >1023 B**, also echoes UART |
| 93 | EXIT | `exit(status)` | |
| 169 | GET_TIME | `get_time()` | ms (from a stubbed timer on amd64) |
| 172 | GETPID | `get_pid()` | |
| 200 | DRAW | `draw(x,y,w,h,color)` | raw framebuffer rect |
| 201 | FLUSH | `flush()` | compositor render |
| 210 | CREATE_WINDOW | `create_window(x,y,w,h,title)` | |
| 211–215 | WINDOW_DRAW/RENDER/BLIT/SET_FLAGS/DESTROY | `window_*` | |
| 216 | SBRK | `sbrk(incr)` | heap grow/shrink |
| 220 | SPAWN | `spawn(path)` | loads+runs an ELF |
| 221 | KILL | `kill_process(pid)` | **no permission check** (ABI-04) |
| 222 | GETPROCS | `get_procs(buf,n)` | `struct ps_info[]` |
| 223 | YIELD | `yield()` | |
| 230/231 | SEND/RECV | `send/recv(pid,msg)` | also aliased at 30/31/32 (ABI-01) |
| 232 | SET_FOCUS | `set_focus(pid)` | |
| 247 | WAIT | `wait(pid)` | non-blocking: pid if dead, -1 if alive |
| 250 | REGISTRY | `registry_read/write` | flat K/V store |
| 251/252 | FILE_WRITE/READ | `file_write/read(path,...)` | ext4; write capped 48 KB |
| 253 | SET_FONT | `set_font(data,size)` | **passes a raw user pointer to the kernel** (GFX-FONT-01) |
| 254 | LIST_DIR | `list_dir(path,buf,size)` | |
| 255/256 | CHDIR/GETCWD | `chdir/getcwd` | |

> The ABI is **not yet coherent or capability-checked** (mixed numbering, no `errno`, no fd
> table, no access control). Treat it as evolving; see epics #93 (ABI) / REVIEW ABI-01..04.

## 8. Drivers & the HAL

`kernel/core/hal_bus.c` is a thin **device registry**: `arch_bus_scan` (PCI on amd64, MMIO
probe on AArch64) + `arch_virtio_scan` populate `hal_device[]`; drivers find devices via
`hal_device_find(vendor, device, idx)`. (The register-access macros in `hal.h` are heavier
than the registry and are slated to be thinned — HAL-01.)

Drivers: VirtIO **block** (`drivers/virtio/virtio_blk.c`), **input**
(`virtio_input.c`), **GPU** (`drivers/gpu/`), plus per-arch UART (`pl011`/`16550`),
interrupt controller (`gic`/`apic`), timer, and PS/2-style keyboard mapping. Interrupts are
routed via `kernel/irq/irq.c` (+ arch dispatch). Real-hardware and many robustness items are
open (see `area:drivers` issues).

## 9. Filesystem & disk image

- **Partitioning (`fs/gpt.c`):** GPT parser with a legacy **MBR** fallback (for hybrid ISOs);
  CRC32 verified for the header.
- **Ext4 (`fs/ext4.c`):** reads the superblock, group descriptor, inodes and directory
  entries; supports direct + single/double-indirect block maps. **Write** is implemented but
  capped at 48 KB (12 direct blocks). It does **not** yet handle extent-format inodes, so it
  only mounts images built by `tools/mkdisk.c` (block-mapped), not standard `mkfs.ext4 -O
  extent` images (EXT4-01).
- **VFS (`fs/vfs.c`):** currently only path normalisation; syscalls call `ext4_*` directly.
  A real vnode/mount layer is the target (VFS-01).
- **Disk image:** `tools/mkdisk.c` builds a 96 MB image: bootloader + kernel + a GPT/MBR
  Ext4 partition populated from `build/<arch>/rootfs/` (`/sys/bin`, `/bin`, `/etc`, `/fonts`,
  and DOOM WADs). `make rootfs` stages it.

## 10. Graphics & windowing

`kernel/graphics/`. `graphics_init` brings up the VirtIO-GPU framebuffer (720×1280 today).
`compositor.c` manages overlapping windows (Z-order, drag, focus, damage tracking, TTY
windows for shells). `font.c` renders TTF glyphs (uploaded by `fontman` via `set_font`);
`gl.c` provides 2D/3D fixed-point primitives. The compositor and font engine are in-kernel
today and are prime candidates for extraction into userland services (epic #95).

## 11. Userland

- **`init` (`user/sys/bin/init.c`):** spawns `notify_srv` + `shell`, then a supervisor poll
  loop that respawns them. (Hardcoded list; `init.cfg` is not yet read — USR-INIT-02.)
- **`shell`:** line editor with built-ins (ls/cat/cd/ps/kill/spawn/…); opens a TTY window.
- **Services:** `notify_srv` (notification popups via IPC), `regedit` (registry UI),
  `fontman` (TTF rasteriser/uploader).
- **Library (`user/sys/lib/`):** `lib.c` (libc-ish + formatting + vendored stb), `malloc.c`,
  `font_lib.c`, arch syscall stubs. Everything is statically linked into each ELF (binaries
  are large; USR-BLOAT-01/02).

## 12. How to extend

**Add a userland app**
1. Create `user/bin/myapp.c` with `int main(void)` and `#include <os1.h>`.
2. In the `Makefile`: add `$(BUILD_DIR)/myapp.elf` to `BIN_ELFS` and an explicit link rule
   (mirror `counter.elf`): object + `$(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)`.
3. `make run` — `mkdisk` copies it into `/bin`; launch from the shell (`spawn /bin/myapp`).

**Add a syscall**
1. Pick an unused number; `#define SYS_FOO` + declare the wrapper in `include/api/os1.h`.
2. Add the wrapper in `user/arch/<arch>/syscall.S` (both arches) and a convenience fn in
   `lib.c`.
3. Handle the case in `kernel/core/syscall_dispatch.c`, using `arch_copy_*_user` for any
   user pointers. (When the coherent-ABI work lands, register it in the unified table.)

**Add a driver**
1. Implement under `kernel/drivers/<class>/`; discover via `hal_device_find` after
   `hal_bus_init`.
2. Register an IRQ handler with `irq_register`; add the source file to the `Makefile`
   `KERN_C_SOURCES`.

## 13. Logging & diagnostics

- `printk` with levels (`pr_info`/`pr_warn`/`pr_err`/`pr_debug`); `console_loglevel`
  gates output. All output also goes to the serial console (`-serial`).
- Boot-time unit tests: `ktest_run_all()` runs cases from the `.ktests` section. (Note: the
  runner currently always reports PASS — LIB-KTEST-01 — so treat it as a smoke test only.)
- `make debug` exposes a gdb stub: `gdb build/<arch>/kernel.elf` then `target remote :1234`.
- `panic()` prints a register/stack dump and (on amd64) halts the CPU.

## 14. Known limitations

The authoritative, severity-ranked list is [`review/REVIEW.md`](review/REVIEW.md) and the
GitHub issues (`code-review` label). Highlights:

- **amd64 `make run`**: 1 GB only; crashes at `-m 4G` (use 3G or the ISO path). *(W5/W4)*
- **No W^X**; identity-mapped kernel; user-frame leak on exit. *(W3)*
- **No capability checks**: any process can kill/overwrite anything. *(W4)*
- **VFS** is a stub; **Ext4** can't read standard (extent) images; write capped at 48 KB.
- **set_font** hands a raw user pointer to the kernel (UAF risk). *(W4)*
- Several **SMP data races** (uaccess TOCTOU, lock-free shared state, no TLB shootdown).
- Allocators don't reclaim; userland binaries are large.

These are the subject of the planned refactor (see `PROJECT_CHARTER.md` and the epics).

---

*License: GPL v2 ([`../LICENSE.md`](../LICENSE.md)).*
