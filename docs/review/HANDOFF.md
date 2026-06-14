# HANDOFF — OS1 / NEXS (read this first)

You are taking over an ongoing **code-review + incremental-hardening** effort on
**OS1/NEXS**, a from-scratch, dual-architecture (AArch64 + x86-64) OS that boots
graphically on QEMU (SMP, VirtIO GPU/input/block, Ext4, window compositor, TTY shell).
All work is on branch **`comprehensive-review`** (pushed to `origin`).

## 0. Read before doing anything
- `docs/review/REVIEW.md` — master findings index (~220, W0–W5 × kind). **§8 = what's already
  fixed and what's still open.** Start there.
- `docs/PROJECT_CHARTER.md` — the goals (seL4-style isolation, Plan 9 "everything is a file",
  thin HAL, coherent ABI). This is the north star.
- `docs/MANUAL.md` — build/run, boot flow, memory model, syscall ABI, how-to-extend.
- `docs/review/analysis/01..09-*.md` — per-subsystem deep analysis (every finding cites file:line).
- `docs/review/TAXONOMY.md` — severity/kind scheme used everywhere.
- GitHub issues (`olmox001/os1test-dev`): label `code-review` = 72 per-finding issues + 5 epics
  + tracking #19. Each issue has file:line + a fix direction.

## 1. Current state (all committed + pushed)
- **Review complete**; README/MANUAL/CHARTER/REVIEW + 9 analysis docs written. README fixed
  (license is **GPLv2**, not MIT; dual-arch; honest status).
- **Phase 2 (comments):** ALL C sources are commented + committed (provably comment-only).
  **Headers + `.S` were reverted** (delegated agents emitted malformed `/* */` / reformatted
  asm and broke the build) → still TO REDO, carefully.
- **Phase 3 fixes landed + runtime-verified + pushed:**
  - amd64 ≥4GB now works: 64-bit PCI BAR read (`0c5dc0a`), high device-MMIO cloned into process
    PGDs (`89c3a52`), PVH-detect-via-struct-magic → real memory map (`fedd9e2`). Closed #44/#28/#29.
  - amd64 SMP `current_task` page-fault fixed: `*(.lbss*)`→`.bss` so PMM metadata stops
    overlapping `cpu_data` (`8b03255`).
  - aarch64 DTB/FDT fixed: raw `kernel.bin` + generated `-dtb` so `x0`=DTB; SMP fallback cap
    64→8 (`b3ea74f`).
  - IPC is **already 64-bit** (`struct ipc_message` has `uint64_t data1/data2` + `payload[64]`) — no change needed.
- Both arches build clean (strict `-Werror -Wall -Wextra -Wpedantic -Wshadow`) and boot to a TTY
  shell (amd64 verified to 8GB; aarch64 FDT-driven).
- **Drivers (2026-06): device manager + USB + non-blocking PS/2.** `d0791db`/`14f2744`: PCI
  driver-binding (`driver_register`/`driver_match_all`), arch-neutral PCI (CF8/CFC + AArch64
  **ECAM** + kernel BAR assignment), full polled **USB stack** (xHCI/EHCI/UHCI/hub/HID), unified
  `input_report` sink (virtio-input/PS-2/USB-HID), input decoupled from the compositor render.
  PS/2 bring-up is now **presence-probed + non-blocking** (it was hanging UTM's BIOS-less boot at
  an unbounded 8042 flush loop). The active plan is
  [`docs/PIANO-DRIVER-MATURITY.md`](../PIANO-DRIVER-MATURITY.md) (Fase 0→6: non-blocking error
  handling, hotplug/recognition, compositor↔process decoupling #83, userland cleanup, real B3
  closure).

## 2. HOW to work here (discipline — follow it)
1. **Verify everything YOURSELF before committing.** Delegated agents have broken arch code,
   headers, and `.S` in this very repo. Never trust an agent's "done" — re-run build + runtime.
2. **One agent at a time** (maintainer's rule). Scope each task tightly; the maintainer verifies
   before commit. Agents must NOT commit — you commit after verifying.
3. **Build BOTH arches** after every change: `make all ARCH=aarch64` and `make all ARCH=amd64`,
   expect 0 errors. amd64 has ONE pre-existing benign linker warning (`'.note.PVH' not in
   segment`) — ignore it.
4. **Runtime-verify** with headless QEMU + serial capture (see §3). For boot/SMP/intermittent
   bugs, run **multiple times** (races).
5. **IDE/clangd diagnostics are NOISE** here (`file not found`, `unknown type uint64_t`) — clangd
   lacks the build's `-I` flags. Trust the real cross-compiler only.
6. **The `build/` dir is wiped by the environment between steps** — do build+run in ONE script.
7. **Comment-only tasks must be additions-only**: prove it by comment-stripping +
   whitespace-normalizing each file vs HEAD and diffing (must be empty). In `.S` use `/* */`.
8. **Commit small** (one fix per commit), message ending:
   `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Push to `comprehensive-review`.
   Don't commit the maintainer's local Makefile tweaks unless intended.
9. The kernel currently runs **identity-mapped (PA==VA)**; much code assumes it. Don't break it casually.

## 3. Commands
- Toolchain check: `make check ARCH=aarch64` / `ARCH=amd64` (cross `aarch64-none-elf-`,
  `x86_64-elf-`; QEMU both).
- **Headless amd64** (one shell; foreground `sleep` is blocked → use a background job):
  ```sh
  make all ARCH=amd64
  qemu-system-x86_64 -m 5G -smp 4 -serial file:/tmp/x.log -display none \
    -device virtio-gpu-pci,disable-legacy=on,disable-modern=off \
    -device virtio-keyboard-pci,disable-legacy=on,disable-modern=off \
    -device virtio-mouse-pci,disable-legacy=on,disable-modern=off \
    -drive if=none,file=$PWD/build/amd64/disk.img,id=hd0,format=raw \
    -device virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off \
    -kernel $PWD/build/amd64/kernel.elf & P=$!; sleep 28; kill $P 2>/dev/null
  ```
- **Headless aarch64** (needs the generated DTB; uses the RAW bin):
  ```sh
  make all ARCH=aarch64 && make build/aarch64/virt.dtb ARCH=aarch64
  qemu-system-aarch64 -M virt -cpu cortex-a57 -m 5G -smp 4 -serial file:/tmp/a.log -display none \
    -device virtio-gpu-device -device virtio-keyboard-device -device virtio-mouse-device \
    -drive if=none,file=$PWD/build/aarch64/disk.img,id=hd0,format=raw -device virtio-blk-device,drive=hd0 \
    -dtb $PWD/build/aarch64/virt.dtb -kernel $PWD/build/aarch64/kernel.bin & P=$!; sleep 30; kill $P 2>/dev/null
  ```
- **Success** in serial: `VirtIO: Block Device Initialized successfully`, `Ext4: Mounted`,
  `TTY Window ... PID 7`, all CPUs online. **Failure**: `PAGE FAULT`, `Unhandled CPU Exception`,
  `KERNEL PANIC`, `Invalid queue size (0)`.

## 4. Remaining roadmap (priority order, with leads)
1. **Higher-half addressing rework + W^X** — the central foundation (MM-VMM-01/02, AMMU-01/02:
   identity-map assumption + all RAM mapped executable). Big/multi-session; do **aarch64 first**
   as the reference, test each increment, never leave the boot broken.
2. **amd64 hardware discovery**: ACPI-MADT CPU count (ARCH-01; currently CPUID, unreliable);
   real PCI/ACPI init (ARCH-02 is a stub); **user-vs-kernel fault isolation** (EXC-AMD64-02 —
   currently a user div-by-zero halts the whole machine; should terminate the process).
3. **Re-comment headers + `.S`** (Phase-2 leftover) — comment-only, additions-only, build-verify
   per file. Previous agents botched this; go slow, small batches.
4. Work the `code-review` issues by severity (W3 first): bounded IPC (SCHED-05), IPC lost-wakeup
   (IPC-01), TLB shootdown (MM-VMM-05/AMMU-08), uaccess TOCTOU (UACC-*), ext4 extents (EXT4-01),
   real VFS layer (VFS-01), etc.
5. Toward the charter: **capability-checked ABI** (ABI-04 — zero access control today: any PID can
   kill/write anything), real allocators (MM-KM-01 — kmalloc never frees to PMM), extract
   compositor/VFS/fonts to **userland services**, Plan 9 file-namespace registry (LIB-REG-01).

## 5. Landmines already learned
- The `0x100000003` SMP fault was `.lbss` (large static arrays like `cpu_data[]`) landing above
  `__kernel_end` and overlapping PMM metadata. If you add big static arrays, make sure linker
  scripts capture `.lbss`/`.lrodata`/`.ldata`.
- amd64 `make run` boots via QEMU `-kernel` → **PVH** (not multiboot2; multiboot1 fails because
  the kernel is a 64-bit ELF). aarch64 `-kernel` only passes `x0`=DTB for a RAW image → we use
  `kernel.bin` + `-dtb`.
- `ktest` always reports PASS (LIB-KTEST-01) — it is NOT a real gate; verify by running.
- The fixed MMIO window is `0xFE000000–0xFFFFFFFF`; >4GB device BARs are handled via
  `arch_vmm_map_device()` + the PML4[1..255] clone in `arch_vmm_create_process_pgd`.
- The maintainer's environment also has an unrelated `os2` QEMU running from `~/Desktop/os2` — not
  this project; don't touch it.

Good luck. Keep the boot green, commit small, verify twice.
