# Phase B — STUB demolition: plan & session handoff

> **Purpose**: self-contained handoff so a fresh session (zero context) can
> start Phase B microphase B1 immediately.  Read this + the linked review
> docs; everything else is derivable from the repo.
>
> **Status (2026-06-12)**: Phase A is **100% complete**.  Phase B: **B1 DONE**
> (VFS + ext4 extents + residuals), **B2 DONE** (epic #92 closed — W^X,
> teardown, allocators, TLB shootdown, **higher-half kernel landed on both
> arches**), **B3 IN PROGRESS** (epic #93 — numbering+errno and the first
> capability layer landed; fd table #90, formal IPC, sandboxing #79 remain).
> All work lives on branch `comprehensive-review` (pushed to origin); the
> maintainer merges to `main` himself.

---

## 1. Where we are (2026-06-11)

Phase A delivered a precise, recoverable fault/trace foundation on BOTH
arches (aarch64 + amd64), then closed its residuals and the issues confirmed
from an external boot-trace analysis.  Full catalog: `docs/review/REVIEW.md`
§8 (commit table) and §9; narrative addenda:
- `docs/review/analysis/10-addendum-2026-06-11-sched-uaf-followup.md`
- `docs/review/analysis/11-addendum-2026-06-11-external-trace-triage.md`

Headline guarantees now in force:
- **No silent crash**: every vector on both arches reports (fault-safe
  printing, dedicated fault stacks, recursion guard, total vector coverage).
- **User faults never kill the kernel**: `crash` terminates the process,
  shell survives (`fault_handle_user_or_panic`, uaccess windows flagged).
- **Symbolized backtraces** live in-kernel (`.ksyms`, survives objcopy).
- **Process lifecycle is leak-free and race-free**: zombies auto-reaped by
  `schedule()`; corpse freeing single-owner; `process_terminate` parked
  check for all sleeping victims; init supervisor deterministic
  (`wait()==-2` ⇒ child gone ⇒ respawn).
- **schedule() has an IRQ contract** (self-masks; any entry state legal).
- **virtio-blk serialised** (the doom `Read failed status=1` SMP race).
- **IRQ layer**: locked handler table, chip-owned EOI, spurious PIC filter.

### Phase A commit list (newest last)
| Commit | What |
|---|---|
| `12843d4` | fault-safe printing, in_fault guard, panic fault-mode |
| `61c871b` | dedicated fault stacks (amd64 IST1/IST2, aarch64 EL1 abort stack) |
| `d6f03e9` | user/kernel fault isolation (EXC-AMD64-02 / #36 fixed) |
| `ac4d76f` | symbolized backtrace (.ksyms two-pass link) |
| `6bed3cf` | total aarch64 vector coverage |
| `b9aad52` | zombie auto-reap; process_wait pure reporter |
| `dc8a3db` | virtio-blk serialisation + static DMA targets |
| `9bf27af` | step 14: PIT halted post-calibration; dead probe block removed |
| `2212423` | step 15: irq table lock, chip-owned EOI, spurious filter (#47, #55) |
| `166887a` | SCHED-IRQ-01: schedule() self-masks IRQs |
| `db503a3` | Z-order focus reset; keystroke log → debug |
| `f37d137` | deterministic respawn; parked check for all sleeping victims |

---

## 2. Build & test playbook (memorize this, it is the whole loop)

```sh
# Build (run for BOTH arches before every commit)
make all ARCH=amd64
make all ARCH=aarch64
make build/aarch64/virt.dtb ARCH=aarch64   # once, if missing

# Headless boot — amd64.  CRITICAL: virtio-pci devices MUST have
# disable-legacy=on; the legacy transport hangs at the first blk read
# (open issue, addendum 11 §2.5).
qemu-system-x86_64 -m 5G -smp 4 -kernel build/amd64/kernel.elf \
  -drive if=none,file=build/amd64/disk.img,id=hd0,format=raw \
  -device virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off \
  -device virtio-keyboard-pci,disable-legacy=on,disable-modern=off \
  -device virtio-gpu-pci,disable-legacy=on,disable-modern=off \
  -device virtio-mouse-pci,disable-legacy=on,disable-modern=off \
  -serial file:/tmp/nexs.log -display none \
  -qmp unix:/tmp/qmp.sock,server,nowait -d cpu_reset -D /tmp/qemu.log

# Headless boot — aarch64 (raw kernel.bin + -dtb are mandatory)
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 5G -smp 4 \
  -kernel build/aarch64/kernel.bin -dtb build/aarch64/virt.dtb \
  -drive if=none,file=build/aarch64/disk.img,id=hd0,format=raw \
  -device virtio-blk-device,drive=hd0 -device virtio-keyboard-device \
  -device virtio-gpu-device -device virtio-mouse-device \
  -serial file:/tmp/nexs.log -display none \
  -qmp unix:/tmp/qmp_a64.sock,server,nowait

# Type into the shell via QMP (script is committed in tools/)
python3 tools/qmp_type.py /tmp/qmp.sock "counter\n" 14   # 3rd arg = pre-delay s
# Boot takes ~12-16 s to the shell.  Useful smoke commands: ps, ls,
# counter, crash (user #PF, must NOT kill the kernel), doom, exit (tests
# init respawn), kill <pid>.

# Health greps after a run
grep -cE 'PANIC|NESTED|Unhandled interrupt' /tmp/nexs.log   # want 0
grep -c 'CPU Reset' /tmp/qemu.log   # ~11 at boot (INIT/SIPI) is normal; none later
```

Toolchain: aarch64 is **pinned to GCC 7.2.0** (sergiobenitez/osxct) — do NOT
upgrade; amd64 uses x86_64-elf-gcc.  `-Werror` with `-Wmissing-prototypes`:
every new extern function needs a header declaration.  Two-pass link emits
`.ksyms` (cosmetic `ksyms.S:4` assembler warning is known/harmless, as is
amd64 `.note.PVH`).

Workflow: commit per logical fix on `comprehensive-review` with descriptive
messages; verify build (both arches) + boot (both arches) + a targeted
runtime test BEFORE each commit; never merge/push to `main` (maintainer
does); an auto-commit watcher may create "update" commits from a dirty tree.

---

## 3. Phase B — microphases (confirm each one with the maintainer first)

Severity-ordered per the review (`docs/review/REVIEW.md` §2/§4, taxonomy in
`docs/review/TAXONOMY.md`, ~72 findings as GitHub issues, label
`code-review`, tracking issue #19, epics #92–#96).

> **Implementation method**: every microphase follows the **ASTRA** layering
> guidelines (`docs/ASTRA.md`, adapted from the maintainer's references,
> 2026-06): the kernel core consumes contracts only; hardware support is
> always a *provider* behind a contract in `kernel/include/kernel/`; arch
> dirs may only grow ISA-layer code. See ASTRA §3 for the per-microphase
> application and §5 for the per-commit rules; ASTRA §4 sketches a possible
> Phase C (userspace ELF driver services).

### B1 — Filesystem W5 criticals — **DONE (2026-06-11)**
**Scope**: VFS-01 (#64) + EXT4-01 (#56) — the two open W5 findings.
**Landed**: `e64756e` (VFS provider contract + ext4 extents + INCOMPAT
enforcement) and `db04684` (mkdisk extent layout by default, `--legacy`
opt-out).  Acceptance met on both arches: shell/counter/doom from an
extents rootfs; zero `ext4_*` calls outside `kernel/fs/`; poisoned
INCOMPAT bit refused loudly.  Residuals noted in ext4.c header: write
path still legacy-only/48 KB (EXT4-05), no caching (EXT4-11).
- Today: `kernel/fs/vfs.c` is 149 lines of path-string normalisation only;
  every syscall calls `ext4_*` directly; `kernel/fs/ext4.c` (811 lines)
  supports only the legacy indirect-block layout — **extent-tree inodes
  (EXT4_EXTENTS_FL) read garbage**; mkfs.ext4 enables extents by default,
  so the rootfs only works because the image builder forces the old layout.
  Incompat feature flags are read but never enforced.
- Plan sketch:
  1. `struct vfs_node`/`struct fs_ops` (open/read/write/list/stat), a mount
     table (GPT partition → fs driver), ext4 registered behind it; syscalls
     251/252/254 + ELF loader route through VFS only.
  2. ext4: parse `ext4_extent_header/idx/extent` in `i_block[]`, walk the
     tree (depth ≥ 1), keep indirect-block fallback; enforce/whitelist
     INCOMPAT flags at mount (reject 64-bit/meta_csum loudly instead of
     corrupting).
  3. Tests: boot both arches on an extent-enabled image (rebuild disk with
     stock mkfs.ext4) + regression on the current image; doom WAD load as
     the large-file stress.
- **Acceptance**: shell + doom + counter from an extents rootfs on both
  arches; no direct `ext4_*` call left outside the VFS layer.

### B2 — Epic #92: memory/address-space rework — **DONE (2026-06-12), findings closed; higher-half LANDED same day**
**Batch 1**: `f4ad8fa` full teardown (MM-VMM-04 #24, AMMU-03 #35 — user
frames + private tables freed, leak-free spawn/exit verified both arches;
aarch64 header-page cross-process aliasing fixed) and `b745a74` W^X
(MM-VMM-01 #22, AMMU-01 #33, ELF-02 #87 — text RX, rodata RO+NX, all other
RAM RW+NX, EFER.NXE, user stack/heap never executable; `nxtest` proves the
fault path).
**Batch 2**: `0b9f6d5` MM-PMM-02 #21 (multi-page DMA cache-clean+fence);
`29bb092` VFS-02 #65 (resolve-path NULL guard); `508c734` MM-BUF-01 #26
(hard buffer-cache cap: slot reservation + sync-retry + loud refusal);
`67ff898` MM-KM-01 #27 (kmalloc grows by 4 MB PMM chunks; ktest proves
growth); `06f017a` MM-VMM-05/AMMU-08 #25 (cross-CPU TLB shootdown
contract: aarch64 = hardware IS TLBI, amd64 = LAPIC IPI round on vector
0xFD wired into unmap/teardown; bonus amd64 panic-halt IPI 0xFE);
`834e347` AMMU-02 #34 (real arch_vmm_protect both arches + two latent
walker bugs: aarch64 block-split level off-by-one that emptied L3 tables,
block-blind get_physical); `cf8fca1` MM-VMM-02 walker half #23 (all
walkers via phys_to_virt/virt_to_phys — identity assumption centralized
in vmm.h).  Also closed as already-fixed-by-B1: #58 #60 #61.
**Higher-half/PA-VA migration (LANDED 2026-06-12, same-day follow-up)**:
`fb4506a` PA/VA contract sweep behind `KERNEL_VIRT_BASE` (memlayout.h;
MM-PMM-07 #—: PMM returns direct-map pointers, every PA↔pointer crossing
through phys_to_virt/virt_to_phys, MMIO accessors translated, identity-
neutral); `8b401f5` aarch64 flip (image at 0xFFFF000040080000, TTBR0/TTBR1
split: kernel half permanently in TTBR1, pure-user process PGDs, empty
idle TTBR0, MMU+caches enabled in start.S boot tables, PSCI entry PA
conversion, fixes latent TCR.IPS=0 32-bit-PA bug); `56dddcf` amd64 flip
(image at 0xFFFF800000200000 with low boot stub at 1MB VA==PA, PML4[256]
alias in boot tables, APs boot on boot_pml4 then adopt kernel_pgd,
pure-user+high-copy process PML4s, pre-populated kernel slots 256..259,
low-2MB identity window for the trampoline kept in every kernel PGD).
Full matrix green on both arches: KTEST 5/5, writetest 3/3, nxtest W^X
kill, crash isolation leak-free, doom, 0 panics.  Unlocks ASLR/KASLR.
W2-class refinements (AMMU-04..07, MM-KM-02..06, MM-PMM-03..06,
MM-BUF-02..05) remain open under the epic.  HAL isolation held: zero
`platform.c` edits (amd64).

### B3 — Epic #93: coherent ABI + capabilities — **IN PROGRESS (2026-06-12)**
**Batch 1 (`0bef4c3`)**: single syscall numbering in
`include/api/syscall_nums.h` shared by kernel switch + userland .S stubs
(ABI-01 #88, ABI-SYS-01 #75; duplicate IPC 30/31/32 removed, TRY_RECV→233,
SET_FOCUS finally public); negative-errno model kernel-wide (ABI-02 #89);
ABI-05 yield-after-send fixed; sbrk heap ceiling 0xBF000000 (stack guard).
**Batch 2 (`11f642a`)**: capability layer at the dispatcher — KILL =
self/children/SYSTEM-ROOT via new `parent_pid` + `process_kill_allowed`
(ABI-04 #91, USR-SEC-02 #78); SET_FOCUS self-only; DESTROY_WINDOW
owner-only (`compositor_window_owner`); FILE_WRITE denies /bin + /sys to
non-SYSTEM (EXT4-02 #57); registry first-writer-wins key ownership
(LIB-REG-02 #73, USR-SEC-01 #77).  Smoke-tested live on both arches
(kill init/notify denied, kill own child allowed).
**Batch 3 (`f9a0b09`)**: per-process **fd table** (ABI-03 #90) —
`kernel/fd.h` (0=kbd stdin, 1/2=own window, FD_FILE ≥3, private offset),
`open`/`close`/`lseek` = 56/57/62, read/write via the table (file I/O
through capped bounce buffers, window writes keep the 1023 cap ABI-06),
write-open ACL on /bin+/sys, O_CREAT → -EINVAL; legacy fd≥100 window
alias kept; `/bin/fdtest` 8/8 on both arches + writetest regression.
**Batch 4 (`51c3179`)**: anti fork-bomb quotas (SCHED-DOS-01 #122,
maintainer crash1/crash2 report) — memory-derived `proc_limit`,
`MAX_PROCS_PER_PARENT`=32 (new `child_count`), `RESERVED_PROC_SLOTS`=8
for privileged recovery; `/bin/forkbomb` plateaus at 32 and the shell
kills it. Per-window/per-IPC-queue quotas still open under #122.
**Batch 5 (`5f5ae7e` + `225e294`)**: SCHED-DOS-02 follow-up — a dead
bomber's children were unkillable orphans evading the quota; now
`__reparent_children()` re-homes them to the nearest live ancestor
(charging its `child_count`) and the kill capability covers all
DESCENDANTS.  IPC-01 #85 closed: blocking recv had never worked — the
reviewed lost-wakeup (fixed re-checking the queue under `msg_lock`)
plus an aarch64-specific arg clobber (return written into x0 = arg0
after the armed syscall retry → recv re-ran with src_pid=0 and slept
unwakeable; `IPC_RECV_RETRY` sentinel keeps the frame untouched).
Sender auth was already in place (`msg.from` kernel-stamped).
**Remaining**: sandboxing (USR-SEC-03 #79 — epic-level outcome, needs
maintainer direction); SCHED-05 AB-BA lock chain stays slotted in B6.

### B4 — Epic #94: amd64 parity (ACPI-MADT CPU count ARCH-01, real
PCI/ACPI init ARCH-02, FPU/XMM save on context switch CPU-AMD64-01,
IST-NMI paranoid entry, remove int 0x80 surface SYS-AMD64-03).
Related: MM-PMM-08 #117 (PCI-hole RAM accounting — generic mm fix,
can land ahead of B4).

### B5 — Epic #95: services/HAL + Plan 9 namespace (compositor→sched
decoupling SCHED-01/GFX-COMP-03 #69, init.cfg actually read USR-INIT-02,
service supervision rate-limit USR-INIT-03, HAL unification).
Related maintainer reports: GFX-COMP-04 #118 (damage/redraw),
USR-NOTIFY-01 #119 (notification popups + kernel-log bridge),
GFX-DYN-01 #121 (dynamic resolution / resize / font alpha / stb images).

### B6 — Epic #96: SMP/races sweep + leftovers: async block I/O
(DRV-VIRTIO-08 — busy-wait now runs with IRQs masked under the blk lock;
correct but throughput-hostile), blocking `wait()` + exit status
(needs SCHED-06 parent/child), kernel_ipc_send
AB-BA chain SCHED-05, legacy virtio-pci transport (addendum 11 §2.5).
(IPC-01 lost-wakeup: già chiuso in B3 batch 5, `225e294`.)

### Maintainer-reported issues (2026-06-12) — slotting

Five reports filed after live use of the higher-half build:

| Issue | What | Slot |
|---|---|---|
| **#117** MM-PMM-08 | `-m 5G` ⇒ "6144 MB": total RAM from highest region END; the 3–4 GB amd64 PCI hole is **counted and allocatable as RAM** (not just a log error) | mm fix, land ASAP (pre-B4; related #20, epic #94) |
| **#118** GFX-COMP-04 | windows only repaint under the mouse; **dead processes' windows linger** until hovered (damage not fed by programmatic updates / destroy) | B5 (graphics); bugfix can land earlier |
| **#119** USR-NOTIFY-01 | notification popup never appears; kernel/user warnings+errors should surface as notifications (needs a log→notify bridge) | B5 (services; related #76, #81) |
| **#120** EPIC userland | layered "onion" userland per ASTRA: custom posix-like kernel ABI → POSIX-as-user-services → conformant libc; GDK-like layered 2D toolkit; OpenGL/Mesa-like 3D; busybox philosophy (standards, no reinvention) | new epic, after/with B5 (needs fd table #90) |
| **#121** GFX-DYN-01 | no hardcoded resolution/values anywhere; virtio-gpu auto resolution (#54, #49); desktop/window resize; font alpha/scaling; stb image formats | B5 (graphics+drivers, under epic #120 umbrella) |

---

## 4. GitHub issues — DONE (2026-06-11)

`gh` is installed and authenticated as `olmox001`.  Closed with fix
comments: **#36** (EXC-AMD64-02, `d6f03e9`), **#37** (EXC-AMD64-03,
`9bf27af`), **#46** (DRV-VIRTIO-03, `dc8a3db`), **#47** (IRQ-01,
`2212423`), **#55** (IRQ-02, `2212423`), **#101** (SCHED-UAF-01,
`3296ce1`/`db4eb4c`/`3509a4f` + lifecycle hardening), **#102**
(ARCH-AMD64-APPGD-01, `3296ce1`).  Phase A status comment posted on the
tracking issue **#19**, including the Phase B pointer.

Fixes from this batch without a dedicated issue (covered by REVIEW.md §8
rows): SCHED-03 zombie reaping (`b9aad52`+`f37d137`), DRV-VIRTIO-04
(`dc8a3db`), EXC-AMD64-01 (`9bf27af`), SCHED-IRQ-01 (`166887a`), focus
reset (`db503a3`).

---

## 5. Known pitfalls for the next session

- **amd64 QEMU without `disable-legacy=on` hangs at "Partition:
  Initializing..."** — it's the legacy transport, not your change.
- aarch64 without `-dtb` boots via the RAM-probe fallback (slower, but must
  keep working — it exercises the abort-vector probe path).
- `process_wait()` is a **pure reporter**; never free corpses outside the
  scheduler reaper.  Supervisors treat `-2` as "child gone".
- The shell takes keyboard focus itself at startup (syscall 232); killing it
  externally while it sleeps in `ipc_recv` exercises the parked-corpse path.
- grep pitfall that bit us: searching userland for `wait` matches thousands
  of numeric lines in `user/bin/doom/tables.c` — always check
  `user/sys/bin/*.c` explicitly when changing process/IPC semantics.
- `make run ARCH=...` opens an interactive window (uses the canonical
  QEMU_FLAGS in the Makefile §"QEMU Flags") — good for GUI checks.

---

## 6. Architectural north star

`docs/ASTRA.md` — the service-tree/provider model the maintainer set as the
implementation method for Phase B and for a candidate Phase C (functional
drivers as supervised ELF services on `map_mmio`/`wait_irq`/`dma_alloc`/IPC
primitives; `blk.elf` migrates last because the rootfs depends on it).  Key
acceptance hook already wired into B1: **no `ext4_*` call outside
`kernel/fs/`** — ASTRA's "providers behind contracts" rule applied to the
first seam.
