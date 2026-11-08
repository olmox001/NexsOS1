/*
 * kernel/core/timer.c
 * Generic Timer Logic (Jiffies, Software Timers, Scheduling)
 *
 * This file is the arch-agnostic timer layer.  It owns:
 *   - kernel_timer_tick(): the central per-tick ISR hook called by the
 *     arch-specific timer IRQ handler (aarch64: drivers/timer/timer.c;
 *     amd64: arch/amd64/platform/platform.c via PIT/APIC).
 *   - Per-CPU software timer lists (struct timer), fired by each CPU's tick.
 *   - Per-CPU clock accounting reconciled against the free-running HAL counter.
 *   - Arch-specific per-CPU timer init via __attribute__((weak)) stubs
 *     that each arch overrides.
 *
 * Layering:
 *   arch IRQ -> kernel_timer_tick() -> per-CPU timer callbacks -> schedule()
 *   compositor/input work stays in the SYS_COMPOSITOR_RENDER process path.
 *
 * Key invariants:
 *   - CPU 0 is the sole writer of global jiffies; every CPU reads it through
 *     atomic snapshots and processes only its own per-CPU timer_list.
 *   - timer_lock protects each CPU's timer_list; callbacks run under that
 *     lock. timer_del() keeps the owner published until callback completion,
 *     so teardown can synchronously wait before freeing callback data.
 *   - The weak timer_handler() stub returns regs unchanged; per-arch overrides
 *     may perform additional bookkeeping (e.g. EOI).
 *
 * Known issues:
 *   ARCH-03 is now limited to the legacy amd64 timer_get_us() API, which still
 *   returns jiffies*1000 (millisecond granularity, not microseconds). The
 *   monotonic syscall path uses mono_ns() and is not affected; platform.c keeps
 *   the legacy stub until the planned B4 cleanup.
 */
#include <drivers/timer.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/list.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

/* USB HID polling and input dispatch run from SYS_COMPOSITOR_RENDER in
 * process context; the timer IRQ path only accounts time, fires timers and
 * invokes the scheduler. */
extern volatile int panic_flag;
/* NOTE(SCHED-STACK-ISO): compositor rendering is not driven from the timer
 * tick. SYS_COMPOSITOR_RENDER runs in process context, so the renderer never
 * nests on an interrupted task's kernel stack. compositor_tick() remains as a
 * legacy trylock helper in compositor.c but is not wired into this IRQ path. */

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
 * deliberately NOT latched: on amd64 hal_timer_get_freq() returns a 1 GHz
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
  uint64_t freq = hal_timer_get_freq();
  if (freq == 0)
    freq = 1; /* guard div-by-zero before any frequency source is up */
  return (uint64_t)(((__uint128_t)counts * NSEC_PER_SEC) / freq);
}

uint64_t mono_ns(void) {
  if (!clk_ready) {
    clk_boot_count = hal_timer_get_count();
    clk_ready = 1;
  }
  return timer_counts_to_ns(hal_timer_get_count() - clk_boot_count);
}

uint64_t timer_get_ns(void) { return mono_ns(); }

/*
 * timer_percpu_tick_current - Tier 2 per-CPU clock accounting + rearm (HAL-driven).
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
 *   4. Reprogram the compare via hal_timer_set_compare(): a real reprogram on
 *      one-shot timers (aarch64 CNTV_CVAL), a no-op on periodic timers (amd64
 *      LAPIC), so this serves both arches unchanged.
 *
 * Locking: touches only this CPU's cpu_info; no lock. IRQ context: YES.
 */
uint64_t timer_percpu_tick_current(void) {
  struct cpu_info *cpu = get_cpu_info();
  uint64_t freq = hal_timer_get_freq();
  uint64_t interval = freq / HZ;
  uint64_t remainder = freq % HZ;

  cpu->tick_error_acc += remainder;
  if (cpu->tick_error_acc >= HZ) {
    interval += 1;
    cpu->tick_error_acc -= HZ;
  }

  cpu->next_tick_target += interval;
  uint64_t now = hal_timer_get_count();
  if (cpu->next_tick_target <= now)
    cpu->next_tick_target = now + interval;

  hal_timer_set_compare(cpu->next_tick_target);
  return cpu->next_tick_target;
}

/*
 * timer_percpu_arm_current - seed this CPU's tick schedule on timer bring-up.
 *
 * Sets next_tick_target one interval out from the live counter, clears the
 * fractional accumulator, and programs the first compare via the HAL. Called
 * once per CPU from the arch per-CPU timer init (aarch64 timer_init_percpu,
 * amd64 lapic_timer_setup); the arch path still owns enabling the timer and
 * unmasking its IRQ.
 */
uint64_t timer_percpu_arm_current(void) {
  struct cpu_info *cpu = get_cpu_info();
  uint64_t freq = hal_timer_get_freq();
  cpu->next_tick_target = hal_timer_get_count() + freq / HZ;
  cpu->tick_error_acc = 0;
  hal_timer_set_compare(cpu->next_tick_target);
  return cpu->next_tick_target;
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
__attribute__((weak)) hal_frame_t *timer_handler(hal_frame_t *regs) {
  return regs;
}

/*
 * kernel_timer_tick - central per-tick entry point called from the timer IRQ.
 *
 * Called on every CPU on every timer interrupt.  Responsibilities:
 *   1. Halt this CPU immediately if another CPU has set panic_flag.
 *   2. Increment cpu->tick_count (per-CPU; no lock needed).
 *   3. CPU 0 only: increment the global jiffies counter.
 *   4. Every CPU: fire expired software timers from its local timer_list.
 *   5. CPU 0 only: call compositor_tick() every compositor_interval ticks.
 *   6. All CPUs: invoke schedule(regs) for preemptive multitasking.
 *
 * Locking: each CPU acquires its own timer_lock (irqsave) around its local
 *          software timer list; no other locks held on entry.
 * IRQ context: yes — called directly from the architecture timer IRQ handler.
 * Returns: the pt_regs pointer returned by schedule() (may differ from regs
 *          if a context switch occurs).
 */
hal_frame_t *kernel_timer_tick(hal_frame_t *regs) {
  /* Halt this CPU if another CPU panicked */
  if (panic_flag) {
    hal_timer_control(0);
    hal_cpu_halt();
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
    uint64_t current_jiffies =
        __atomic_load_n(&jiffies, __ATOMIC_RELAXED);
    if (expected > current_jiffies)
      __atomic_store_n(&jiffies, expected, __ATOMIC_RELEASE);
  }

  /* Fire THIS CPU's expired software timers — every CPU, against the global
   * jiffies clock — so a process that slept on this core is woken here, locally.
   * Callbacks run inside this CPU's timer_lock and must not call timer_add/_del
   * or anything that re-acquires it. */
  {
    struct timer *t, *tmp;
    uint64_t tflags;
    uint64_t current_jiffies =
        __atomic_load_n(&jiffies, __ATOMIC_ACQUIRE);
    spin_lock_irqsave(&cpu->timer_lock, &tflags);
    list_for_each_entry_safe(t, tmp, &cpu->timer_list, list) {
      if (current_jiffies >= t->expires) {
        /* Detach and restore the self-linked idle state. Besides making
         * list_empty() truthful, this lets a fired timer be safely re-armed
         * without relying on stale NULL links left by list_del(). */
        list_del_init(&t->list);
        __atomic_store_n(&t->pending, false, __ATOMIC_RELEASE);
        /* Keep the owner CPU published while the callback is executing.
         * timer_del() snapshots this owner and takes its timer_lock; because
         * callbacks run under that lock, a remote cancellation waits until the
         * callback has finished before the process/opaque data can be freed. */
        if (t->callback)
          t->callback(t->data);
        __atomic_store_n(&t->cpu, -1, __ATOMIC_RELEASE);
      }
    }
    spin_unlock_irqrestore(&cpu->timer_lock, tflags);
  }

  /* Hard-IRQ path deliberately does not poll USB or dispatch input.
   * input_drain() can enter compositor/window code and contend on
   * compositor_lock; doing that here can stall timer delivery behind a
   * long terminal/compositor critical section. USB polling is device work,
   * not a timer-IRQ responsibility. PID 1 performs both in its serialized
   * SYS_COMPOSITOR_RENDER process-context frame loop. */

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
  __atomic_store_n(&t->pending, false, __ATOMIC_RELAXED);
  __atomic_store_n(&t->cpu, -1, __ATOMIC_RELAXED); /* fully quiescent */
}

/*
 * timer_add - schedule a software timer to fire at tick 'expires'.
 *
 * Adds t to the tail of timer_list.  'expires' is an absolute jiffies value;
 * use (jiffies + delay_ticks) to express a relative delay.
 * The timer will fire on the next kernel_timer_tick() on its recorded owner
 * CPU after jiffies >= expires.
 *
 * Locking: synchronously cancels any prior queue/callback on its owner CPU,
 *          then acquires the current CPU's timer_lock (irqsave). It may spin
 *          until a remote callback completes. Do not call from a timer callback
 *          (callbacks execute under timer_lock).
 * Side effects: links t on the current CPU and sets t->pending = true.
 */
void timer_add(struct timer *t, uint64_t expires) {
  /* Re-arm through the synchronous cancel path, even when pending is already
   * false.  A fired timer keeps its owner CPU published until its callback
   * returns, so timer_del() also waits for an in-flight callback before this
   * timer's list node or opaque data can be reused.  This additionally keeps
   * TIMER-UAF-01's single-link invariant: the node is detached before reuse. */
  timer_del(t);

  struct cpu_info *c = get_cpu_info();
  uint64_t flags;
  spin_lock_irqsave(&c->timer_lock, &flags);
  t->expires = expires;
  /* Publish the owner before pending.  timer_del() reads cpu atomically, then
   * serializes with this list update using the owner's timer_lock. */
  __atomic_store_n(&t->cpu, (int)c->cpu_id, __ATOMIC_RELAXED);
  list_add_tail(&t->list, &c->timer_list);
  __atomic_store_n(&t->pending, true, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&c->timer_lock, flags);
}

/*
 * timer_del - synchronously cancel a timer or wait for its callback to finish.
 *
 * A queued timer is unlinked and its list node reinitialized on its owner CPU.
 * Once the tick dequeues it,
 * pending becomes false but cpu remains the owner until the callback returns.
 * Therefore a remote timer_del() still finds the owner, waits on that CPU's
 * timer_lock, revalidates the owner, and returns only after the callback is
 * quiescent.  This is required before process teardown can free callback data.
 *
 * Locking: acquires the owner timer_lock (irqsave); retries if ownership moved.
 * IRQ context: safe, but may spin until a remote callback completes.
 * Serialization: callers must serialize this operation against timer_add() and
 * timer_setup() for the same timer; concurrent timer_del() calls are safe.
 * Callback restriction: callbacks execute under timer_lock and must not call
 * timer_add() or timer_del() on any timer protected by that same lock.
 */
void timer_del(struct timer *t) {
  uint64_t flags;
  for (;;) {
    int cpu = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
    if (cpu < 0 || cpu >= MAX_CPUS)
      return; /* no queued timer and no callback in flight */

    spin_lock_irqsave(&cpu_data[cpu].timer_lock, &flags);
    int owner = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
    if (owner != cpu) {
      /* The owner completed or moved while this CPU lock was acquired. */
      spin_unlock_irqrestore(&cpu_data[cpu].timer_lock, flags);
      continue;
    }
    if (__atomic_load_n(&t->pending, __ATOMIC_ACQUIRE)) {
      list_del_init(&t->list);
      __atomic_store_n(&t->pending, false, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&t->cpu, -1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&cpu_data[cpu].timer_lock, flags);
    return;
  }
}

/*
 * timer_pending - check whether a timer is on the pending list.
 *
 * Returns whether t is linked on an owner timer_list.  No lock is taken, so
 * the result may be stale. It is false while a dequeued callback is executing;
 * it is NOT a quiescence/lifetime check. Use timer_del() before freeing callback
 * data or reinitialising the timer.
 */
bool timer_pending(struct timer *t) {
  return __atomic_load_n(&t->pending, __ATOMIC_ACQUIRE);
}
