# TIMER-UAF-01 — Per-CPU software-timer list corruption (kernel fault)

**Status:** CERTIFIED · ROOT-CAUSED · FIXED
**Severity:** Critical (unrecoverable kernel panic, both arches)
**Subsystem:** `kernel/core/timer.c` (per-CPU software timers) + `kernel/core/syscall_dispatch.c` (`sys_yield`) + `kernel/sched/process.c` (`kernel_ipc_send` wake)
**Introduced by:** the per-core timer model (commits `00a83e6`, `fb2f77c` — per-CPU software-timer lists + the `sys_yield` anti-spin throttle). The previous model used a single global timer list fired only by CPU 0 and was not exposed to this race.
**Raw evidence:** `kernel_fault.md` (aarch64 trace), the amd64 UTM trace in the session.

---

## 1. Symptom

Two architecturally different panics, **same root cause** (memory corruption of a
per-CPU software-timer linked list), triggered under load (multiple apps using
`sleep`/`yield` + IPC: doom, demo3d, top, forkbomb).

### aarch64 (deterministic decode)
```
ELR_EL1: 0xffff000040086ae0   = kernel_timer_tick+0x130
FAR_EL1: 0x0000000000000008   = write to address 0x8  (NULL + 8)
ESR_EL1: 0x96000044           = EC 0x25 (Data Abort, same EL), WnR=1 (write), DFSC=translation L0
```
`kernel_timer_tick+0x130` disassembles to the inlined `__list_del`:
```
ldp x2, x0, [x1]     ; x2 = entry->next , x0 = entry->prev
str x0, [x2, #8]     ; next->prev = prev      <-- FAULT, x2 (entry->next) == 0
str x2, [x0]         ; prev->next = next
```
The faulting entry had **`next == 0` and `prev == 0`** (registers X02=0, X00=0):
i.e. a node that was *already* `list_del`'d (our `list_del()` NULLs both
pointers) is being deleted **again** — a double `list_del` inside the per-CPU
timer firing loop.

### amd64 (UTM)
```
Vector 13 (#GP), err 0, RIP = pic_chip_end+0x33
```
`pic_chip_end+0x33` is the function's terminal **`ret`**. A `#GP` (not `#PF`) on
`ret` with error code 0 = a **non-canonical return address** popped off the
stack: the kernel stack of the interrupted task (RBP in PID 8 / demo3d's kstack)
had its saved return address overwritten. Same defect class — a stray list write
landing in a kernel stack — manifesting as stack corruption instead of a NULL
write. A `VMM: PGD … destroyed` (a process teardown) is logged at the instant of
the fault, confirming the load/teardown context.

---

## 2. Root cause

`struct timer` is embedded in `struct process` as `sleep_timer`. It is armed by
two syscalls, both of which sleep the caller and rely on the per-CPU software
timer wheel:

* `sys_nanosleep()` — **already** guarded: `if (!timer_pending(&p->sleep_timer))`
  before `timer_setup()/timer_add()`.
* `sys_yield()` anti-spin throttle — **NOT guarded**: it called
  `timer_setup(&p->sleep_timer, …)` (which is `INIT_LIST_HEAD` +
  field init) and `timer_add()` **unconditionally**.

Separately, `kernel_ipc_send()` (`kernel/sched/process.c:1491`) wakes a
`PROC_SLEEPING` target by flipping it to `PROC_READY` and enqueuing it **without
cancelling `sleep_timer`**. So a process can run again while its `sleep_timer` is
**still linked** in a per-CPU `timer_list`.

The fatal interleaving:

1. Process P trips the yield throttle → `sleep_timer` armed (linked on CPU A,
   `pending=true`), `PROC_SLEEPING`, schedules away.
2. Before the 1-tick timer fires, P is force-woken by an IPC send
   (`PROC_SLEEPING → READY`, **timer left linked**).
3. P runs, keeps spinning `yield()` within the same tick, trips the throttle
   again. Because it was unguarded, it calls
   `timer_setup(&p->sleep_timer)` → `INIT_LIST_HEAD()` on a node that is **still
   linked**: `node->next = node->prev = &node`. The old neighbours still point at
   the node. `timer_add()` then links it into a (possibly different) CPU's list.
4. The node now lives in two lists / has dangling cross-links. The next
   `kernel_timer_tick()` firing walk performs a second `list_del` on a node whose
   pointers were already NULLed → **write to NULL+8** (aarch64) or a stray write
   into an adjacent kernel stack → **#GP on `ret`** (amd64).

This is why the two arches diverged on *symptom* while sharing the *cause*: the
corrupting write lands on whatever memory follows the mangled node.

---

## 3. Resolution (certified)

Two surgical, defense-in-depth changes — no redesign of the three-tier model.

**(a) `sys_yield()` — guard the re-arm exactly like `sys_nanosleep()`**
(`kernel/core/syscall_dispatch.c`):
```c
if (!timer_pending(&p->sleep_timer)) {
    timer_setup(&p->sleep_timer, proc_sleep_wake, p);
    timer_add(&p->sleep_timer, jiffies + 1);
}
```
`timer_setup()`/`INIT_LIST_HEAD()` is now never invoked on a still-linked node.
If the timer is already pending (force-woken case), the existing 1-tick trigger
wakes us — we just re-sleep.

**(b) `timer_add()` — never double-link a node** (`kernel/core/timer.c`):
```c
if (t->pending)
    timer_del(t);   /* unlink from its current CPU first */
```
A node can no longer be linked twice even if a future caller forgets the
`timer_pending` guard. `timer_del()` takes the owner-CPU `timer_lock` (not held
here), so there is no recursion; the under-lock `pending` re-check makes it a
safe no-op against a concurrent firing on the owner CPU.

### Why this is sufficient (invariant)
The **only** writers of a `sleep_timer`'s list linkage are `timer_add`,
`timer_del`, and the per-CPU firing loop, all under the owner CPU's
`timer_lock`; `pending` is always mutated together with linkage under that lock,
so an unlocked `t->cpu` read in `timer_del` can observe the real owner CPU or the
post-fire `-1`, never a *wrong positive*. With (a)+(b), `INIT_LIST_HEAD` is only
ever applied to an unlinked node and `list_add_tail` is only ever applied to an
unlinked node ⇒ the per-CPU `timer_list` can no longer be corrupted ⇒ the double
`list_del` (and the stack-corruption variant) is impossible.

---

## 4. Verification

* `make all ARCH=amd64` and `make all ARCH=aarch64` — clean build, both arches.
* Headless boot, both arches: reached the init supervisor loop, **0**
  `PANIC|NESTED|Unhandled|Unrecoverable|Data abort|#GP`.

---

## 5. Follow-ups (tracked separately, NOT required for this fix)

* **TIMER-UAF-02 (correctness, low risk):** make `kernel_ipc_send` (and any
  non-timer wake of a `PROC_SLEEPING` task) cancel `sleep_timer` so a woken
  sleeper never carries a stale pending timer. Currently harmless (the spurious
  fire hits `proc_sleep_wake`'s `state == PROC_SLEEPING` guard and self-removes),
  so this is hardening, not a defect.
* **HAL-ARCH-01:** the per-arch divergence in *symptom* is a reminder that the
  amd64 IRQ EOI path (`pic_chip_end` doing LAPIC EOI **and** legacy-PIC EOI for
  vector 32) and the aarch64 timer IRQ path are not yet expressed through one
  uniform HAL contract. Audit `TUTTO IL KERNEL` for residual direct arch access
  (see `docs/ASTRA.md`).
