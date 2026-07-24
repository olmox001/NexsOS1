# PLAN 2026-07-23 — Hardening, formal diagnostics, portable SDK, Rust core

Maintainer directive, 2026-07-23.  Seven programmes (A–E from the first
directive, **F–G added 2026-07-23 second directive**), executed in order, each
one task at a time: **study the surface → write the plan → apply it → test on
both architectures → refine → next**.  Both arches build and boot at every task
boundary; the maintainer drives `make run` interactively, this plan's own gate
is the headless boot plus the build.

**COMMIT POLICY (maintainer, 2026-07-23):** from the second directive onward,
NO commit without the maintainer's explicit authorization.  Work is staged and
verified; the maintainer authorises the commit.

Programmes F and G are the "make the kernel actually usable" directive:
on-disk PERSISTENCE with a real partition model + a first-boot INSTALLER
(Programme F), on top of REAL storage & device drivers (Programme G).  They
EXTEND existing in-tree plans rather than replacing them — `docs/MICROSCOPE-
RELEASE-STORAGE.md` (block contract done, tmpfs/xfs/memory-drivers open),
`docs/PIANO-DRIVER-MATURITY.md` (Fase 2 = runtime hotplug/plug-and-play, USB
stack partly landed), `docs/FUTURE_DRIVER_EXPANSION_PLAN.md` (NVMe/AHCI as block
providers, partition-table parser).  Method stays ASTRA: every device/format is
a provider behind a contract; `arch/` holds only ISA/boot glue.

This file is the task list.  It is updated IN PLACE as tasks complete, and any
defect or open point found while doing something else is added to §H rather than
fixed silently — unless it is a one-line fix, which is applied on the spot and
recorded in the same table.

Naming: defects use the project's existing internal ID convention
(`SUBSYS-NN`, e.g. `ABI-04`, `SCHED-05`, `UACC-AMD64-02`), NOT CVE numbers.
These are not published vulnerabilities and giving them CVE identifiers would
make them look like something they are not.  The catalog in §A is the
equivalent artefact: one row per defect, with severity, status and owning task.

---

## Programme A — Security & correctness audit  (IN PROGRESS)

Surface, in the order the maintainer named it.  Every file gets a verdict, and
every EXISTING open marker gets CONFIRMED / OBSOLETE / RECLASSIFIED — a stale
"known issue" is worse than an unknown one, because it consumes the attention
that would find the real one.

| task | surface | state |
|---|---|---|
| A1 | `kernel/core/syscall_dispatch.c` + both arch entry paths (`arch/aarch64/cpu/syscall.c`, `arch/amd64/cpu/syscall.c`), user stubs | done |
| A2 | `kernel/core/object.c` — handle table, capability acquisition, object ctl | done |
| A3 | `kernel/lib/registry.c` — key parsing, ACL, virtual routing | done |
| A4 | capabilities: `include/abi/caps.h`, level presets, `level_for_path`, creator clamp | done |
| A5 | `kernel/sched/process.c` — spawn, kill, subtree, stop/cont, wait, reap | done |
| A6 | windows: `compositor_*` capability surface + `OS1_NS_WIN` acquisition | done |
| A7 | `user/sys/bin/nxinit.c` + the supervised-service model | done |
| A8 | userland abstraction: `user/sys/lib/lib.c`, `include/api/*` | done |
| A9 | Verdict pass over all 108 pre-existing markers | done |

### A-catalog — findings
Severity: **S1** exploitable/crash from unprivileged userland · **S2** privilege
or isolation defect requiring some privilege · **S3** correctness/robustness ·
**S4** hygiene.

| id | sev | surface | finding | status |
|---|---|---|---|---|
| ABI-06 | S2 | process.c | `process_kill_subtree()` killed a MACHINE root's whole subtree while sparing the root | **FIXED** b7e5668 |
| GFX-COMP-RESERVE-02 | S1 | compositor.c | `compositor_lock` → `sched_lock` AB-BA + UAF on the returned `struct process *` | **FIXED** b7e5668 |
| ABI-08 | S1 | syscall_dispatch.c | `SYS_WAIT` arg1 written without the one-arg stub zeroing the register | **FIXED** b7e5668 |
| **GFX-WIN-WRITE-01** | **S1** | syscall_dispatch.c | `SYS_WINDOW_WRITE` gated only by CAP_WINDOW (the WEAKEST cap — guests hold it), no owner/ctty check: least-privileged process could write text into ANY window (id from the ungated SYS_WINDOW_ENUM). UI-spoofing against shell/dock/notify. | **FIXED** (this batch) — owner/ctty/machine check, mirrors DRAW/BLIT/SET_FLAGS |
| **PORTCAP-01** | **S2** | object.c | `sys_port_send_caps` rolled back installed handles only on install-loop failure; on `-EPIPE`/`-EAGAIN` (queue full) the transferred handles stayed in the receiver. `-EAGAIN` is retryable, so a peer keeping a service's port queue full leaks a fresh handle set into the service's table per retry → capability-table exhaustion DoS. | **FIXED** (this batch) — unwind on any `ret < 0` |
| **SPAWN-LVL-01** | **S3** | syscall_dispatch.c | `level_for_path()` prefix-matches the RAW path but the VFS resolves `..`, so `/sys/bin/../../home/x` takes the ROOT preset. Not an escalation (creator clamp drags it back to USER), but "safe only because a distant second check covers it". | **FIXED** (this batch) — reject `..` components in a spawn path |
| **CAP-POLICY-01** | **S2 (design)** | process.c `level_ceiling[]` | `PLVL_USER = CAP_ALL`. Every ordinary process holds **every** capability (SPAWN, FS_WRITE, IPC_ANY, WINDOW, REG_WRITE). The capability MECHANISM is enforced everywhere, but the POLICY is "everyone gets everything except guest", so least-privilege is not realised — this is the maintainer's "problem with capabilities". Also the root of USR-SEC-01 (any process may overwrite `srv.notify_pid`). | OPEN → B2 (needs per-app/per-service caps, ASTRA §7.11 Q5 — cannot be narrowed unilaterally without breaking every app) |
| PROC-REF-01 | S2 | object.c, process.c | a `struct process *` outlives the lock that validated it (`sys_port_send_caps` `rcv`, `process_redirect_child_fd_from`) | OPEN → B1 |

### A — verdicts on surface (no defect = why)
- **A1 dispatcher / arch entry / stubs**: user-pointer copy discipline is
  UNIFORM — every syscall taking a user pointer either copies through
  `arch_copy_*` or hands to a `sys_*` that does (mechanically verified, table
  above).  `argc`/`nredir`/`nfds` all bounded before any `n * sizeof` (no
  size-overflow).  Both arch entry paths mechanically checked stub-arity vs
  dispatcher-arg-use (done in the prior session); only `_sys_spawn`/`_sys_wait`
  needed the scratch-register zeroing, both done.  **GFX-WIN-WRITE-01** was the
  one real hole. Verdict: clean after this batch.
- **A2 object.c**: handle indices bounded at every entry (`< 0 || >= NPROC_HANDLES`);
  acquisition ACLs present per namespace; ctl owner/privilege checks present.
  **PORTCAP-01** the real hole; **PROC-REF-01** the lifetime one.
- **A3 registry.c**: writes gated by CAP_REG_WRITE; virtual keys routed before
  the ACL with their own per-process rule; strict copy for keys/values (no
  silent truncation).  `reg_proc_split` pid parse is unbounded (`*10+`) but the
  pid is only used as a table lookup that fails for a wrapped value → S4, noted
  §H.  Verdict: sound.
- **A4 capabilities**: the mechanism is correct and enforced; the POLICY is the
  finding (**CAP-POLICY-01**).  Creator clamp is monotone and correct.
- **A5 process.c**: spawn/kill/wait/reap reviewed with the prior session's
  fixes; **PROC-REF-01** the remaining lifetime gap.
- **A6 windows**: `OS1_NS_WIN` acquisition ACL correct (WRITE/DESTROY to
  another pid's window is machine/root only); dispatcher owner checks now
  complete after GFX-WIN-WRITE-01.
- **A7 nxinit**: supervised-service model reviewed in the prior session
  (backoff, port-claim retry).  USR-SEC-01 is a symptom of CAP-POLICY-01.
- **A8 userland abstraction**: declared-vs-implemented libc audit is EMPTY
  (prior session); POSIX layer is a mapping over `OS1_*`, no parallel impl.
- **A9 markers**: 108 project-code defect IDs catalogued; the audit CONFIRMED
  the security-relevant ones above and reclassified USR-SEC-01 as a symptom of
  CAP-POLICY-01.  The MM-*/ARCH-*/AMMU-*/BOOT-* families are correctness/hardening
  notes outside this audit's named surface — swept into Programme B backlog, not
  lost.

---

## Programme B — Resolve the catalog, one micro-phase per defect

Each defect gets its own micro-phase: study → fix → build both arches →
headless boot both arches → stage.  Ordered by severity, then by whether the
fix unblocks another.  Populated from §A; B1 is already known.

| task | defect | state |
|---|---|---|
| B1 | PROC-REF-01 | **DONE** — resolved by holding sched_lock across lookup+use instead of adding a process-wide refcount.  `sched_lock` exposed in sched.h (it pins the pool); three sites fixed: `sys_cap_grant` (also closes OBJ-GRANT-REAP), `sys_port_send_caps` rcv, `dispatch_spawn` src.  Order sched→object→kmalloc; handle-table allocation refused under sched_lock (a real target always has one).  Both arches boot clean. |
| B2 | CAP-POLICY-01 — reframed after the maintainer's correction: this is NOT per-app bitmasks, it is the **4-LEVEL abstraction** (machine/root/user/guest) that must stratify.  The mask half AND the per-namespace ACL half together.  Split into B2.0–B2.4 below. | in progress |

### B2 — the level model, corrected understanding
The maintainer corrected two approximations: (1) apps reason in the 4 LEVELS
(machine/root/user/guest), mapped over caps, not raw bits — the POSIX
abstraction sits on that; (2) that mapping exists at KERNEL level
(`struct process.level`, `proc_is_privileged`/`proc_is_machine`,
`caps_for_level` (was `level_ceiling`), `level_for_path`, `registry_caller_owner`),
mirrored in userland by `nxperm.h`.  The stated target semantics:
- **machine** — full authority (it IS the check).
- **root** — full, EXCEPT writing `/sys/bin` and `/system` (VFS ACL, not mask).
- **user** — restructured: home moves `/home` → `/mnt/usr1/home` (per-user
  partition prep), per-service manifest presets LATER.
- **guest** — windows only (the one level whose MASK is genuinely narrow).
- **The POSIX abstraction must work over our VFS.**

| task | item | state |
|---|---|---|
| B2.0 | Loss analysis: is the mask/ACL regressed vs the reference or a release? | **DONE** — nothing lost.  `level_ceiling[PLVL_USER]` = CAP_ALL since the model's first commit `24fab00`; VFS write-ACL byte-identical across `884b7f3` and every release tag; `/system` and `/mnt/usr` never existed.  The stratification is a FORWARD upgrade, not a repair. |
| USR-SEC-01 | system-owned registry key writable by anyone (owner-0 hole) | **DONE** `bf92c76` — deny is now "a non-system caller writes only a key it owns", matching registry_del which was already correct. |
| B2.1 | separate the level→mask policy from the scheduler into caps | **DONE** `d93b6fc` — `caps_for_level()` in `include/abi/caps.h`, kernel + nxperm derive from it, drift bug removed. |
| B2.2 | `/system` machine-only in the VFS write-ACL (root refused, like /sys/bin) | **DONE** `5b8b77d` — path guarded before it is populated (harmless; nothing writes it). |
| B2.3 | user home `/home` → `/mnt/usr1/home` | **BLOCKED on a disk-layout decision** — see below.  84 hardcoded `/home` references across userland (icons, shell cd, nxexec `~`, history, image paths); rootfs is a single ext4 partition with no `/mnt`; the VFS supports multiple mounts but roots a single one; no symlink support.  This is the maintainer's "per-user partition = future block".  Options to decide: (a) separate mounted partition at `/mnt/usr1` vs (b) a directory in the same rootfs; and how the 84 hardcodes migrate (consult `HOME` everywhere vs keep `/home` as a compat alias). |
| B2.4 | per-service capability manifest/preset (ASTRA §7.11 Q5) | pending — after B2.3.  Mechanism: a per-path/per-app cap mask layered over `caps_for_level`, default-permissive, tightened one service at a time. |

---

## Programme C — Formal diagnostics

Goal: make any function in the kernel accurately and readably diagnosable, and
make unlikely logical/semantic errors *catchable* rather than merely possible.

- **C1 — `printk` audit.** `kernel/lib/printk.c` (LIB-PRINTK-01,
  LIB-VSNPRINTF-02 open here): levels, ratelimiting, per-subsystem filtering,
  and whether the UART path is safe from IRQ and from a spinlock-held context.
- **C2 — the UART channel is separate from the graphical shell.** Establish it
  formally: a diagnostic channel that survives a dead compositor, with its own
  contract, not "printk happens to reach the serial port".
- **C3 — two error systems and the POSIX link.** kernel `pr_err`/panic/fault
  path vs userland `OS1_report_error` → notify severities vs POSIX `errno`.
  Today these meet ad hoc.  One documented mapping, one seam each direction.
- **C4 — assertion & invariant layer.** A `KASSERT`-class facility with the
  context the fault path already knows how to symbolise, so an invariant break
  reports WHERE and WHAT, not just a fault address.
- **C5 — structured subsystem tracing** gated per subsystem, off by default,
  so a diagnosis is a flag rather than a rebuild with temporary `pr_info`s
  (the plan records that pattern being used repeatedly).

## Programme D — Portable SDK, dynamic linking, kernel/userland separation

The end state: userland binaries do not statically carry the libc, the kernel's
API and the userland's API are separate artefacts, and apps are built against
an SDK rather than against the kernel tree.

- **D1 — finish Phase 10a first.** `lib.c` still `#include`s three kernel `.c`
  sources (USR-LIB-01).  Dynamic linking cannot decouple what is compiled in.
- **D2 — ELF dynamic loading in the kernel loader**: `PT_DYNAMIC`, `PT_INTERP`,
  relocations, a runtime loader process.
- **D3 — `libos1.so`**: the libc as a shared object, one copy in memory.
- **D4 — the SDK tree**: headers + link stubs + a sysroot, versioned, with
  nothing from `kernel/` reachable.  The layering gate (already in tree)
  becomes the SDK's admission test.
- **D5 — port the userland to the SDK**, app by app, keeping static linking
  available until the last one moves.

## Programme E — Rust in the kernel core

- **E1 — toolchain**: replace/augment the GCC cross toolchains; pick the Rust
  target triples (`aarch64-unknown-none-softfloat`, `x86_64-unknown-none`),
  decide on `core`/`alloc` and build integration with the existing Makefile.
- **E2 — kernel Rust semantics through the HAL.** Before any module is
  rewritten: panic handler, allocator shim onto `kmalloc`, the spinlock and
  IRQ-state primitives, `unsafe` boundaries for MMIO, and the calling
  convention on both arches.  This is HAL-0's successor and depends on it —
  two architectures that implement the same primitive differently cannot be
  given one Rust abstraction.
- **E3 — a first module**, chosen for being self-contained and verifiable
  rather than for being important.
- **E4 — the delicate modules**, one at a time, each with the C version kept
  until the Rust one passes the same tests.

---

## Programme F — On-disk persistence, partition model, first-boot installer

> Maintainer directive 2026-07-23 (second).  This is the programme that makes
> the system **actually usable**: persistent on real disk, installed once, then
> booting as a named user rather than as root.

### F0 — VERIFIED CURRENT STATE (real files, no assumptions)
Everything below was read out of the tree, not inferred:

| fact | evidence |
|---|---|
| The block layer is already a CONTRACT with one active backend | `kernel/drivers/block/block.c` — `block_register()`, `block_read/write()` route to `static const struct block_dev *active` |
| virtio-blk registers, then **ramdisk OVERRIDES it if a boot module exists** | `kernel/main.c:236-240` — `virtio_blk_init(); ramdisk_init();` |
| ramdisk writes are **volatile by construction** | `kernel/drivers/block/ramdisk.c` — `memcpy(disk, buf, len); /* RAM-backed: writes are volatile */` |
| ext4 writes reach the backend, so **persistence already works on the virtio-blk path** | `kernel/fs/ext4.c:90,252` → `block_write()`; `make run` attaches `disk.img` as virtio-blk |
| The ISO/release path is the RAM one ("loaded next to the kernel") | `Makefile:840` `module2 /boot/disk.img diskimg`; module reserved as `MEM_REGION_RESERVED` before PMM (`kernel/main.c:199-211`) |
| `mkdisk` builds a **single** GPT partition, hand-rolled single-group ext4 | `tools/mkdisk.c` — `MIN_PARTITION_BLOCKS (432 MiB)`, ~1014-inode cap, `plan_partition_blocks()` |
| The VFS supports MULTIPLE mounts but roots exactly one | `kernel/fs/vfs.c:54-57` mount table, `vfs_mount_at`/`vfs_umount`; "Single root mount" |
| No symlink / bind-mount support | no `VFS_TYPE_LINK`, no bind in `vfs.c` |
| RAM discovery is already an arch-HAL contract | `arch_platform_get_mem_regions()` — aarch64 FDT + manual probe, amd64 MB2 MMAP |
| Storage drivers present: **virtio-blk and ramdisk only** | `kernel/drivers/block/`, `kernel/drivers/virtio/virtio_blk.c`; **no AHCI/NVMe/SATA/SCSI anywhere** |
| 84 hardcoded `/home` references in project userland | measured, `grep '"/home'` |

**Conclusion:** the block CONTRACT and ext4 write-back already exist (MICROSCOPE
R1 landed).  What is missing is the PARTITION MODEL, the RAM-copy + authorised
write-back persistence policy, the installer, and real disk drivers.

### F-target — the architecture the maintainer specified
```
DISK (after install)
 ├── P1 KERNEL   immutable on disk; never writable at runtime
 ├── P2 ROOT "/" writable by ROOT only
 ├── P3 MACHINE  machine-only paths (the 4th partition the maintainer asked for,
 │               so "/" can be root-writable without exposing machine state)
 └── P4 USR1     /mnt/usr1 — per-user; more users = more partitions

BOOT
 kernel loaded from P1 → detects RAM → assigns ~20% of RAM to "/"
 → system image mounted there → nxinit from /sys/bin (unchanged)
 → kernel + root are COPIES IN RAM; the disk copies stay authoritative

PERSISTENCE (tied to the syscalls)
 userland write → VFS → path is authorised & disk-backed?
   → operation completes in the RAM view
   → the changed file/section is handed to the DISK DRIVER, which persists it
```
Setup modes: **full** (a disk exists → choose disk, choose the partition sizes)
and **RAM-only** (test setup: home initialised in RAM from detected free space).

### F-phases

- **F1 — DESIGN DOC FIRST (`docs/DESIGN-PERSISTENCE-INSTALL.md`).**  R6 rule
  ("doc dedicato prima del codice") applies: this changes the boot contract, the
  disk format and the write path at once.  The doc fixes: partition table
  layout + GUIDs, which trees live on which partition, the RAM-copy semantics
  (what is copied, when, what stays disk-authoritative), the write-back
  contract (who decides a write is persistable, at what granularity — file vs
  section — and the failure/ordering semantics), and the install/first-boot
  state machine.  **Nothing in F2+ starts before this is agreed.**
- **F2 — `mkdisk` multi-partition.**  Generalise the hand-rolled GPT+ext4 writer
  from one partition to N (it currently writes exactly one; the single-group
  ext4 and the ~1014-inode cap are per-partition limits that must be sized per
  role).  Emit P1..P4.  Keep a single-partition mode so today's `make run` stays
  byte-identical until F5 flips it.
- **F3 — kernel: mount the partition set.**  Use the existing multi-mount table
  (`vfs_mount_at`) to mount ROOT, MACHINE and USR1; extend the GPT probe to
  recognise the roles.  Extend `vfs_write_allowed()` so the tree ACL and the
  PARTITION agree (a machine-only path must also be on the machine partition —
  today the ACL is path-string-only, B2.2).
- **F4 — persistence write-back.**  The RAM-copy + authorised-write-back path,
  tied to the syscall boundary as the maintainer specified.  Depends on the
  memory work MICROSCOPE R4 (RAM-disk + tmpfs + PMM zones share one accounting
  path) and interacts with the buffer cache (`kernel/mm/buffer.c`).
  **OPEN INVESTIGATION (maintainer flagged "attento a … su amd64"):** on amd64
  the kernel shares the address space with the current process and identity-maps
  usable RAM (`kernel/arch/amd64/mm/mmu.c:116`), so carving a RAM partition at
  runtime must coordinate with PMM regions and the identity map; aarch64 splits
  TTBR0/TTBR1 and does not.  This asymmetry must be settled in F1's doc, and it
  is the same class as HAL-0's uaccess divergence.
- **F5 — `nxdisk` service.**  Partitioning, formatting, mounting — a supervised
  service behind a port (`OS1nx_disk`, per the `OS1nx_<service>` standard), so
  the installer is a CLIENT and the privileged disk work is one auditable place.
- **F6 — `nxcomp` service.**  Compression/decompression as a service
  (`OS1nx_comp`).  Format decision: prefer a small, permissively-licensed,
  self-contained decompressor that is GPLv2-compatible and needs no allocator
  heroics — candidates to evaluate in F1: **zlib/DEFLATE** (zip), **miniz**
  (single-file, MIT), **LZ4** (BSD, trivial decoder), **zstd** (BSD).  Selection
  criteria: decoder size, no dynamic allocation requirement, license
  compatibility with GPLv2, and whether we need seekable/streaming.  Used to
  ship the `usr` tree compressed inside `disk.img` and expand it at install.
- **F7 — `nxsetup` installer (first boot, runs ONCE).**  Chooses the username,
  sizes the partitions (or RAM-only mode), asks `nxdisk` to partition/format,
  asks `nxcomp` to expand `usr`, seeds the user's environment
  (`sys.env.HOME` → `/mnt/usr1/home`, per the B2.3 decision "everything via
  sys.env"), and copies the boot chain (bootloader + kernel, shipped compressed
  in `disk.img` like the ISO carries them) onto P1 so the machine becomes
  self-booting.  Guarded by a "already installed" marker so it never runs twice.
- **F8 — `nxauth` = `su`.**  Called from the shell; switches to another user or
  to root.  Bounded by the maintainer's earlier decision (nxauth v1 = root with
  a preset password); this phase widens it to named users once F7 creates them.
  Interacts with the LEVEL model (B2) — a user session runs at PLVL_USER with
  its own home partition.
- **F9 — default user migration.**  `user/home` → `user/usr1`; the shell opens
  as `usr<name>` with its partition mounted, not as root.  **This SUPERSEDES
  B2.3a/b/c** — the home move is now a sub-step of the installer work rather
  than a standalone migration, but the two decisions already taken stand:
  separate ext4 partition mounted at `/mnt/usr1`, and all 84 hardcoded paths
  resolved through `sys.env`.
- **F10 — `make` integration.**  Compress the `usr` tree into `disk.img` at
  build time; `disk.img` gains the boot chain (bootloader+kernel) so it can
  install like the ISO; re-check `make release` and the tmpfs story
  (MICROSCOPE R2, still open) — the release ISO currently boots RAM-volatile.

---

## Programme G — Real storage & device drivers (both arches, unified HAL)

> Prerequisite for F on real hardware: today the only block backends are
> virtio-blk and a RAM disk.  Extends `docs/FUTURE_DRIVER_EXPANSION_PLAN.md` §4
> (storage controllers as block providers) and `docs/PIANO-DRIVER-MATURITY.md`
> (Fase 2 = runtime plug-and-play), both already ASTRA-shaped.

- **G1 — AHCI/SATA block provider.**  HDD/SSD over the existing `block_dev`
  contract; PCI discovery already exists (`kernel/drivers/pci/pci.c`
  `pci_enumerate`).  No FS changes: it registers like virtio-blk.
- **G2 — NVMe block provider.**  The de-facto SSD standard (PCIe); same
  contract.  FUTURE_DRIVER_EXPANSION §4 already names the target controllers.
- **G3 — partition-table parser completion.**  GPT exists (`kernel/fs/gpt.c`,
  with GPT-01/02/03 markers open); F2/F3 need role-aware partition
  identification, and multi-disk selection for the installer.
- **G4 — USB completion + device tree.**  The stack is present (`xhci.c`,
  `ehci.c`, `uhci.c`, `usb_hid.c`, `usb_core.c`) and HID works on xHCI/EHCI;
  what is missing is enumeration completeness and the device-tree/ACPI
  description path.  `kernel/drivers/usb/xhci.c:14` carries an
  `ASTRA-VIOLATION` (calls `arch_vmm_map_device()` directly) that this phase
  must clear.
- **G5 — runtime plug-and-play, unified under the HAL.**  `usb_core.c:354`
  records "Runtime hotplug / re-scan is Fase 2"; the HAL device registry is
  immutable/lockless after SMP bring-up.  Make it mutable at runtime with
  recognition + dispatch, hotplug events HCD → HAL → IPC to userland, the SAME
  mechanism on aarch64 and amd64.  This is `PIANO-DRIVER-MATURITY` Fase 2 —
  adopt that plan, do not re-derive it.

**Ordering note:** F can be developed and verified entirely on virtio-blk (the
contract is backend-agnostic), so F does not block on G.  G is what makes F work
on real hardware.  G5 and F4 both touch the HAL and should not be interleaved.

---

## Programme R — ASTRA surface reduction: everything through the object layer

> Maintainer, 2026-07-23: *"guarda tutte le syscall p9, dobbiamo adattarle e
> adattare il vfs, tutto deve passare dal layer object in maniera analoga,
> riduciamo tutte le superfici come da ASTRA"* — and: correct ALL the
> implementations already built, not only the recent work; this vision is now
> part of the plan permanently.

**The rule.** ASTRA §6.2/§6.3/§6.6: every resource is a node in a namespace,
every node is representable as a file, every operation is a message on a
capability-bearing handle.  A kernel verb that reaches a resource WITHOUT going
through the object layer is a second implementation of something that already
exists — it doubles the audit surface, and the two copies drift (this is the
same defect class as the `level_ceiling` mirror B2.1 removed, and the three
registry paths below).

**Libc is unaffected as an API.**  POSIX names stay exactly where they are —
that is the personality layer the project already mandates ("POSIX vive SOPRA
os1").  What changes is WHO implements them underneath: composition over the
object primitives instead of a private kernel verb.

### R0 — MEASURED census (2026-07-23, real files)

| surface | object layer (ASTRA) | parallel path |
|---|---|---|
| **Files** | `open()` — **496** userland call sites | `file_read` 49, `file_write` 19, `list_dir` 20, `OS1_fs_unlink` 20, `OS1_fs_write` 17, `OS1_fs_read` 8, `OS1_fs_list` 2 → **~135 sites** over `SYS_FILE_READ/WRITE`, `SYS_LIST_DIR`, `SYS_MKDIR`, `SYS_UNLINK` |
| **Registry** | `OS1_NS_REG` + `OBJ_TYPE_REGKEY` — **0 userland users** | `SYS_REGISTRY` — 14 uses in lib.c |
| **Registry (again)** | `/reg` regfs mount — the p9 namespace, ASTRA §7.6 marked DONE | so the SAME data has **three** access paths |
| **Windows** | 8 `OBJ_CTL_*` verbs on `OBJ_TYPE_WINDOW` | **17** ad-hoc `SYS_WINDOW_*`/display syscalls |
| **IPC** | `OBJ_TYPE_PORT` (capability-addressed) | 12 uses of ambient pid `SYS_SEND/RECV/TRY_RECV` |
| **Process** | `OBJ_TYPE_PROCESS` + `OBJ_CTL_KILL/STOP/CONT`, `SYS_OBJECT_WAIT` | `SYS_KILL`, `SYS_WAIT` |

Two findings worth stating plainly:
- **The object layer has already won on files** (496 vs ~135).  The reduction is
  realistic, not aspirational.
- **The capability check is NOT the gap.**  `vfs_write_allowed()` is already the
  single write-authority seam and both paths call it.  So this programme is
  about ONE IMPLEMENTATION, not about closing a hole — which is why it can be
  done without a security window opening mid-migration.

### R-phases (each: build + headless boot on BOTH arches before the next)

- **R1 — files.**  Delete the path-based FS verbs from the kernel and compose
  them in libc over `open`/`lseek`/`read`/`write`/`close`.  `file_read(path,…)`
  is open+seek+read+close — a userland composition, not a syscall.  Removes
  `SYS_FILE_READ`, `SYS_FILE_WRITE`, `SYS_LIST_DIR` from the ABI.
  `SYS_MKDIR`/`SYS_UNLINK` are namespace MUTATIONS with no object equivalent
  yet — they stay until R1b gives the namespace a create/remove verb, and that
  is recorded rather than hand-waved.
- **R2 — registry: one truth.**  `/reg` is already a mounted namespace, so
  `open("/reg/…")` IS the object path.  Collapse the three onto it:
  `SYS_REGISTRY` becomes a compatibility shim (or goes), `OBJ_TYPE_REGKEY` has
  zero users and is a deletion candidate.  **Constraint:** the virtual
  `sys.proc.<pid>.env.*` routing lives inside `SYS_REGISTRY`'s path
  (`reg_virtual_proc_write`), and `getenv/setenv` depend on it — that routing
  must move with the data, not be lost.
- **R3 — windows.**  17 ad-hoc verbs onto the `OBJ_TYPE_WINDOW` object that
  already exists with 8 `OBJ_CTL_*` verbs.  Largest numeric reduction, highest
  risk (compositor + the ACL work from GFX-WIN-WRITE-01), so it goes after the
  two safe ones.
- **R4 — IPC.**  Ambient pid `SYS_SEND/RECV/TRY_RECV` → ports.  The daemon
  design doc already states ambient pid IPC is the seL4 rule ports repair, and
  Phase 16 already owns the unbounded-queue DoS on the ambient path — so this
  phase converges with work already scheduled.
- **R5 — process.**  `SYS_KILL`/`SYS_WAIT` → `OBJ_TYPE_PROCESS` verbs.  Note
  `SYS_WAIT` must keep working for a REAPED pid (Phase 9b): a capability cannot
  be acquired for a dead process, so the object path alone cannot express it —
  the retained-status lookup has to be part of the design, not discovered
  afterwards.
- **R6 — the VFS itself.**  Mounts through the namespace: `vfs_mount_at()` is
  the existing p9 seam (used by `/reg`, `/proc`, ktest) and the F-programme's
  role partitions (MACHINE→`/system`, USR→`/mnt/usr1`) must mount through it
  rather than growing new special cases.  The ROOT mount stays bootstrap-special
  by necessity — you cannot mount `/` through a namespace that does not exist
  yet — and that exception is written down here so it is not mistaken for drift.

**Ordering rationale:** R1 and R2 remove implementations without changing
authority; R3–R5 change how authority is NAMED and therefore need the object
model to already be the only file/registry path.  R6 is small and unblocks the
F-programme's partition set.

---

## §H — Open points found while doing other work

Anything noticed in passing lands here immediately.  A one-line fix may be
applied on the spot, but it is still recorded.

| id | where | note | disposition |
|---|---|---|---|
| PROC-REF-01 | object.c, process.c | see §A | B1 |
| CAP-POLICY-01 | process.c level_ceiling | PLVL_USER = CAP_ALL — see §A | B2 |
| REG-PID-PARSE-01 | registry.c `reg_proc_split` | unbounded `pid = pid*10+d`, can wrap negative; harmless (lookup fails) | S4, fix with B-family registry pass |
| CPU-AMD64-01 | arch/amd64/cpu | FPU/SSE save-restore landed-and-reverted; matters before amd64-heavy work; also blocks E2 (Rust needs a settled FP/SSE context contract) | Programme B / E2 |
| UACC-AMD64-02/03/04 | arch/amd64/mm/uaccess.c | amd64 uaccess has no lock vs concurrent unmap (aarch64 does); documented TOCTOU | HAL-0 (pre-existing phase) → gates E2 |
