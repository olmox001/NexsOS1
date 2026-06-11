# Phase B — STUB demolition: plan & session handoff

> **Purpose**: self-contained handoff so a fresh session (zero context) can
> start Phase B microphase B1 immediately.  Read this + the linked review
> docs; everything else is derivable from the repo.
>
> **Status**: Phase A is **100% complete** (core + residuals + external-trace
> fixes + the supervisor-respawn fix).  All work lives on branch
> `comprehensive-review` (pushed to origin); the maintainer merges to `main`
> himself.

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

### B1 — Filesystem W5 criticals (FIRST; awaiting confirmation)
**Scope**: VFS-01 (#64) + EXT4-01 (#56) — the two open W5 findings.
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

### B2 — Epic #92: memory/address-space rework (largest, most invasive)
Higher-half kernel, W^X (MM-VMM-01/AMMU-01), real PA/VA separation
(`virt_to_phys` is identity today — `vmm.h`), allocator hardening, full
teardown paths.  Prereq for ASLR/KASLR.  Touches every arch boundary: keep
the per-arch work inside `kernel/arch/<arch>/` (HAL isolation rule; do not
touch `kernel/arch/amd64/platform/platform.c`).

### B3 — Epic #93: coherent ABI + capabilities
Single syscall numbering (ABI-01), errno model (ABI-02), per-process fd
table (ABI-03), capability checks killing ABI-04 (any process can kill PIDs,
steal focus via syscall 232, overwrite files), formal IPC API (the external
review's "no IPC model" point), registry auth (USR-SEC-01).

### B4 — Epic #94: amd64 parity (ACPI-MADT CPU count ARCH-01, real
PCI/ACPI init ARCH-02, FPU/XMM save on context switch CPU-AMD64-01,
IST-NMI paranoid entry, remove int 0x80 surface SYS-AMD64-03).

### B5 — Epic #95: services/HAL + Plan 9 namespace (compositor→sched
decoupling SCHED-01/GFX-COMP-03 #69, init.cfg actually read USR-INIT-02,
service supervision rate-limit USR-INIT-03, HAL unification).

### B6 — Epic #96: SMP/races sweep + leftovers: async block I/O
(DRV-VIRTIO-08 — busy-wait now runs with IRQs masked under the blk lock;
correct but throughput-hostile), blocking `wait()` + exit status
(needs SCHED-06 parent/child), IPC lost-wakeup IPC-01, kernel_ipc_send
AB-BA chain SCHED-05, legacy virtio-pci transport (addendum 11 §2.5).

---

## 4. GitHub issues — status updates to apply

`gh` is not installed on this machine and no API token is available
non-interactively.  After `brew install gh && gh auth login`, apply:

```sh
R=olmox001/os1test-dev
gh issue comment 36 -R $R -b "Fixed on comprehensive-review in d6f03e9 (Phase A steps 8-10): every vector routes through fault_handle_user_or_panic; user faults terminate the process and the shell survives (verified both arches, QMP crash test). Closing on merge."
gh issue comment 47 -R $R -b "Fixed on comprehensive-review in 2212423 (Phase A step 15): EOI is chip-owned (irq_chip_end -> pic_chip_end does the full LAPIC+8259 sequence); the amd64 acknowledge-loop bypass is documented as by-design (vectored dispatch). Spurious IRQ7/IRQ15 now filtered via ISR before dispatch."
gh issue comment 55 -R $R -b "Fixed on comprehensive-review in 2212423 (Phase A step 15): irq_handlers[] guarded by irq_table_lock; both dispatch paths copy (handler,data) under the lock and invoke outside it."
# then: gh issue close 36 47 55 -R $R   (or close on merge, maintainer's call)
# Find the issue numbers for the other fixes of this batch (if filed):
gh issue list -R $R --label code-review --search "SCHED-03 zombie" 
gh issue list -R $R --label code-review --search "virtio stack DMA"
```

Fixes from this batch without a known issue number (covered by REVIEW.md §8
rows): SCHED-03 (`b9aad52`+`f37d137`), DRV-VIRTIO-03/04 (`dc8a3db`),
EXC-AMD64-01/03 (`9bf27af`), SCHED-IRQ-01 (`166887a`), focus reset
(`db503a3`).

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
