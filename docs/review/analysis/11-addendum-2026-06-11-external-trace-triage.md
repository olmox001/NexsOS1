# Addendum 11 — Triage of the external boot-trace analysis (2026-06-11)

An external agent reviewed the serial traces of two full boots (aarch64 +
amd64, GUI session with doom/demo3d) and produced a prioritized issue list.
This addendum is the **source-verified triage** of every claim — confirmed
items were fixed on `comprehensive-review` in the same session (commit hashes
below), misreadings are documented so they do not re-enter the backlog as
phantom bugs.

Scope of the fix batch (all verified by build on BOTH arches + headless QEMU
runtime tests with QMP keyboard injection, committed separately):

| Commit | Fix |
|---|---|
| `b9aad52` | sched: auto-reap zombies in `schedule()`; single-owner corpse freeing |
| `dc8a3db` | virtio-blk: serialise requests; static DMA targets; pre-publish `used` snapshot |
| `9bf27af` | amd64 step 14: PIT halted post-calibration (EXC-AMD64-03); dead probe block removed (EXC-AMD64-01) |
| `2212423` | irq step 15: `irq_handlers[]` lock (IRQ-02/#55); chip-owned EOI (IRQ-01/#47); spurious 8259 filter |
| `166887a` | sched: SCHED-IRQ-01 — `schedule()` masks IRQs itself (single contract) |
| `db503a3` | gui/input: Z-order focus reset on window destroy; keystroke log → debug |

## 1. Claim-by-claim verdict

### 1.1 CONFIRMED — real bugs, now fixed

**"VirtIO read failure status=1 → filesystem corruption risk" (their CRITICO #1).**
CONFIRMED as a bug, with a different root cause than speculated ("queue not
synchronized / DMA mapping bug").  `virtio_blk_read/write` shared one set of
ring descriptors (`desc[0..2]`) and built them with **no locking**; the
request header and status byte were **stack** DMA targets (DRV-VIRTIO-03).
Two CPUs issuing I/O concurrently (doom WAD lumps + any other FS user)
clobbered each other's descriptors mid-flight → the device read a torn
header → `VIRTIO_BLK_S_IOERR`.  Fixed in `dc8a3db` (driver-level spinlock,
static DMA targets, plus the DRV-VIRTIO-04 pre-publish `used->idx` snapshot).
Re-tested: aarch64 `counter` + `doom` concurrently → full WAD load, engine
tick loop, 0 read failures.

**"Zombie process accumulation" (their CRITICO #2).** CONFIRMED — this is
SCHED-03, already in the catalog.  The shell spawns without ever calling
WAIT, so every exited process (doom, demo3d) kept its pool slot, its 128 KB
kernel stack and its whole PGD forever.  Their proposed mechanism
("scheduler fragmentation") was wrong — zombies are not on runqueues — but
the leak was real.  Fixed in `b9aad52`: `schedule()` auto-reaps a ZOMBIE
prev via the per-CPU deferred-free stack (same mechanism as PROC_DEAD).
The fix surfaced and closed two adjacent lifecycle holes (see §2).

### 1.2 PARTIALLY CONFIRMED — real fragility, misread mechanism

**"Input routing inconsistency: focus to PID 7 but input to PID 11"
(their CRITICO #3).** The headline evidence is a MISREAD: `Char='d' (val=1)`
/ `(val=0)` is standard evdev press/release semantics (not duplicate events
needing "debounce"), and the `-> PID` in each line *is* the routing target,
which is the **same variable** the compositor sets (`keyboard_focus_pid`) —
there are not two desynchronized focus notions.  Input going to a different
PID after a focus line is normal: any app may take focus via syscall 232 at
startup.  What IS real: (a) on window destroy, focus reset to a **hardcoded
PID 7** ignoring Z-order — wrong whenever the shell is not PID 7 (PID order
depends on boot services); fixed in `db503a3` (top-most surviving window
wins, 7 only as empty-desktop fallback).  (b) syscall 232 has **no
permission check** — any process can steal global focus.  That is ABI-04
(known, W4) and lands with the capability model (epic #93).

**"Logging non thread-safe / needs per-core buffers" (their IMPORTANTE #1).**
Half-misread: printk already has per-CPU buffers (`cpu_info.printk_buf`) and
serialised output; interleaved *lines* from [C0..C3] are cosmetic, not
corruption.  The genuine problems behind the noisy traces were (a) one
pr_info per keystroke press AND release, from IRQ context (fixed → pr_debug,
`db503a3`), and (b) an "IRQ: Unhandled interrupt 47" flood during disk I/O —
which turned out to be a real driver-layer finding, see §2.1.

### 1.3 ALREADY KNOWN — catalogued, scheduled for Phase B

| Their claim | Catalog entry | Where it lands |
|---|---|---|
| No IPC model / capabilities / permissions | ABI-01..04, IPC-01.. | Epic **#93** (coherent ABI + caps) |
| No ASLR/KASLR, kernel/user heap separation | MM-VMM-01, AMMU-01 (W^X, higher-half) | Epic **#92** (address-space rework) |
| "Two parallel kernels" / driver duplication | — partially wrong: core (sched/VMM/process/IPC) **is** shared; per-arch GIC vs PIC/LAPIC and MMIO vs PCI is the intended HAL split | Epic **#95** unifies the service/HAL layer |
| Scheduling fairness tuning | SCHED-01 (focus boost in kernel = design inversion) | Epic #93/#95 |

### 1.4 Overstated / not actionable

- "filesystem corruption futura" from the IOERR path: reads fail cleanly
  (status≠OK → -1), no write-path corruption was implicated by the trace.
- "system degradation nel tempo" via zombie *scheduler* load: zombies were
  never enqueued; the cost was slots + memory (now reclaimed anyway).

## 2. New findings surfaced by this session (not in the external list)

### 2.1 Spurious 8259 IRQ15 flood (found while testing, fixed `2212423`)
The "Unhandled interrupt 47" storm during disk I/O is the slave PIC
reporting **spurious IRQ15** (virtio INTx pulses deasserting before INTA).
Worse than noise: the old code answered each with a non-specific **slave
EOI**, which can wrongly clear a genuine in-service slave bit.  Now filtered
before dispatch via the ISR register (`pic_handle_spurious`): no EOI for
spurious IRQ7, master-only EOI for spurious IRQ15; LAPIC spurious vector
0xFF also absorbed.

### 2.2 Parked-corpse leak in `process_terminate` (found during reaper work, fixed `b9aad52`)
Killing a process sleeping on a wait queue marked it DEAD and returned —
but a fully parked sleeper is not `current_task` anywhere and sits on no
runqueue, so **no reap path ever reached it**: permanent leak of stack +
PGD + slot.  Now: parked sleepers are freed immediately (checked under the
owning CPU's `sched_lock`); a sleeper still mid-execution (SLEEPING set,
not yet switched away) is left to its own `schedule()`'s prev==DEAD reap.

### 2.3 `process_wait` double-free hazard (latent, fixed `b9aad52`)
With (and even before) auto-reaping, `process_wait` freeing a ZOMBIE's
resources raced the deferred-free drain on the victim's CPU (corpse already
on the reap stack → double `pmm_free_pages`/`vmm_destroy_pgd`).
`process_wait` is now a pure reporter (pid / -1 alive / -2 reaped); corpse
freeing has exactly one owner: the scheduler reaper.

### 2.4 Supervisor respawn was racing the reaper (user-reported, fixed `f37d137`)
User report: "the first shell respawns on aarch64 but not on amd64".  Not an
arch bug — two stacked defects made init's supervision probabilistic:
(1) init respawned only on `wait(pid)==pid`; with auto-reap (`b9aad52`) the
kernel frees corpses on its own, so init had to *catch the corpse in the
pre-drain window* — won on the observed aarch64 runs, lost on amd64 (pure
scheduling timing).  `wait()==-2` (already reaped) is now treated as "child
gone" too.  (2) `process_terminate` freed a SLEEPING non-waitqueue victim
(a shell blocked in `sys_ipc_recv` — its normal state) immediately and
unconditionally: never a waitable corpse (always -2), and the free did not
check `current_task` — an IPC sleeper that had not yet switched away could
have its stack/PGD freed under a running CPU (pre-existing SCHED-UAF-family
hazard).  The terminate tail is now unified with a parked check for ALL
non-running states.  NOTE: the initial auto-reap landing (`b9aad52`)
*tightened* this race for supervisors — the original triage (§1.1) missed
that `init.c` polls `wait()`; corrected here.  Verified both arches:
`exit` → respawn, external `kill` of the IPC-sleeping notify server →
respawn, ps clean, 0 panics.

### 2.5 Legacy virtio-blk-pci hangs at first read (open, low priority)
Booting amd64 with virtio devices instantiated as **legacy PCI** (no
`disable-legacy=on`) hangs at "Partition: Initializing..." — the legacy
port-I/O path never completes the first read.  The canonical QEMU flags
(Makefile `QEMU_FLAGS`) always use `disable-legacy=on`, so this only bites
hand-rolled invocations.  Related to the DRV-VIRTIO-02 family (MMIO/PCI
transport quirks).  Candidate issue for the Phase B driver work; NOT fixed
in this batch.

## 3. Phase A residuals — closed in this batch

- **Step 14** (`9bf27af`): EXC-AMD64-03 — the PIT was left free-running after
  LAPIC calibration; with LINT0 necessarily ExtINT (it carries all legacy PIC
  lines incl. PCI INTx), any IRQ0 unmask would have double-ticked vector 32.
  PIT now halted post-calibration (mode-0 control word, no count).
  EXC-AMD64-01 — unreachable amd64 probe-recovery block + flags deleted
  (probing is an aarch64-only fallback with a working ELR fixup).
- **Step 15** (`2212423`): IRQ-02/#55 — `irq_handlers[]` now lock-guarded,
  dispatch copies the (handler,data) pair under the lock and calls outside
  it.  IRQ-01/#47 — EOI is chip-owned (`irq_chip_end` → `pic_chip_end` does
  the full LAPIC+8259 sequence); the dead acknowledge-loop on amd64 is
  documented as by-design (vectored dispatch has no IAR).
- **SCHED-IRQ-01** (`166887a`): `schedule()` masks IRQs as its first action
  (before `get_cpu_info()`), restores on no-switch exits, returns masked on
  the switch exit (IRET/ERET loads the next frame's flags).  All syscall
  entry states are now legal; the nested-schedule class is closed.

## 4. Still open after this batch (priority order)

1. **Async block I/O** (DRV-VIRTIO-08): reads still busy-wait with IRQs
   masked under the new lock — correct but throughput-hostile under load.
   Phase B driver epic.
2. **Blocking `wait()` + exit-status collection**: needs parent/child links
   (SCHED-06); auto-reap intentionally discards status today.
3. **Focus/permission model**: syscall 232 unguarded (ABI-04) → epic #93.
4. **Legacy virtio-pci transport** (§2.5) → Phase B driver epic.
5. Everything in §1.3 (epics #92/#93/#95).
