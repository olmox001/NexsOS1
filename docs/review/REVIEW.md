# OS1 / NEXS — Master Code Review

> Comprehensive, evidence-based review of the whole codebase (368 source files,
> ~107k LOC). Classification per [`TAXONOMY.md`](TAXONOMY.md): severity **W0–W5** ×
> **kind**. Per-subsystem detail under [`analysis/`](analysis/). Companion:
> [`../PROJECT_CHARTER.md`](../PROJECT_CHARTER.md) (purpose & target architecture).
>
> Review date: 2026-06-02 · Branch: `comprehensive-review` · Build: **[verified]** both arches.
> All 9 subsystems analysed (fs included). Agent-delegated docs are maintainer spot-checked.

---

## 1. Headline: verified runtime behaviour (claim vs reality)

The project is presented as "boots correctly on `make run` for both arches, graphical,
virtio, detecting CPUs, mapping up to 4GB, running GUI + TTY shell." Built and run
headless (QEMU, serial capture):

| Capability | aarch64 (`make run`) | amd64 (`make run`, `-kernel`) |
|---|---|---|
| Builds (strict `-Werror`…) | ✅ clean | ✅ clean |
| Boots to **TTY shell in a composited window** | ✅ | ✅ **at `-m 3G`** |
| RAM detection | ✅ **3967 MB, dynamic** | ❌ **magic `0x0` → hardcoded 1 GB** (ignores `-m`) |
| Maps "up to 4GB" | ✅ | ❌ (1 GB only) |
| SMP (cores online) | ✅ **4/4** (work-stealing) | ⚠️ boots; weak detection |
| `-m 4G` | ✅ | ❌ **crash** (virtio queue size 0 → divide-by-zero) |

**Verdict:** the aarch64 path genuinely delivers the stated asset. The amd64 path
delivers it **only at ≤~3 GB via `make run`**; the 4GB/real-memory-map path works only
through the **GRUB-ISO (`make release`)** route — which is what the old, more confident
reports actually tested. This is the single most important correction in the review.

### The amd64 critical chain (root-caused, verified)
`make run` boots amd64 via QEMU `-kernel` → PVH entry (kernel has MB2+PVH headers, **no
MB1**) → magic arrives as `0x0` (**BOOT-01**) → platform hardcodes **1 GB** (**BOOT-02**) →
at `-m 4G`, QEMU puts the virtio-pci 64-bit BAR above 4 GB → **`pci_get_bar` truncates it
to 32 bits** (**DRV-VIRTIO-01, W5**) → `QUEUE_NUM_MAX` reads 0 → divide-by-zero → amd64
has **no user/kernel fault isolation** so it **halts** (**EXC-AMD64-02**).

---

## 2. Severity rollup (all 9 subsystems)

| Severity | Count | Meaning |
|---|---|---|
| **W5** Critical | 3 | DRV-VIRTIO-01 (4G crash), EXT4-01 (extent format), VFS-01 (no VFS layer) |
| **W4** Severe | 9 | boot/4GB, stack-DMA, IRQ no-op, font UAF, ELF map-escape, no-capabilities, ext4 write+no-ACL |
| **W3** Significant | 60 | bugs/SMP-races/security/wrong-design on used paths |
| **W2** Moderate | 101 | limitations, partial behaviour, refinements |
| **W1** Minor | 42 | dead code, stale comments, micro-perf |
| **W0** Info | 5 | cosmetic |
| **Total** | **~220** | actionable tier (W3+) = **72** |

## 3. Cross-cutting themes → foundations (the refactor spine)

These recur across subsystems and are the dependency-ordered foundations (see charter §5):

1. **PA/VA model + W^X** — everything silently assumes identity mapping; all RAM is mapped
   executable. (MM-VMM-01/02, AMMU-01/02, ELF-02)
2. **Coherent, capability-checked ABI** — mixed/duplicated syscall numbers, no errno, no fd
   table, **zero capability checks**. (ABI-01/02/03/04, USR-SEC-01/02/03, LIB-REG-02)
3. **Real allocators** — kmalloc never frees to PMM; PMM has no buddy; userland malloc gaps.
   (MM-KM-01, MM-PMM-02/03, USR-MALLOC-01..05)
4. **Boot stability (GPLv2-compatible loader)** — amd64 boot-protocol mishandling. (BOOT-01/02)
5. **Thin HAL + real drivers/device-tree** — over-abstracted MMIO path; many driver stubs/races.
   (HAL-01, DRV-*, ARCH-01/02)
6. **Service isolation (seL4)** — compositor/font/vfs in-kernel and entangled with the scheduler.
   (SCHED-01, GFX-COMP-03, GFX-FONT-01, USR-SEC-03)
7. **SMP correctness** — no TLB shootdown; multiple lock-free shared-state races.
   (MM-VMM-05, AMMU-08, IRQ-02, DRV-UART-01, GFX-COMP-01/02, UACC-*-TOCTOU)
8. **Process/IPC model** — lost-wakeup race, unbounded IPC, non-blocking wait, no W^X for users.
   (IPC-01, SCHED-05, ELF-01)

---

## 4. Critical & severe findings (W5 / W4) — full

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| DRV-VIRTIO-01 | W5 | BUG | `pci/pci.c:106`, `amd64/hal.c:26-37` | 64-bit BAR truncated to 32 bits → garbage MMIO base at `-m 4G` → queue size 0 → divide-by-zero. **Root cause of the amd64 4G crash.** |
| BOOT-01 | W4 | BUG·WRONG-DESIGN | `amd64/boot/start.S`; `platform.c:157-186` | No MB1 header; QEMU `-kernel` uses PVH; magic expected in a register never matches → unknown protocol. |
| BOOT-02 | W4 | BUG | `platform.c:173-185` | Falls back to hardcoded 1 GB; 4GB unreachable on `make run`; fragile → 4G crash. |
| DRV-VIRTIO-03 | W4 | BUG·SECURITY | `virtio/virtio_blk.c:99-120` | `req`/`status` on the kernel **stack** used as DMA targets; device writes to stack; no coherency/alignment. |
| IRQ-01 | W4 | WRONG-DESIGN | `timer/pic_pit.c:57-59`, `irq/irq.c:87-135` | `acknowledge()` always returns 1023; generic `irq_handler` is a no-op on amd64; real dispatch bypasses the chip EOI contract. |
| GFX-FONT-01 | W4 | SECURITY·BUG | `graphics/font.c:174-191`, `syscall_dispatch.c:234` | `sys_set_font` stores a **raw user pointer** into kernel globals, dereferenced during IRQ-context rendering → UAF / info-leak. |
| ELF-01 | W4 | SECURITY | `sched/elf.c:48-92` | No `p_vaddr` range check; process PGDs share kernel upper-half by reference → crafted ELF can corrupt kernel page tables. |
| ABI-04 | W4 | SECURITY | `core/syscall_dispatch.c:151,166,176,251` | No capability checks: any process can kill any PID, steal focus, destroy windows, write any file. |
| EXT4-01 | W5 | BUG·MISSING | `fs/ext4.c:278-316`, `ext4.h:103` | Driver never reads `i_flags`; can't detect ext4 **extent-format** inodes → garbage reads on standard `mkfs.ext4` images (works only via `mkdisk`'s hand-built block-mapped inodes). |
| VFS-01 | W5 | WRONG-DESIGN | `fs/vfs.c`, `vfs.h` | "VFS" is a 59-line path-normaliser: no vnode, no mount table, no `file_ops`; FS syscalls call `ext4_*` directly. Primary blocker to Plan 9 / seL4-service goals. |
| EXT4-02 | W4 | SECURITY | `core/syscall_dispatch.c:176-199` | `FILE_WRITE` has no access control — any PID can overwrite any file, including `/init`. |
| EXT4-03 | W4 | DOC·BUG | `fs/ext4.c:3,234` | "Read-Only" header is false: `ext4_write_file` really persists to disk (capped 48 KB / 12 direct blocks). |

(Full W3 table and the W2/W1/W0 detail live in the per-subsystem docs; the W3 set is the
remaining actionable tier and is the basis for the issue batch — see §6.)

---

## 5. Corrections & reconciliations (review integrity)

Maintainer spot-checks of agent-delegated analysis corrected the following before anything
was published — recorded for transparency:

- **My own early errors, retracted:** (a) I claimed init's supervisor loop was "broken
  (blocking wait)" — wrong; `process_wait` is non-blocking, the loop is a valid poll.
  (b) I claimed `draw3d.c` "doesn't exist" — it exists but is **not compiled** (orphan).
- **Reconciled inference:** the amd64 4G crash I first attributed to MMIO-mapping range
  (AMMU-07) is actually **DRV-VIRTIO-01** (64-bit BAR truncation), confirmed by source.
  AMMU-07 downgraded to a lesser, separate MMIO-coverage limitation.
- **Agent overstatements caught & downgraded:** USR-INIT-01 "PID-reuse W3" → **W1**
  (`next_pid` is monotonic, no reuse). USR-MALLOC-01 calloc mechanism reworded (the
  `memset` is in-bounds; the *caller* overflows). LIB-MATH-01 "all 3D corrupted W3" →
  **W2 latent** (only consumer `draw3d.c` is not compiled).
- **New cross-cutting finding surfaced:** orphaned/uncompiled source files exist in-tree
  (`graphics/draw3d.c`, `bin/test_init.c`, the dead `user/sys/lib/syscall.S`) — dead code.
- **Retraction (post-fix, maintainer-corrected):** SCHED-UAF-01's *aarch64* crash trace (the
  `addr2line` hit at `process.c:937`) was a **false trace from a drafted-but-unapplied fix** and
  does not reproduce; the teardown use-after-free is **amd64-only** (§9 corrected accordingly).

Provenance: docs **01, 02, 09** hand-written by maintainer; **03–08** agent-generated and
maintainer spot-checked (top/critical findings verified against source; **07, 08** carry
explicit correction notes). Every finding cites `file:line`.

---

## 6. Per-subsystem index

| Doc | Subsystem | Notes |
|---|---|---|
| [01-mm](analysis/01-mm-memory-management.md) | PMM/VMM/buffer/kmalloc | maintainer |
| [02-boot-arch-hal](analysis/02-boot-arch-hal.md) | boot, platform, HAL, arch-MMU | maintainer |
| [03-arch-cpu-exceptions](analysis/03-arch-cpu-exceptions.md) | CPU/IDT/exceptions/syscall-entry/uaccess | agent, vetted |
| [04-drivers-irq](analysis/04-drivers-irq.md) | virtio/gpu/uart/gic/timer/pci/irq | agent, vetted (W5 here) |
| [05-fs](analysis/05-fs.md) | vfs/ext4/gpt | agent, vetted (2×W5) |
| [06-graphics](analysis/06-graphics.md) | compositor/font/gl/region | agent, vetted |
| [07-lib-headers](analysis/07-lib-headers.md) | kernel lib + ABI headers + registry | agent, vetted+corrected |
| [08-userland](analysis/08-userland.md) | init/shell/services/libs/apps | agent, vetted+corrected |
| [09-sched-process-ipc-abi](analysis/09-sched-process-ipc-abi.md) | scheduler/process/IPC/ABI | maintainer |

## 7. Issues (GitHub)

The W3+ actionable tier (**72 findings**) is filed as individual GitHub issues on
`olmox001/os1test-dev`, labeled by severity (`w3`/`w4`/`w5`), kind (`bug`, `security`,
`wrong-design`, `missing`, `stub`, `bad-impl`, `refine`, `perf`, `review-doc`) and
`area:*`, all tagged `code-review`. W0–W2 findings remain in the per-subsystem docs above.

- **Tracking epic:** [#19](https://github.com/olmox001/os1test-dev/issues/19)
- **Per-finding issues:** [#20–#91](https://github.com/olmox001/os1test-dev/issues?q=is%3Aissue+is%3Aopen+label%3Acode-review) (72)
- **Cross-cutting epics:** #92 Memory & address-space · #93 ABI & capabilities ·
  #94 amd64 boot/4GB · #95 Service isolation (seL4/Plan 9) · #96 SMP correctness
- Filter examples: `gh issue list --label code-review`, `--label w5`, `--label area:fs`.

Each issue body carries the `file:line` location, the finding text (maintainer-corrected),
and a pointer to its subsystem doc + this index. They are the unit of work for the
delegated fix phase (Phase 3).

## 8. Phase 3 — fixes landed (branch `comprehensive-review`)

Each verified by build (both arches) + headless QEMU runtime, committed separately.
The boot/crash fixes were delegated one-agent-at-a-time and maintainer-verified before
commit. For the W3 issue-tier rows: #80/#62 were agent self-verified under the authorized
build+boot workflow; #98/#59/#63/#42/#70/#50/#74 (this session) were each verified by an
independent build on both arches + headless boot on both arches before commit (several were
implemented by delegated sub-agents that did not commit), **pending maintainer review**.

| Commit | Fix | Issue |
|---|---|---|
| `0c5dc0a` | amd64 read full 64-bit PCI BAR (virtio.c + hal.c) + `arch_vmm_map_device` | **#44** (W5) ✅ |
| `89c3a52` | amd64 clone high device-MMIO PML4 entries into process PGDs (fixes ≥4G `0xc0…` fault) | part of #94 ✅ |
| `fedd9e2` | amd64 detect PVH via `hvm_start_info.magic` → real memory map (up to 4GB+) | **#28, #29** ✅ |
| `8b03255` | amd64 `*(.lbss*)`→`.bss` so PMM metadata no longer overlaps `cpu_data` (SMP `current_task` page-fault) | runtime-discovered ✅ |
| `b3ea74f` | aarch64 real DTB via `-dtb`/raw `kernel.bin` (FDT works, `x0` set) + SMP fallback cap 64→8; `-m 5G` default both arches | runtime-discovered ✅ |
| `3f9f81f` | userland `calloc(nmemb,size)` integer-overflow guard (pre-multiply `size > SIZE_MAX/nmemb` check) | **#80** (W3) ✅ |
| `c6c268a` | bound user-supplied I/O buffer size at 16 MiB before `kmalloc` (FILE_WRITE/READ/LIST_DIR, cases 251/252/254) | **#62** (W3) ✅ |
| `6fd1b47` | graphics: capture the IPC message + close decision under `compositor_lock`, then **release it before** `kernel_ipc_send`/`process_terminate` in `compositor_handle_click` → resolves the AB-BA freeze on window-close/kill | **#100** (W4) ⚠️ freeze only |
| `848d6c8` | sched: clamp `sys_getprocs` `max_count` to `MAX_PROCESSES` before `kmalloc` (unchecked multiply + unbounded alloc) | **#98** (W3) ✅ |
| `8e01551` | fs: `struct ext4_group_desc` `padding[14]`→`[12]` (34→32 B, matches the on-disk GDT entry; stops multi-group write corruption) | **#59** (W3) ✅ |
| `7839076` | fs: abort to the MBR parser on partition-entry CRC mismatch in `gpt_init` (mirrors the header-CRC fallback) | **#63** (W3) ✅ |
| `0e6a790` | arch: null-guard `current_process` in aarch64 `arch_copy_to_user` (mirrors `arch_copy_from_user`) | **#42** (W3) ✅ |
| `d02038d` | graphics: floor `graphics_font_height()` to the built-in default when `ascent+descent<=0` (compositor div-by-zero) | **#70** (W3) ✅ |
| `392d7fc` | drivers: check `pmm_alloc_pages/_page` returns in `virtio_input` `init_device` (NULL-deref → graceful bail) | **#50** (W3) ✅ |
| `94c936c` | lib: `ktest` counts real pass/fail via a `ktest_test_failed` flag set by `KASSERT` (was always N PASS / 0 FAIL) | **#74** (W3) ✅ |
| `3296ce1` | amd64: window-close triple-fault root-cause fixes + **AP adoption of the live `kernel_pgd` in `arch_cpu_init`** (ARCH-AMD64-APPGD-01; platform.c untouched); toolchain pin script | **#101, #102** ✅ |
| `db4eb4c` | sched + aarch64: `arch_cpu_switch_context` loads the shared `kernel_pgd` when `page_table == NULL` on **both arches** (SCHED-UAF-01 idle/kernel-thread residual); removes the redundant-and-buggy `hal_vmm_set_pgd` block in `schedule()` | SCHED-UAF-01 ✅ |
| `3509a4f` | follow-up review fixes: IRQ-masked deferred-free drain (SCHED-UAF-02), reaped-corpse fallback guard (SCHED-UAF-03), stale-comment rewrite, APPGD reuse of `arch_vmm_set_pgd`, aarch64 redundant ISB, toolchain-script probe/Apple-Silicon guards, ignore `tools/mkdisk` — see [analysis/10-addendum-2026-06-11](analysis/10-addendum-2026-06-11-sched-uaf-followup.md) | addendum 10 ✅ |
| `12843d4` | **Phase A** steps 5-7: fault-safe reporting (`fault_printf` lock-free, `uart_putc_emergency`, MSR/MPIDR `arch_cpu_info_fault_safe`, per-CPU `in_fault` recursion guard, fault-context `panic()` mode) | Phase A ✅ |
| `61c871b` | **Phase A** steps 3-4: dedicated fault stacks — amd64 IST1 (#PF/#GP) + IST2 (#DF) via TSS; aarch64 per-CPU EL1 abort stack with parked-SP + probe copy-back.  Verified by fault injection: nested #PF = one clean line + halt, 0 cpu_resets (was triple-fault) | Phase A ✅ |
| `d6f03e9` | **Phase A** steps 8-10: user-vs-kernel isolation — generic `fault_handle_user_or_panic`; amd64 user faults terminate+schedule (**EXC-AMD64-02 / #36**); uaccess windows flagged on both arches (**CPU-AARCH64-01**, **SYS-AARCH64-02**).  Verified: `crash` from the shell terminates PID, shell survives, `counter` runs after — both arches | **#36** + 2 ✅ |
| `ac4d76f` | **Phase A** steps 11-12: symbolized backtrace — fp walker + kallsyms-style `.ksyms` blob (two-pass link, survives aarch64 `objcopy -O binary`).  Verified: `kernel_main+0x3e3` matches addr2line; live symbols from raw kernel.bin | Phase A ✅ |
| `6bed3cf` | **Phase A** step 13: total aarch64 vector coverage — all 8 `b .` silent-hang vectors (FIQ, EL0-AArch32) now report + terminate/panic | Phase A ✅ |
| `b9aad52` | sched: **auto-reap zombies** in `schedule()` (SCHED-03 mitigated — shell never WAITs; doom/demo3d leaked slot+stack+PGD per exit); `process_wait` → pure reporter (double-free hazard); parked-sleeper corpse leak in `process_terminate` closed.  Verified: `crash` → `ps` shows no zombie, next spawn reuses the freed slot — both arches | SCHED-03 ✅ |
| `dc8a3db` | virtio-blk: **serialise requests** (driver spinlock — torn-descriptor SMP race was the `Read failed status=1` / doom `W_ReadLump` failure in the external traces); static DMA targets (DRV-VIRTIO-03); pre-publish `used->idx` snapshot (DRV-VIRTIO-04).  Verified: aarch64 `counter`+`doom` → full WAD load, 0 failures | DRV-VIRTIO-03/04 ✅ |
| `9bf27af` | **Phase A step 14**: PIT halted after LAPIC calibration → vector 32 single-source (EXC-AMD64-03; LINT0 stays ExtINT — it carries PCI INTx); dead amd64 probe block + flags removed (EXC-AMD64-01) | EXC-AMD64-01/03 ✅ |
| `2212423` | **Phase A step 15**: `irq_handlers[]` lock (IRQ-02/#55, pair copied under lock); chip-owned EOI via `irq_chip_end` → `pic_chip_end` does LAPIC+8259 (IRQ-01/#47); spurious 8259 IRQ7/IRQ15 + LAPIC 0xFF filtered before dispatch (kills the "Unhandled interrupt 47" flood AND the wrong slave EOI it sent) | **#47, #55** ✅ |
| `166887a` | sched: **SCHED-IRQ-01** — `schedule()` masks IRQs itself before `get_cpu_info()`; no-switch exits restore, switch exit returns masked (IRET/ERET loads next frame's flags).  Closes the nested-schedule class for every syscall entry state | SCHED-IRQ-01 ✅ |
| `db503a3` | gui/input: focus reset on window destroy → top-most surviving window by Z-order (was hardcoded PID 7); per-keystroke IRQ-context log → pr_debug | trace triage ✅ |
| `f37d137` | sched/init: **deterministic service respawn** — init treats `wait()==-2` as "child gone" (was racing the auto-reaper: respawn worked on aarch64 runs, not amd64); `process_terminate` parked check generalised to ALL sleeping victims (IPC sleepers were freed immediately without a `current_task` check — never a waitable corpse + SCHED-UAF-family hazard).  Verified both arches: `exit`→respawn, external `kill` of sleeping notify→respawn | user-reported ✅ |
| `e64756e` | fs: **Phase B1** — real VFS layer (`fs_ops` provider contract, mount table, partition probing) with ext4 registered behind it; zero `ext4_*` calls outside `kernel/fs/`.  ext4 gains extent-tree reads (any depth, holes/unwritten as zeros) and mount-time feature enforcement (unknown INCOMPAT/64bit/multi-group rejected loudly; unknown RO_COMPAT → read-only mount).  Verified: both arches on legacy AND extents images (shell/doom/counter from extent inodes; 11 MB WAD = depth-1/2-leaf tree); poisoned-INCOMPAT image refused | VFS-01 #64 ✅ EXT4-01 #56 ✅ EXT4-06/08/10/12 ✅ |
| `db04684` | tools: mkdisk writes extent inodes by default (`--legacy` opt-out, `MKDISK_LAYOUT`); extent cap 8 blocks on big files forces depth-1 trees so the kernel's index-node walk stays a tested path | EXT4-01 test vector ✅ |
| `40f580f` | fs: **B1 residuals** — write path extended (extent depth-0 append, legacy single-indirect ≈4.2 MB; check-before-alloc so rejects never leak bitmap bits) + per-loop interior-block cache; writetest rewritten as a real 3-case verifier (was targeting a path that no longer exists) | EXT4-05 partial ✅ EXT4-11 ✅ |
| `f4ad8fa` | mm: **Phase B2** — full process teardown: vmm_destroy_pgd frees user frames (PTE_USER leaves) + every private table page under value-equality ownership rules; dead amd64 arch teardown removed; aarch64 PUD[1] PMD deep-copied per process (cross-process 0x7ffff000 aliasing fixed).  Verified: spawn/exit cycles return PMM free count to the identical value on both arches | MM-VMM-04 #24 ✅ AMMU-03 #35 ✅ |
| `b745a74` | mm: **W^X both arches** — text RX / rodata RO+NX / all other RAM RW+NX via vmm_map_ram_wx (new `__erodata` symbol); EFER.NXE on BSP+APs; amd64 flag translation fixed (opt-in RW — user RO segments were writable; NX honoured; PCD/PWT pass-through); user stack+heap PAGE_USER_DATA.  Proven by `nxtest` (stack exec → fault, process killed, shell survives) | MM-VMM-01 #22 ✅ AMMU-01 #33 ✅ ELF-02 #87 ✅ |
| `0b9f6d5` | mm: multi-page PMM allocations cache-cleaned + fenced like the single-page path (pmm_alloc_aligned inherits) — DMA-safe | MM-PMM-02 #21 ✅ |
| `29bb092` | fs: vfs_resolve_path guards `current_process`; kernel-context relative paths resolve from `/` | VFS-02 #65 ✅ |
| `508c734` | mm: MAX_BUFFERS is a hard cap — slot reserved in total_buffers under buffer_lock before allocating; full cache → evict, buffer_sync + re-evict, then loud NULL refusal; all failure paths release the slot | MM-BUF-01 #26 ✅ |
| `67ff898` | mm: kmalloc small-object pool grows by 4 MB PMM chunks on exhaustion (grow-race donates the losing chunk back); initial footprint 32→4 MB; ktest_run_all moved after MM bring-up; new `test_kmalloc_growth` proves a chunk-boundary crossing.  Cross-bucket reuse / return-to-PMM remains MM-KM-02 (W2) | MM-KM-01 #27 ✅ |
| `06f017a` | mm: **cross-CPU TLB shootdown contract** (`arch_tlb_shootdown_va/all`) — aarch64 satisfied in hardware (IS TLBI broadcast; stale "local-only" comments corrected), amd64 = LAPIC IPI round (vector 0xFD, ack bitmask, bounded wait, lock-waiters service in-flight rounds) wired into arch_vmm_unmap + vmm_destroy_pgd; bonus: amd64 panic-halt IPI (vector 0xFE — `send_ipi_all` was NULL, panic never stopped peer CPUs) | MM-VMM-05 #25 ✅ AMMU-08 ✅ |
| `834e347` | mm: **real arch_vmm_protect both arches** (was amd64 stub / aarch64 missing): 4KB-precise attribute rewrite, large-page split, SMP shootdown; + two latent walker bugs — aarch64 block-split level off-by-one ("split" 2MB block → EMPTY L3 table, 511 pages silently unmapped) and block-blind arch_vmm_get_physical on both arches; new `test_vmm_protect` ktest (RW→RO→RW by PTE readback on the live kernel PGD) | AMMU-02 #34 ✅ |
| `cf8fca1` | mm: all page-table walkers route PTE derefs through `phys_to_virt()` / stores through `virt_to_phys()` — identity-map assumption centralized in vmm.h (single starting point for the future higher-half migration) | MM-VMM-02 #23 ✅ (walker half; migration deferred) |

Rows `3f9f81f` through `94c936c` are the **W3 issue-tier** fix phase — small, scoped, additive
correctness/security hardening on the issue backlog, distinct from the boot/crash fixes above.
Each was verified by build (both arches) + boot (no regression). Where the fixed path is not
exercised at boot (the capped/overflow guards #80/#62/#98, the multi-group write #59, the
CRC-mismatch fallback #63, the alloc-failure bails #50, the malformed-font floor #70, the
aarch64 null guard #42), the standard is build + no-regression + correct-by-inspection.
LIB-KTEST-01 (#74) additionally had its FAIL path proven by a throwaway broken assertion
(→ 2 PASSED / 1 FAILED) before reverting.

`6fd1b47` (GFX-COMP-13, **user-reported W4**) is a real SMP lock-ordering fix — verified by
build (both arches) + boot. **It resolves only the freeze / AB-BA deadlock**; the companion
zombie/no-reap symptom (when `process_terminate` runs from mouse-IRQ context) is a **separate,
still-open** fix tracked via SCHED-03, and the underlying compositor↔sched coupling stays open
as GFX-COMP-03 (#69).

**Verified runtime status now:** amd64 boots clean at `-m 3G / 5G / 8G` (detects 6–9 GB,
virtio-blk + Ext4, 4 SMP cores, no faults); aarch64 FDT-driven (real RAM + CPU count),
boots to the TTY shell.

**IPC → 64-bit: already satisfied (verified).** `struct ipc_message`
(`include/api/posix_types.h`) already carries `uint64_t data1; uint64_t data2; char
payload[64];` — present since `main` — and all producers/consumers use 64-bit
(keyboard packs `(uint64_t)code<<16`, `lib.c` reads `data1>>16`, `ipc_send`/`ipc_recv`
use 64-bit). Exercised at runtime every boot (keyboard input + `notify()`). No change needed.

**Remaining (open, future sessions — multi-step refactors, not concludable in one short pass):**
amd64 ACPI-MADT CPU count (ARCH-01), real PCI/ACPI init (ARCH-02); async block I/O
(DRV-VIRTIO-08 — reads still busy-wait, now with IRQs masked under the blk lock);
blocking `wait()` + exit-status collection (needs SCHED-06 parent/child links);
legacy virtio-pci transport hang (addendum 11 §2.5); the kernel/userland
higher-half **addressing rework** (the central PA==VA invariant); W^X (MM-VMM-01/AMMU-01);
and re-commenting the headers + `.S` files reverted in Phase 2 (all C sources are
commented and committed).  **All Phase A residuals are now closed**: step 14
(`9bf27af`), step 15 (`2212423`), SCHED-IRQ-01 (`166887a`); EXC-AMD64-02 (#36) fixed in
`d6f03e9`.  The external boot-trace triage (zombie leak, virtio-blk race, focus reset,
spurious-IRQ flood — what was confirmed, what was misread) is
[analysis/11-addendum-2026-06-11](analysis/11-addendum-2026-06-11-external-trace-triage.md).
The Phase B microphase plan + fresh-session handoff (build/test playbook, B1–B6
scope and acceptance criteria) is [docs/PHASE-B-PLAN.md](../PHASE-B-PLAN.md).

## 9. amd64 runtime crashes — root-caused (both FIXED)

Two amd64 runtime defects were precisely root-caused via headless QEMU + interactive
`make run` (serial capture, an in-#PF-handler PGD walk). **Both are now fixed:**
SCHED-UAF-01 (below, verified by build + boot + a kill-stress on both arches; the
idle/kernel-thread residual closed on both arches in `db4eb4c`) and
ARCH-AMD64-APPGD-01 (closed in `3296ce1`, see status under its entry). The follow-up
recall review of that fix branch — what it verified, the two new SCHED-UAF-0x findings
it fixed (`3509a4f`), and the residual/undetected-issues catalog — is
[analysis/10-addendum-2026-06-11](analysis/10-addendum-2026-06-11-sched-uaf-followup.md).

**SCHED-UAF-01 — process-teardown use-after-free; crash on window-close (amd64). [FIXED]**
*Mechanism:* closing a window (compositor close button → `process_terminate`) could leave the
terminated process **still linked in a per-CPU runqueue** when its `struct process` page was
freed (and PMM-poisoned `0xCC`); `schedule()`'s O(1) pick and the **work-stealing** path then
dereferenced the freed node (`next->priority` on a poisoned struct) → GPF → triple-fault →
reboot on amd64. *Root cause:* `process_terminate` mutated `state`/`run_list`/`on_cpu` under
the **global** `sched_lock`, while `schedule()` mutates them under the **per-CPU** `sched_lock`
— not mutually exclusive, so a victim could be re-enqueued (resurrected), left DEAD-but-queued,
or freed while still referenced.
*Fix (landed in `kernel/sched/process.c`):* the scheduler is now the sole owner of runqueue
membership and of freeing runnable processes. `process_terminate` marks a RUNNING/READY victim
**sticky `PROC_DEAD` under the owning CPU's `sched_lock`** (re-validating `on_cpu`) and never
frees or dequeues it; the scheduler reaps it — a running victim via the `prev==DEAD` path, a
queued victim via a new `pick==DEAD` path — both feeding a per-CPU **reap stack** (chained via
the unused legacy `process.next`) drained at the top of `schedule()` outside the lock.
`__enqueue_task` refuses `DEAD/ZOMBIE` (no resurrection) and work-stealing skips corpses.
Sleeping/created victims are freed immediately under the global lock; self-exit stays ZOMBIE.
*Verified:* clean build + boot on both arches, plus a kill-stress driving `process_terminate`
of RUNNING `demo3d` victims across `-smp 4` (50× to completion on aarch64, 11× on amd64) with
**no crash / PAGE FAULT / triple-fault / corrupt run_list**. Interactive close-button
(mouse-IRQ) confirmation pending maintainer. *Related (still open):* the compositor calls
`process_terminate` from mouse-IRQ context — design coupling tracked as SCHED-03 / GFX-COMP-03.

**ARCH-AMD64-APPGD-01 — APs run on the stale boot PML4 → high device-MMIO faults. [FIXED in `3296ce1`]**
*Fix as landed:* each AP adopts the live dynamic `kernel_pgd` at the end of `arch_cpu_init`
(amd64 HAL, via `arch_vmm_set_pgd`; no-op on the BSP's early call where `kernel_pgd` is still
NULL). This is safe because `init_memory()` (→ `vmm_dynamic_remap`) runs before
`arch_smp_init()` (`main.c:107` vs `:123`) and APs enable IRQs only after `cpu_init()` —
so no device IRQ can be taken before adoption. `platform.c` / the trampoline are untouched
(the original fix direction below was superseded for that reason). Original root-cause record:
`start.S` has `kernel_pgd_phys: .quad boot_pml4`, and `arch_cpu_wake_secondary`
(`platform.c` ~:459) launches APs with CR3 = `kernel_pgd_phys`. But `arch_vmm_init_hw` /
`vmm_dynamic_remap` switch the BSP to a **new dynamic `kernel_pgd`** (the one
`arch_vmm_map_device` later populates with the >4GB virtio BARs at `PML4[1]`);
`kernel_pgd_phys` is never updated. APs therefore run on **`boot_pml4`**, whose `PML4[1]` is
absent → a device IRQ touching high MMIO from an AP/idle (e.g. the virtio-input ISR at
`0xc000005000`) page-faults. *Confirmed:* an in-#PF PGD walk showed `cr3=boot_pml4
(≠ kernel_pgd)`, `PML4[1]=0` while `kernel_pgd PML4[1]` is present. *Fix direction:* set the
AP trampoline CR3 to the current dynamic `kernel_pgd` (`(uint32_t)(uintptr_t)kernel_pgd` —
identity-mapped, VA==PA) in `arch_cpu_wake_secondary`, or update `kernel_pgd_phys` after the
dynamic remap. (Verified no-regression in isolation — 4 APs still come online — but it was
bundled with the reverted SCHED-UAF work, so it is not committed.)
