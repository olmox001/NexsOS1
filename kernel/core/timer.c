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
extern volatile int panic_flag;
/* compositor_interval: tick stride between compositor_tick() calls; set once
 * from HZ and COMPOSITOR_TARGET_FPS on the first tick processed by CPU 0. */
static uint64_t compositor_interval = 1;
#define COMPOSITOR_TARGET_FPS 30

/* timer_list: doubly-linked list of pending software timers, sorted by
 * insertion order (not expiry order — O(n) scan on each tick).
 * Protected by timer_lock. */
static LIST_HEAD(timer_list);
/* timer_lock: guards timer_list; held while firing callbacks. */
static spinlock_t timer_lock = SPINLOCK_INIT;

/* jiffies and timer_freq are defined in drivers/timer/timer.c for AArch64
 * or in arch/amd64/platform/platform.c for AMD64. */
extern volatile uint64_t jiffies;

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

  /* Global jiffies incremented only by the primary core */
  if (cpu->cpu_id == 0) {
    jiffies++;
  }

  /* Process software timers and compositor on CPU 0 */
  if (cpu->cpu_id == 0) {
    struct timer *t, *tmp;
    uint64_t flags;
    spin_lock_irqsave(&timer_lock, &flags);
    /* Walk the list safely (list_for_each_entry_safe allows deletion).
     * Callbacks are fired *inside* timer_lock — they must not call
     * timer_add/timer_del or any function that acquires timer_lock. */
    list_for_each_entry_safe(t, tmp, &timer_list, list) {
      if (jiffies >= t->expires) {
        list_del(&t->list);
        t->pending = false;
        if (t->callback)
          t->callback(t->data);
      }
    }
    spin_unlock_irqrestore(&timer_lock, flags);

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
  uint64_t flags;
  spin_lock_irqsave(&timer_lock, &flags);
  t->expires = expires;
  t->pending = true;
  list_add_tail(&t->list, &timer_list);
  spin_unlock_irqrestore(&timer_lock, flags);
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
  spin_lock_irqsave(&timer_lock, &flags);
  if (t->pending) {
    list_del(&t->list);
    t->pending = false;
  }
  spin_unlock_irqrestore(&timer_lock, flags);
}

/*
 * timer_pending - check whether a timer is on the pending list.
 *
 * Returns t->pending.  No lock taken — caller must accept a stale snapshot
 * (the timer may fire between this read and any subsequent action).
 */
bool timer_pending(struct timer *t) { return t->pending; }
