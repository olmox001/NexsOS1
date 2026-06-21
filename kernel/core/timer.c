/*
 * kernel/core/timer.c
 * Generic Timer Logic (Jiffies, Software Timers, Scheduling)
 *
 * This file is the arch-agnostic timer layer.  It owns:
 *   - kernel_timer_tick(): the central per-tick ISR hook called by the
 *     arch-specific timer IRQ handler (aarch64: drivers/timer/timer.c;
 *     amd64: arch/amd64/platform/platform.c via PIT/APIC).
 *   - A software timer list (struct timer), run on CPU 0 every tick.
 *   - Compositor pacing: fires compositor_tick() at ~30 FPS on CPU 0.
 *   - Arch-specific per-CPU timer init via __attribute__((weak)) stubs
 *     that each arch overrides.
 *
 * Layering:
 *   arch IRQ -> kernel_timer_tick() -> schedule()
 *                                   -> software timer callbacks (CPU 0)
 *                                   -> compositor_tick()          (CPU 0)
 *
 * Key invariants:
 *   - jiffies is incremented only by CPU 0 to avoid SMP races; all other
 *     CPUs still go through kernel_timer_tick() for preemption but do not
 *     touch jiffies or the software timer list.
 *   - timer_lock protects the timer_list linked list; callbacks are invoked
 *     while timer_lock is held — callbacks must not try to acquire timer_lock.
 *   - The weak timer_handler() stub returns regs unchanged; per-arch overrides
 *     may perform additional bookkeeping (e.g. EOI).
 *
 * Known issues:
 *   ARCH-03  (W2 STUB) timer_get_us() (defined in the arch platform files)
 *            returns jiffies*1000 on amd64 — a "dummy for now" value, not a
 *            real microsecond clock.  sys_get_time() is therefore inaccurate
 *            on amd64.  aarch64 uses the arch counter and is accurate.
 *            (This defect is in the arch platform file, not in this file;
 *            cited here because kernel_timer_tick drives jiffies.)
 */
#include <drivers/timer.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/list.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

extern void compositor_tick(void);
/* USB has no IRQ wired (polled HCDs); the tick drives HID input on CPU 0 so it
 * works even when no virtio/PS-2 interrupt ever fires (e.g. UTM, real HW). */
extern void usb_hid_poll(void);
extern volatile int panic_flag;
/* compositor_interval: tick stride between compositor_tick() calls; set once
 * from HZ and COMPOSITOR_TARGET_FPS on the first tick processed by CPU 0. */
static uint64_t compositor_interval = 1;
#define COMPOSITOR_TARGET_FPS 30

/* Software timers are PER-CPU: each CPU owns a timer_list + timer_lock in its
 * struct cpu_info (kernel/cpu.h), fired by that CPU in kernel_timer_tick against
 * the global jiffies clock. timer_add() arms on the current CPU and the timer
 * records its owner CPU (t->cpu) so timer_del() removes it from the right list.
 * This keeps a process's sleep timer on the core that holds it: local wakeup, no
 * cross-CPU dependency and no global timer-lock contention. */

/* jiffies and timer_freq are defined in drivers/timer/timer.c for AArch64
 * or in arch/amd64/platform/platform.c for AMD64. */
extern volatile uint64_t jiffies;

/* ---------------------------------------------------------------------------
 * Real-time reference (docs/TIMER-MODEL.md §1)
 *
 * mono_ns() is the single source of truth: monotonic nanoseconds since boot,
 * computed from the free-running hardware counter (CNTVCT on aarch64, TSC on
 * amd64) and its frequency, both arch HAL primitives. jiffies is a cheap
 * integer view that CPU 0 reconciles against mono_ns() every tick so dropped
 * ticks are recovered (Tier 1).
 *
 * Only the EPOCH (clk_boot_count) is captured once, lazily, on the first call.
 * The frequency is re-read fresh every call (a cheap inline counter accessor),
 * deliberately NOT latched: on amd64 arch_timer_get_freq() returns a 1 GHz
 * fallback until the TSC is calibrated, and latching that stale value would
 * permanently skew the clock. Reading it live means the clock self-corrects the
 * instant calibration publishes the real frequency. In practice the first call
 * already happens after calibration (it runs in early arch init, before any
 * scheduling or timer tick), so no skew is observed.
 * ------------------------------------------------------------------------- */
static uint64_t clk_boot_count;
static volatile int clk_ready;

/* timer_counts_to_ns - convert a hardware-counter delta to nanoseconds.
 *
 * The single place that scales counter units to ns: counts * 1e9 / freq via a
 * 128-bit intermediate (counts * 1e9 needs > 64 bits within minutes). Backed by
 * __udivti3/__multi3 in kernel/lib/math.c (no libgcc in this -nostdlib build).
 * Used by mono_ns() and by the per-process CPU-time read path, which accumulates
 * raw counts in the scheduler hot path and converts only when queried. */
uint64_t timer_counts_to_ns(uint64_t counts) {
  uint64_t freq = arch_timer_get_freq();
  if (freq == 0)
    freq = 1; /* guard div-by-zero before any frequency source is up */
  return (uint64_t)(((__uint128_t)counts * NSEC_PER_SEC) / freq);
}

uint64_t mono_ns(void) {
  if (!clk_ready) {
    clk_boot_count = arch_timer_get_count();
    clk_ready = 1;
  }
  return timer_counts_to_ns(arch_timer_get_count() - clk_boot_count);
}

uint64_t timer_get_ns(void) { return mono_ns(); }

/*
 * timer_percpu_tick - Tier 2 per-CPU clock accounting + rearm (HAL-driven).
 *
 * One arch-neutral routine replaces the previously duplicated per-arch drift
 * logic (aarch64 timer_handler, amd64 idt.c). Everything is expressed in
 * HARDWARE COUNTER units via the HAL timer primitives:
 *   1. interval = freq/HZ; accumulate the freq%HZ remainder in tick_error_acc
 *      and add one extra count when it crosses HZ (long-term-rate exact).
 *   2. Advance cpu->next_tick_target by the corrected interval.
 *   3. Catch-up clamp: if the target already fell behind the live counter
 *      (ticks lost to IRQ latency / a long critical section), reset to
 *      now + interval — the per-CPU "compare against real time to recover lost
 *      time" rule, instead of replaying a burst.
 *   4. Reprogram the compare via arch_timer_set_compare(): a real reprogram on
 *      one-shot timers (aarch64 CNTV_CVAL), a no-op on periodic timers (amd64
 *      LAPIC), so this serves both arches unchanged.
 *
 * Locking: touches only this CPU's cpu_info; no lock. IRQ context: YES.
 */
void timer_percpu_tick(struct cpu_info *cpu) {
  uint64_t freq = arch_timer_get_freq();
  uint64_t interval = freq / HZ;
  uint64_t remainder = freq % HZ;

  cpu->tick_error_acc += remainder;
  if (cpu->tick_error_acc >= HZ) {
    interval += 1;
    cpu->tick_error_acc -= HZ;
  }

  cpu->next_tick_target += interval;
  uint64_t now = arch_timer_get_count();
  if (cpu->next_tick_target <= now)
    cpu->next_tick_target = now + interval;

  arch_timer_set_compare(cpu->next_tick_target);
}

/*
 * timer_percpu_arm - seed this CPU's tick schedule on timer bring-up.
 *
 * Sets next_tick_target one interval out from the live counter, clears the
 * fractional accumulator, and programs the first compare via the HAL. Called
 * once per CPU from the arch per-CPU timer init (aarch64 timer_init_percpu,
 * amd64 lapic_timer_setup); the arch path still owns enabling the timer and
 * unmasking its IRQ.
 */
void timer_percpu_arm(struct cpu_info *cpu) {
  uint64_t freq = arch_timer_get_freq();
  cpu->next_tick_target = arch_timer_get_count() + freq / HZ;
  cpu->tick_error_acc = 0;
  arch_timer_set_compare(cpu->next_tick_target);
}

/* Weak stubs for arch-specific timer functions (can be overridden) */
/*
 * timer_init_percpu - per-CPU timer hardware init (weak stub).
 *
 * Each arch overrides this to program its local timer (e.g. aarch64 CNTV_CTL,
 * amd64 LAPIC timer).  The weak no-op is used when no arch override is linked.
 *
 * IRQ context: no — called once from the per-CPU boot path with IRQs off.
 */
__attribute__((weak)) void timer_init_percpu(void) {}

/*
 * timer_handler - arch-specific timer IRQ hook (weak stub).
 *
 * Called from the arch timer IRQ handler *before* kernel_timer_tick().
 * Allows arch code to perform EOI or counter reprogramming.  The weak stub
 * is a pass-through that returns regs unchanged.
 *
 * IRQ context: yes — runs inside the timer interrupt.
 * Returns: the (possibly modified) pt_regs pointer.
 */
__attribute__((weak)) struct pt_regs *timer_handler(struct pt_regs *regs) {
  return regs;
}

/*
 * kernel_timer_tick - central per-tick entry point called from the timer IRQ.
 *
 * Called on every CPU on every timer interrupt.  Responsibilities:
 *   1. Halt this CPU immediately if another CPU has set panic_flag.
 *   2. Increment cpu->tick_count (per-CPU; no lock needed).
 *   3. CPU 0 only: increment the global jiffies counter.
 *   4. CPU 0 only: fire expired software timers under timer_lock.
 *   5. CPU 0 only: call compositor_tick() every compositor_interval ticks.
 *   6. All CPUs: invoke schedule(regs) for preemptive multitasking.
 *
 * Locking: acquires timer_lock (irqsave) around the software timer walk on
 *          CPU 0; no other locks held on entry.
 * IRQ context: yes — called directly from the architecture timer IRQ handler.
 * Returns: the pt_regs pointer returned by schedule() (may differ from regs
 *          if a context switch occurs).
 */
struct pt_regs *kernel_timer_tick(struct pt_regs *regs) {
  /* Halt this CPU if another CPU panicked */
  if (panic_flag) {
    arch_timer_control(0);
    arch_cpu_halt();
  }

  struct cpu_info *cpu = get_cpu_info();
  cpu->tick_count++;

  /* Tier 1 — global clock (docs/TIMER-MODEL.md §2): only CPU 0 writes jiffies
   * (no SMP write race), and instead of a blind ++ it reconciles jiffies with
   * real time. If ticks were dropped (IRQ latency, a long critical section, a
   * CPU-bound focused task), 'expected' jumps ahead and jiffies catches up in
   * one step. The guard keeps it monotonic; steady state advances by exactly 1
   * per tick, so normal behaviour is unchanged. */
  if (cpu->cpu_id == 0) {
    uint64_t expected = mono_ns() / NS_PER_TICK;
    if (expected > jiffies)
      jiffies = expected;
  }

  /* Fire THIS CPU's expired software timers — every CPU, against the global
   * jiffies clock — so a process that slept on this core is woken here, locally.
   * Callbacks run inside this CPU's timer_lock and must not call timer_add/_del
   * or anything that re-acquires it. */
  {
    struct timer *t, *tmp;
    uint64_t tflags;
    spin_lock_irqsave(&cpu->timer_lock, &tflags);
    list_for_each_entry_safe(t, tmp, &cpu->timer_list, list) {
      if (jiffies >= t->expires) {
        list_del(&t->list);
        t->pending = false;
        t->cpu = -1;
        if (t->callback)
          t->callback(t->data);
      }
    }
    spin_unlock_irqrestore(&cpu->timer_lock, tflags);
  }

  /* CPU 0 only: input polling + compositor. */
  if (cpu->cpu_id == 0) {
    /* Poll USB HID every tick (CPU 0): keyboard events feed the evdev buffer,
     * pointer motion goes straight to the compositor. Cheap when idle (just an
     * event-ring head check per device). */
    usb_hid_poll();

    /* Calculate compositor interval once: HZ/30 ticks (or 1 if HZ < 30).
     * interval_init guards against re-computation on every tick. */
    static int interval_init = 0;
    if (!interval_init) {
        if (HZ >= COMPOSITOR_TARGET_FPS) {
            compositor_interval = HZ / COMPOSITOR_TARGET_FPS;
        } else {
            compositor_interval = 1;
        }
        interval_init = 1;
    }

    if ((jiffies % compositor_interval) == 0) {
      compositor_tick();
    }
  }

  /* Call Scheduler for Preemption */
  return schedule(regs);
}

/*
 * timer_setup - initialise a struct timer before first use.
 *
 * Sets the callback, opaque data pointer, pending=false, and initialises the
 * embedded list_head.  Must be called before timer_add().
 *
 * Locking: none — caller owns t exclusively at this point.
 * IRQ context: safe (no locks taken, no shared state written).
 */
/* Software timer management */
void timer_setup(struct timer *t, timer_callback_t callback, void *data) {
  INIT_LIST_HEAD(&t->list);
  t->callback = callback;
  t->data = data;
  t->pending = false;
  t->cpu = -1; /* not armed on any CPU yet */
}

/*
 * timer_add - schedule a software timer to fire at tick 'expires'.
 *
 * Adds t to the tail of timer_list.  'expires' is an absolute jiffies value;
 * use (jiffies + delay_ticks) to express a relative delay.
 * The timer will fire on the next kernel_timer_tick() on CPU 0 after
 * jiffies >= expires.
 *
 * Locking: acquires timer_lock (irqsave) — safe to call from any context
 *          including IRQ handlers, but NOT from inside a timer callback
 *          (would deadlock on timer_lock).
 * Side effects: sets t->pending = true.
 */
void timer_add(struct timer *t, uint64_t expires) {
  /* Defence in depth (TIMER-UAF-01): never link the same node twice.  If the
   * timer is somehow still queued — a re-arm of a not-yet-fired timer, or a
   * caller that bypassed timer_pending() — unlink it from its current CPU
   * FIRST.  A double list_add_tail() (or a timer_setup()/INIT_LIST_HEAD on a
   * still-linked node) corrupts the per-CPU timer_list and later crashes
   * kernel_timer_tick() with a double list_del (write to NULL+8 on aarch64,
   * a clobbered return address -> #GP on amd64).  timer_del() takes the owner
   * CPU's timer_lock, which we do not hold here, so there is no recursion. */
  if (t->pending)
    timer_del(t);

  struct cpu_info *c = get_cpu_info();
  uint64_t flags;
  spin_lock_irqsave(&c->timer_lock, &flags);
  t->expires = expires;
  t->pending = true;
  t->cpu = (int)c->cpu_id; /* armed on the current core; fired by its tick */
  list_add_tail(&t->list, &c->timer_list);
  spin_unlock_irqrestore(&c->timer_lock, flags);
}

/*
 * timer_del - cancel a pending software timer.
 *
 * Removes t from timer_list only if t->pending is true.  Safe to call on a
 * timer that has already fired or was never added (no-op in that case).
 *
 * Locking: acquires timer_lock (irqsave).
 * IRQ context: safe.
 * Side effects: sets t->pending = false.
 */
void timer_del(struct timer *t) {
  uint64_t flags;
  /* TIMER-UAF-02: t->cpu is read WITHOUT a lock (we don't know which CPU's
   * timer_lock to take until we read it).  Under SMP that snapshot can be stale
   * vs. a concurrent timer_add() on the owning core, which publishes
   * t->pending=true BEFORE t->cpu — so an unlocked reader may momentarily see
   * (pending=true, cpu=-1).  The previous code took the cpu<0 fast path here and
   * cleared t->pending WITHOUT unlinking; that broke the pending<=>linked
   * invariant, after which the post-DEAD reaper's timer_del() trusted
   * pending==false and skipped list_del() too, leaving a still-linked timer on a
   * process that was then freed and poisoned (0xCC) — the kernel_timer_tick UAF.
   *
   * Fix: (1) never clear t->pending in the unlocked cpu<0 branch — leaving the
   * invariant intact means a later timer_del()/timer_add() still cancels it
   * correctly; (2) re-validate t->cpu UNDER the owning timer_lock and retry if it
   * moved, so the unlink always happens against the list the node is really on. */
  for (;;) {
    int cpu = t->cpu;
    if (cpu < 0 || cpu >= MAX_CPUS)
      return; /* not armed on a valid CPU (or arm mid-publish): leave pending */

    spin_lock_irqsave(&cpu_data[cpu].timer_lock, &flags);
    if (t->cpu != cpu) {
      /* fired (tick set cpu=-1) or re-armed on another core meanwhile —
       * re-snapshot and decide again with the correct lock. */
      spin_unlock_irqrestore(&cpu_data[cpu].timer_lock, flags);
      continue;
    }
    if (t->pending) {
      list_del(&t->list);
      t->pending = false;
      t->cpu = -1;
    }
    spin_unlock_irqrestore(&cpu_data[cpu].timer_lock, flags);
    return;
  }
}

/*
 * timer_pending - check whether a timer is on the pending list.
 *
 * Returns t->pending.  No lock taken — caller must accept a stale snapshot
 * (the timer may fire between this read and any subsequent action).
 */
bool timer_pending(struct timer *t) { return t->pending; }
