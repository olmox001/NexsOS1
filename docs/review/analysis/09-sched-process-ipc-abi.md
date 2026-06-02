# Subsystem Analysis 09 — Scheduler, Process, IPC & Syscall ABI

> Severity/kind tags per [`../TAXONOMY.md`](../TAXONOMY.md). Hand-written (maintainer).
> Filesystem (vfs/ext4/gpt) is covered separately in `05-fs.md`.

| | |
|---|---|
| **Sources (hand-read)** | `kernel/sched/process.c` (939), `kernel/sched/elf.c` (180), `kernel/core/syscall_dispatch.c` (384), `include/api/os1.h` (179) |
| **Related** | arch syscall entry (`03-arch-cpu-exceptions.md`), `vmm_*`/`pmm_*` (`01-mm`) |
| **Build** | **[verified]** clean; **[verified]** runtime: init(PID1)→notify_srv(6)→shell(7), TTY live, SMP work-stealing exercised on aarch64 |

---

## 1. Purpose & Role

`process.c` owns the process model (fixed pool of 64), a per-CPU O(1)
priority-bitmap scheduler with work-stealing, wait queues, a kmalloc-backed
message-passing IPC, and `sbrk`. `elf.c` is the static ELF64 loader.
`syscall_dispatch.c` is the arch-agnostic syscall switch; `os1.h` is the public
userland ABI.

## 2. What Works (verified vs static)

- **[verified]** Preemptive multitasking + SMP: aarch64 brings up 4 cores, init
  spawns services on secondary CPUs, the focused shell stays responsive.
- **[static]** The per-CPU runqueues + `prio_bitmap` + `__builtin_ctz` give a
  clean O(1) pick; deferred-free correctly avoids freeing a stack you're standing on.
- **[static]** Syscall arg copies generally go through `arch_copy_*_from_user`
  with length caps before use — the right pattern (where applied).

## 3. Central themes

1. **Kernel ↔ userland-service coupling.** The scheduler reaches *up* into the
   compositor (graphics) for focus-based boosting. For a seL4-style design the
   dependency must invert: the kernel exposes scheduling primitives; a userland
   policy server (or the compositor, itself userland) influences priority via a
   capability — never a direct in-kernel call.
2. **The ABI is not a contract.** Numbering, error reporting, fd semantics, and
   permissions are ad-hoc and partly self-contradictory. This is the root of the
   "no coherent ABI" problem and must be designed before userland grows.
3. **The kernel trusts userland inputs more than it should** (ELF `p_vaddr`,
   unbounded IPC queues, no capability checks) — the opposite of the isolation goal.

## 4. Findings

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| SCHED-01 | W3 | WRONG-DESIGN | `process.c:511-513,568-581` | Scheduler calls `compositor_get_focus_pid()` and boosts the focused window's PID — the kernel scheduler depends on the graphics compositor. |
| SCHED-02 | W2 | BAD-IMPL | `process.c:471-704` | `schedule()` is large and intricate (deferred-free + work-steal + focus-boost + idle), guarded by many `pc==0` panics that betray past context-corruption bugs; hard to verify. |
| SCHED-03 | W2 | WRONG-DESIGN | `process.c:710-742` | `process_wait` is non-blocking (returns -1 while alive) → userland must busy-poll; zombies are reaped *only* via `process_wait`, so unwaited children leak pool slots. |
| SCHED-04 | W2 | BUG · DOC | `process.c:270-284` | Comment says "Kernel Stack (16KB)" but allocates `STACK_SIZE`=128KB per process. |
| SCHED-05 | W3 | BUG · SECURITY | `process.c:768-813` | `kernel_ipc_send` nests `sched_lock`→`msg_lock`→`cpu->sched_lock` (acknowledged AB-BA risk); unbounded per-process `msg_queue` lets a sender OOM a receiver (no flow control). |
| SCHED-06 | W2 | WRONG-DESIGN | `process.c:710-742`, `struct process` | No parent/child relationship; `wait` accepts any PID; no process groups/sessions. |
| SCHED-07 | W2 | BUG | `process.c:894-939` | `sys_sbrk` has no upper bound and no collision check with the fixed user stack at `0xC0000000`; depends on `heap_end` being initialised. |
| SCHED-08 | W1 | PERF | `process.c:222-228` | `struct process` page is zeroed twice (PMM already zeroes, then `memset`). |
| IPC-01 | W3 | BUG | `process.c:793-807` vs `841-844` | **Lost-wakeup race:** the sender only wakes a target already in `PROC_SLEEPING`; a receiver that missed the queue check but hasn't yet set `SLEEPING` can then sleep off-runqueue with a message queued and never be woken. [inferred] |
| IPC-02 | W2 | WRONG-DESIGN | `syscall_dispatch.c:81-85,163-165`; `process.c:849` | Syscall 31 (`IPC_RECV`) does *not* reschedule after blocking, but 231 (`RECV`) does; `sys_ipc_recv` returns 0 whether it received or blocked — callers can't tell. |
| ELF-01 | W4 | SECURITY | `elf.c:48-92` | No `p_vaddr` range check. Process PGDs share the kernel upper-half by reference, so a crafted ELF mapping a segment at a kernel VA can corrupt shared kernel page tables → privilege escalation / kernel corruption. [inferred] |
| ELF-02 | W3 | SECURITY | `elf.c:59-64` | User-segment NX is set only under `#ifdef ARCH_AARCH64`; on **amd64 all user segments are executable** (no W^X for userland). |
| ELF-03 | W2 | PERF | `elf.c:128-145` | Eagerly allocates every segment page **plus a 1MB stack (256 pages)** up front; no demand paging. |
| ELF-04 | W2 | MISSING | `elf.c:128-130` | No stack guard page; fixed stack VA; no check that segments don't overlap the stack. |
| ELF-05 | W2 | REFINE | `elf.c:38-46` | `e_phnum`/`p_memsz` are unbounded — a crafted ELF can exhaust memory (DoS). |
| ABI-01 | W3 | WRONG-DESIGN | `syscall_dispatch.c:78-85,159-165`; `os1.h:18-46` | Incoherent numbering: Linux-aarch64 numbers (63/64/93/247) mixed with ad-hoc (200/210/250) **and duplicate IPC numbers** — 30/31/32 *and* 230/231 both map to `sys_ipc_*`; 30/31/32 aren't even declared in `os1.h`. |
| ABI-02 | W3 | MISSING | `syscall_dispatch.c` (throughout) | No `errno`: all failures return a bare `-1`; `errno.h` ships but the syscall layer never returns negative error codes (`sys_ipc_send` is the lone exception, returning `-EINVAL`). |
| ABI-03 | W3 | WRONG-DESIGN | `syscall_dispatch.c:314,363-374` | No file-descriptor table; `fd` is overloaded (0=stdin/IPC, 1/2=window-by-pid, ≥100=window id). Neither POSIX nor Plan 9. |
| ABI-04 | W4 | SECURITY | `syscall_dispatch.c:151,166-168,176,251` | No capability/permission checks: any process may `kill` any PID, steal input focus, destroy any window, or write any file. The central gap vs the seL4 capability goal. |
| ABI-05 | W2 | BUG | `syscall_dispatch.c:159-162,296-298` | Self-admitted broken IPC-yield logic (comments: *"pt_regs_arg is read-only… I'll fix it below"*). |
| ABI-06 | W2 | BUG · PERF | `syscall_dispatch.c:350-377` | `sys_write` silently truncates >1023 bytes and **always** echoes every write to the UART (debug behaviour left in a hot path). |
| ABI-07 | W2 | BUG | `syscall_dispatch.c:136-149` | `SPAWN` disables IRQs across `process_create` + `process_load_elf` — i.e. across blocking ext4/virtio disk I/O. [inferred] |
| ABI-08 | W1 | DOC | `os1.h:150-151,173-175` | Duplicate `sin_fp`/`cos_fp`/`fixmul` prototypes with differing types; libc surface and OS API are conflated in one header. |

### Detailed entries (selected)

**ABI-04 / ABI-01 — the "coherent ABI" problem `[static]`**
The dispatcher (`syscall_dispatch.c:54-299`) is a flat `switch` with no notion of
*who* is allowed to make a call. `KILL` (`case 221`) calls `process_terminate(arg0)`
with only a `PROC_PERM_SYSTEM` self-protection check inside terminate — any user
process can kill any non-system PID, redirect global keyboard focus
(`case 232`), or write any path (`case 251`). Combined with the numbering mess
(ABI-01) and the absent `errno` (ABI-02), there is no stable, least-privilege
contract for userland to target. *Fix direction:* a single versioned syscall
table (one numbering scheme), negative-errno returns, an fd/handle table per
process, and capability checks at the dispatch boundary (a handle confers the
right to act on an object). This is the foundation the seL4/Plan 9 work sits on.

**ELF-01 — crafted-ELF kernel corruption `[inferred, security]`**
`process_load_elf` maps each `PT_LOAD` at `phdr.p_vaddr` into `proc->page_table`
without checking that `p_vaddr` lies in the user range. Because
`arch_vmm_create_process_pgd` copies the kernel's upper-half PGD entries *by
reference* (`aarch64/mm/mmu.c:204-206`, `amd64/mm/mmu.c:225-227`), a segment whose
`p_vaddr` falls in a kernel-shared region would install a user-writable mapping
into a table shared with the kernel. A hostile or buggy binary could thus corrupt
kernel memory. *Fix:* reject `p_vaddr`/`p_vaddr+p_memsz` outside `[0, USER_TOP)`
and require user PGD isolation for kernel tables.

**IPC-01 — lost wakeup `[inferred]`**
`sys_ipc_recv` checks the queue, and on a miss takes `cpu->sched_lock`, sets
`PROC_SLEEPING`, releases, and rewinds the syscall. A concurrent
`kernel_ipc_send` enqueues the message and only re-queues the target *if it
already reads* `PROC_SLEEPING` (`process.c:793`). If the send lands in the window
after the receiver's queue-check but before its state store, the target ends up
`SLEEPING`, off every runqueue, with a message waiting — no one re-queues it.
*Fix:* set `SLEEPING` and re-check the queue under the same lock the sender uses
(a proper wait-queue with condition re-test), or enqueue-on-send unconditionally.

## 5. Refactor Direction (toward the declared goals)

| Goal | Implication |
|---|---|
| **seL4 isolation / capabilities** | ABI-04 (capabilities at dispatch), ELF-01 (validate user mappings), SCHED-01 (decouple scheduler from compositor), bounded IPC (SCHED-05). |
| **Coherent ABI** | ABI-01/02/03: one versioned table, negative-errno, real fd/handle table. |
| **Plan 9 "everything is a file"** | Replace the overloaded `fd` ints and the registry syscall with a per-process namespace of file-like handles; IPC becomes read/write on channels. |
| **Long-lived userland services** | Blocking `wait`/reaping (SCHED-03), demand paging (ELF-03), flow-controlled IPC (SCHED-05). |
| **W^X (with mm/arch)** | ELF-02 (amd64 user NX), ties to MM-VMM-01 / AMMU-01/02. |

## 6. Verification Notes
- Scheduling/SMP/IPC happy-path: **[verified]** (aarch64 boot to interactive shell).
- IPC-01 lost-wakeup, ELF-01 mapping escape, SCHED-05 AB-BA: **[inferred]** from
  source; candidates for a targeted stress test during Phase 3.
