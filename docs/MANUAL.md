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
make run ARCH=amd64          # boots to shell (default -m 5G)
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

**amd64 caveats:** the `-kernel` path (what `make run` uses) boots via **PVH** and now
parses the real memory map (the old "1 GB fallback", BOOT-01/02, is fixed). Two things
remain: (a) virtio-pci devices **must** be declared with `disable-legacy=on` — the legacy
transport hangs at the first block read; (b) total-RAM accounting derives `total_pages`
from the highest region end address, so with `-m 5G` the log reports 6144 MB and the
3–4 GB PCI hole is treated as allocatable RAM (epic #94).

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
math on both arches). Two-phase: `vmm_init` maps a bootstrap window; `vmm_dynamic_remap`
rebuilds the map for all discovered RAM using 2 MB blocks. Per process the page tables are
**pure-user**: on aarch64 the kernel half lives permanently in **TTBR1** and process PGDs
(TTBR0) contain zero kernel entries; on amd64 process PML4s copy only the high slots
(256..511, pre-populated shared PDPTs).

> **Central invariant (since 2026-06-12):** the kernel runs **higher-half** with a uniform
> direct map: `VA = PA + KERNEL_VIRT_BASE` (`0xFFFF000000000000` aarch64,
> `0xFFFF800000000000` amd64) for the image, all RAM and MMIO. Every PA↔pointer crossing
> goes through `phys_to_virt`/`virt_to_phys` (`kernel/include/kernel/memlayout.h` is the
> single flip point). **W^X is enforced**: text RX, rodata RO+NX, all other RAM RW+NX,
> user stack/heap never executable. User space owns the low half exclusively.

**Kernel heap (`kernel/lib/kmalloc.c`).** Power-of-two buckets (16 B–4 KB); the pool starts
at 32 MB and **grows by 4 MB PMM chunks** on exhaustion (MM-KM-01 fixed). Freed memory is
still not returned to the PMM.

**User heap.** `sys_sbrk` grows/shrinks a per-process heap; `user/sys/lib/malloc.c` is a
first-fit allocator with forward coalescing on top of `sbrk`.

## 6. Processes, scheduling & IPC

`kernel/sched/process.c`. Pool array of `MAX_PROCESSES` (128); the effective live limit is
**derived from usable memory** at boot (anti fork-bomb, SCHED-DOS-01). Each `struct process`
holds its PID (monotonic, never reused), page table, kernel stack, saved `pt_regs` context,
CWD, priority, an IPC message queue, a per-process **fd table**, a privilege **level** and a
**capability mask**.

- **Scheduler:** per-CPU priority run-queues with an `O(1)` bitmap pick and **work-stealing**
  across CPUs; 100 Hz preemption. The focused window's process gets a **bounded** priority
  boost (capped per-CPU at `FOCUS_BOOST_MAX`) so the foreground stays responsive but can never
  monopolise a core and starve the rest — opening a CPU-bound focused program no longer freezes
  userspace on a single core (SCHED-01; fully decoupling the scheduler from the compositor is
  Fase 3).
- **Timers & sleep — three-tier model** (`docs/TIMER-MODEL.md`): one real-time reference,
  `mono_ns()`, is derived from the free-running hardware counter (CNTVCT on aarch64, calibrated
  TSC on amd64 — the old `jiffies*1000` stub is gone). **(1) Global clock:** CPU 0 reconciles
  `jiffies` against `mono_ns()` every tick instead of a blind `++`, so dropped ticks (IRQ
  latency, a CPU-bound focused task) are *recovered* and the clock tracks wall time. **(2)
  Per-CPU clock:** each core corrects its own tick cadence against the counter (aarch64
  fractional drift accounting; amd64 at parity). **(3) Per-process clock:** software timers are
  **per-CPU** (each core fires its own `timer_list`, a sleeper is woken on the core that holds
  it — no global lock, no cross-CPU wake), and sleep deadlines are **absolute real time**
  (`wake_ns`), so `sleep()`/`usleep()`/`nanosleep` **block** and wake at the right wall-clock
  instant even under load. Per-process CPU time (`cpu_time_ns`) is accumulated from `mono_ns()`
  deltas. Userland reads time via `os1_mono_ns()`/`os1_cpu_ns()` (os1.h) and POSIX
  `clock_gettime()` (`<time.h>`) built on top; capability timer objects are issue #135.
- **Lifecycle:** `process_create` → `process_load_elf` → `enqueue_task`; `process_terminate`
  (zombie → **auto-reaped by the scheduler**; `process_wait` is a non-blocking pure
  reporter, `-2` = child gone). Teardown frees user frames + private tables (leak-free).
  Each process records its `parent_pid`; `kill` is capability-checked
  (self or any **descendant**; privileged levels kill anything). A dead
  parent's children are reparented to the nearest live ancestor (SCHED-DOS-02).
- **Privilege & capabilities (USR-SEC-03 #79):** one of four levels —
  `machine` (the machine's own identity: not a login user, unkillable,
  bypasses all checks, resolver for future real users) > `root` > `user` >
  `guest` — plus a fine-grained capability mask (`CAP_SPAWN/FS_WRITE/IPC_ANY/
  WINDOW/REG_WRITE`, shared kernel↔userland in `include/api/caps.h`). Spawn is
  monotonic: a child is never more privileged than its creator, never above
  its level's ceiling, never beyond the creator's caps. `spawn_caps`/
  `spawn_level` request a restricted child; plain `spawn` yields a full `user`.
- **ELF loading (`elf.c`):** maps each `PT_LOAD` segment (with a user-window guard on
  `p_vaddr`), a 1 MB stack at `0xC0000000`, sets the entry point. `sbrk` is capped below
  the stack.
- **IPC:** `kernel_ipc_send` copies a message into the target's queue and wakes it;
  `sys_ipc_recv` pops or sleeps (the lost-wakeup race is fixed, IPC-01: recv re-checks the
  queue under `msg_lock` before sleeping). `msg.from` is kernel-stamped (sender auth);
  without `CAP_IPC_ANY` a process may only message its parent or descendants. The queue is
  still unbounded (SCHED-05/per-queue quota → B5/B6). The whole IPC path is **arch-neutral C —
  identical and verified on amd64 and AArch64** (no arch `#ifdef`).

## 7. Syscall ABI reference

Userland calls go through `user/arch/<arch>/syscall.S` (SVC on AArch64 in `x8`+`x0..x5`;
`SYSCALL` on amd64 in `rax`+`rdi,rsi,rdx,r10,r8,r9`) into
`kernel/core/syscall_dispatch.c`. The **single source of numbers** is
`include/api/syscall_nums.h` (#define-only), compiled into both the kernel dispatcher and
the preprocessed userland stubs; wrappers are declared in `include/api/os1.h`. Failures
return **negative errno** (`-EPERM`, `-EFAULT`, …; codes in `include/api/posix_types.h`).

| # | Name | Wrapper | Notes |
|---|---|---|---|
| 56/57/62 | OPEN/CLOSE/LSEEK | `open/close/lseek` | per-process fd table (ABI-03); open-for-write needs CAP_FS_WRITE |
| 63 | READ | `read(fd,buf,n)` | fd 0 = stdin (keyboard IPC); fd≥3 = file at private offset; blocks |
| 64 | WRITE | `write(fd,buf,n)` | fd 1/2 = stdout (the caller's **own** window, by PID — a child does not inherit the spawner's); fd≥3 = file; no truncation, also echoes UART |
| 93 | EXIT | `exit(status)` | |
| 169 | GET_TIME | `get_time()` | ms; real-time on **both** arches (`mono_ns()/1e6`, amd64 stub retired) |
| 257 | NANOSLEEP | `sleep(ms)`/`usleep(us)`/`nanosleep` | **real blocking** sleep (arg0 = ns); per-process timer armed on the running core, **absolute real-time deadline** (`wake_ns`), woken locally — no busy-wait |
| 258 | CLOCK_GETTIME | `os1_mono_ns()`/`os1_cpu_ns()`, `clock_gettime()` | Tier 3 clock; arg0: 0=monotonic ns, 1=process CPU ns; returns ns (`docs/TIMER-MODEL.md`) |
| 172 | GETPID | `get_pid()` | |
| 200 | DRAW | `draw(x,y,w,h,color)` | raw framebuffer rect |
| 201 | FLUSH | `flush()` | compositor render |
| 210 | CREATE_WINDOW | `create_window(x,y,w,h,title)` | |
| 211–215 | WINDOW_DRAW/RENDER/BLIT/SET_FLAGS/DESTROY | `window_*` | DESTROY is **owner-only**; draw/focus need CAP_WINDOW |
| 217 | WINDOW_WRITE | `window_write(id,buf,n)` | write text to a window by id (needs CAP_WINDOW); replaces the old fd≥100 overload |
| 216 | SBRK | `sbrk(incr)` | heap grow/shrink; capped below the user stack |
| 220 | SPAWN | `spawn(path)` | full `user` child; needs CAP_SPAWN; records `parent_pid` |
| 234 | SPAWN_CAPS | `spawn_caps/spawn_level` | restricted spawn (level+caps), monotonically clamped |
| 221 | KILL | `kill_process(pid)` | **capability-checked**: self or any **descendant**; privileged kills anything |
| 222 | GETPROCS | `get_procs(buf,n)` | `struct ps_info[]` |
| 223 | YIELD | `yield()` | |
| 230/231 | SEND/RECV | `send/recv(pid,msg)` | blocking; SEND needs CAP_IPC_ANY for non-relatives; `msg.from` kernel-stamped |
| 232 | SET_FOCUS | `set_focus(pid)` | needs CAP_WINDOW; cross-PID focus needs machine level |
| 233 | TRY_RECV | `try_recv(&msg)` | non-blocking; `-EAGAIN` when empty |
| 247 | WAIT | `wait(pid)` | non-blocking: pid if dead, -1 alive, -2 gone |
| 250 | REGISTRY | `registry_read/write` | K/V store; write needs CAP_REG_WRITE; **first-writer-wins key ownership** |
| 251/252 | FILE_WRITE/READ | `file_write/read(path,...)` | via VFS; write needs CAP_FS_WRITE; `/bin` `/sys` machine-only |
| 253 | SET_FONT | `set_font(data,size)` | **passes a raw user pointer to the kernel** (GFX-FONT-01) |
| 254 | LIST_DIR | `list_dir(path,buf,size)` | |
| 255/256 | CHDIR/GETCWD | `chdir/getcwd` | |

> Epic #93 (B3) is **closed**: single numbering, negative errno, the per-process fd table
> (ABI-03 #90), capability checks at every gated surface with a 4-level privilege model
> (USR-SEC-03 #79), the IPC-01 lost-wakeup fix, and the userland legacy purge + stdout
> inheritance (USR-TTY-01 #123). Still open elsewhere: the modern terminal protocol
> (#123 problem 2, post-B3), per-window/per-IPC quotas (#122 residue, B5), SET_FONT
> (GFX-FONT-01).

## 8. Drivers & the HAL

`kernel/core/hal_bus.c` is a thin **device registry** + **driver-binding** layer:
`arch_bus_scan` (PCI on amd64, ECAM/MMIO on AArch64) + `arch_virtio_scan` populate
`hal_device[]` (each entry carries its PCI **class triplet**); `hal_device_find(vendor,
device, idx)` and `hal_device_find_class(class, subclass, prog_if, idx)` look devices up. A
driver declares a `struct device_driver` (`kernel/include/kernel/driver.h`) matching by
**vendor:device** or by **PCI class**, and `driver_register()` + `driver_match_all()` bind it
to every matching device — this is the "device manager". Discovery is one-shot at boot today;
runtime **hotplug** is Fase 2.

**Arch-neutral PCI (`drivers/pci/pci.c`):** config access is pluggable (`pci_config_ops`) —
CF8/CFC port I/O on amd64, **ECAM MMIO** on AArch64. On bare-metal QEMU `virt` (no firmware to
program BARs) the kernel assigns BARs from MMIO windows (`pci_set_mmio_windows` /
`pci_alloc_mmio`) and maps them via `arch_vmm_map_device`, so the **same PCI providers run on
both architectures**.

**Drivers:** VirtIO **block** (a provider behind the `block_register` contract), **input** and
**GPU**; a full polled **USB stack** (`drivers/usb/`): `usb_core` enumeration + HCDs **xHCI
(3.0)**, **EHCI (2.0)**, **UHCI (1.1)**, the **hub** class (route strings / TT) and **HID
boot** keyboard/mouse; per-arch UART (`pl011`/`16550`), interrupt controller (`gic`/`apic`),
timer, and **PS/2** (8042, amd64 only — bring-up is presence-probed and **non-blocking**, so an
absent controller never hangs the boot).

**Unified input:** every provider (virtio-input, PS/2, USB-HID) feeds the single sink
`input_report(type, code, value)` ([keyboard.c](../kernel/drivers/keyboard/keyboard.c)) with
evdev events; keys go through the layout + IPC to the focused process, pointer events update
the compositor (repainted on its own ~30 Hz tick, never from the input path). Interrupts are
routed via `kernel/irq/irq.c` (+ arch dispatch). Real-hardware and robustness items remain
open (see `area:drivers` issues).

## 9. Filesystem & disk image

- **Partitioning (`fs/gpt.c`):** GPT parser with a legacy **MBR** fallback (for hybrid ISOs);
  CRC32 verified for the header.
- **Ext4 (`fs/ext4.c`):** reads the superblock, group descriptors, inodes and directory
  entries; supports **extent-tree inodes (any depth)** plus the legacy direct/indirect
  block maps, with INCOMPAT feature enforcement at mount (unknown RO_COMPAT ⇒ read-only;
  unknown INCOMPAT ⇒ loud refusal). **Write** supports legacy single-indirect (~4.2 MB)
  and extent depth-0 append; an interior-block cache speeds tree walks.
- **VFS (`fs/vfs.c`):** real provider layer — `struct fs_ops` + a mount table (GPT
  partition → fs driver); all file syscalls and the ELF loader route through it. Zero
  `ext4_*` calls outside `kernel/fs/`.
- **Disk image:** `tools/mkdisk.c` builds a 96 MB image: bootloader + kernel + a GPT/MBR
  Ext4 partition populated from `build/<arch>/rootfs/` (`/sys/bin`, `/bin`, `/etc`, `/fonts`,
  and DOOM WADs). Extent-tree inodes by default (`--legacy` opt-out). `make rootfs` stages it.

## 10. Graphics & windowing

`kernel/graphics/`. `graphics_init` brings up the **VirtIO-GPU** framebuffer (720×1280 today;
this is the only GPU provider — there is no VGA/VBE/Bochs-DISPI provider yet, planned).
Rendering is CPU-side: `compositor.c` manages overlapping windows (Z-order, drag, focus,
damage tracking, TTY windows for shells); `font.c` renders TTF glyphs (uploaded by `nxfont`
via `set_font`); `gl.c` provides 2D/3D fixed-point primitives. The compositor and font engine
are in-kernel today and are prime candidates for extraction into userland services (epic #95).

Rendering works, but several **drawing bugs are known and tracked** in
[`review/analysis/06-graphics.md`](review/analysis/06-graphics.md): damage/redraw is
incomplete (windows don't refresh until the mouse passes over; dead-process windows linger —
GFX-COMP-04 #118), the damage region is a coarse bounding box (GFX-COMP-09), titles are drawn
unclipped (GFX-COMP-10), and the mouse path mutates window state from IRQ context without a
lock (GFX-COMP-02). These are scheduled for Fase 3/Fase 5.

## 11. Userland

- **`init` (`user/sys/bin/init.c`):** spawns `notify_srv` + `shell`, then a supervisor poll
  loop that respawns them. (Hardcoded list; `init.cfg` is not yet read — USR-INIT-02.)
- **`shell`:** line editor with built-ins (ls/cat/cd/ps/kill/spawn/…); opens a TTY window.
- **Services:** `notify_srv` (notification popups via IPC), `regedit` (registry UI),
  `nxfont` (TTF rasteriser/uploader).
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
1. Pick an unused number; `#define SYS_FOO` in `include/api/syscall_nums.h` (the single
   source for kernel and userland) and declare the wrapper in `include/api/os1.h`.
2. Add the stub in `user/arch/<arch>/syscall.S` (both arches, use the `SYS_FOO` macro —
   the stubs are preprocessed) and a convenience fn in `lib.c`.
3. Handle the `case SYS_FOO` in `kernel/core/syscall_dispatch.c`, using `arch_copy_*_user`
   for any user pointers; return 0/positive on success, **negative errno** on failure;
   add a capability check if the call acts on another process's resources.

**Add a driver**
1. Implement under `kernel/drivers/<class>/`. Either look the device up directly
   (`hal_device_find` / `hal_device_find_class` after `hal_bus_init`), or — preferred —
   declare a `struct device_driver` (match by vendor:device or PCI class) and call
   `driver_register()` so the device manager binds it automatically.
2. Register an IRQ handler with `irq_register`; add the source file to the `Makefile`
   `KERN_C_SOURCES`. Keep bring-up **non-blocking**: never spin on a status bit unbounded —
   use `poll_until`/`spin_until` (`kernel/io_poll.h`) so an absent/wedged device degrades
   gracefully instead of hanging the boot (see `drivers/ps2/ps2.c`).

## 13. Logging & diagnostics

- `printk` with levels (`pr_info`/`pr_warn`/`pr_err`/`pr_debug`); `console_loglevel`
  gates output. All output also goes to the serial console (`-serial`).
- Unit tests: `ktest_run_all()` runs cases from the `.ktests` section **after memory
  init** and reports real PASS/FAIL counts (LIB-KTEST-01 fixed); 5 cases today (string ×2,
  math, kmalloc growth, vmm_protect).
- `make debug` exposes a gdb stub: `gdb build/<arch>/kernel.elf` then `target remote :1234`.
- `panic()` prints a register/stack dump and (on amd64) halts the CPU.

## 14. Known limitations

The authoritative, severity-ranked list is [`review/REVIEW.md`](review/REVIEW.md) and the
GitHub issues (`code-review` label). Highlights (post B1/B2, mid-B3, 2026-06-12):

- **amd64 parity gaps** (ACPI/MADT CPU count, FPU context, PCI init) — epic #94. (The PCI-hole
  RAM-accounting bug is fixed: MM-PMM-08 #117, `-m 5G` now reports 5119 MB usable / 6144 span.)
- IPC queue is still **unbounded** (no per-queue quota; SCHED-05/#122 residue → B5/B6). The
  modern terminal protocol (#123 problem 2) is post-B3.
- **set_font** hands a raw user pointer to the kernel (UAF risk). *(W4)*
- Several **SMP data races** remain in drivers/compositor (uaccess TOCTOU, lock-free
  shared state — see `area:drivers`/`area:graphics` issues).
- Compositor/graphics: hardcoded resolution, damage tracking misses programmatic updates,
  windows of dead processes linger until hovered; notification popups unreliable.
- Allocators don't reclaim to the PMM; userland binaries are large; the userland libc is
  minimal and not POSIX-conformant.

These are the subject of the ongoing refactor (see `PROJECT_CHARTER.md`,
`PHASE-B-PLAN.md` and the epics #93–#96).

---

*License: GPL v2 ([`../LICENSE.md`](../LICENSE.md)).*
