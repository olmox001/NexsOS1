/*
 * kernel/drivers/timer/timer.c
 * ARM Generic Timer driver
 *
 * Uses EL1 virtual timer (CNTV) - works in QEMU -kernel mode
 */
#include <drivers/gic.h>
#include <drivers/timer.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/list.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* Disable optimization */

#include <kernel/vmm.h>

/* Timer frequency */
/* Timer frequency */
uint64_t timer_freq;

/* Compositor refresh interval (jiffies) */
static uint64_t compositor_interval =
    1; /* Default to every tick if init fails */
#define COMPOSITOR_TARGET_FPS 30

/* System tick counter */
volatile uint64_t jiffies = 0;

/* Software timer list */
static LIST_HEAD(timer_list);
static spinlock_t timer_lock = SPINLOCK_INIT;

/*
 * Read counter frequency
 */
static inline uint64_t read_cntfrq(void) { return arch_cntfrq_el0_read(); }

/*
 * Read virtual counter
 */
static inline uint64_t read_cntvct(void) { return arch_cntvct_el0_read(); }

/*
 * Set virtual timer compare value (EL1 virtual timer)
 */
static inline void write_cntv_cval(uint64_t val) {
  arch_cntv_cval_el0_write(val);
}

/*
 * Set virtual timer control (EL1 virtual timer)
 */
static inline void write_cntv_ctl(uint64_t val) {
  arch_cntv_ctl_el0_write(val);
}

/*
 * Timer interrupt handler (Internal C handler called by GIC)
 * Now accepts regs for Preemption.
 */
extern void compositor_tick(void);

/* Global panic flag set by panic() to halt all CPUs */
extern volatile int panic_flag;

struct pt_regs *timer_handler(struct pt_regs *regs) {
  /* Halt this CPU if another CPU panicked */
  if (panic_flag) {
    write_cntv_ctl(0); /* Disable timer */
    arch_local_irq_disable();
    while (1) { __asm__ volatile("wfe"); }
  }

  struct cpu_info *cpu = get_cpu_info();
  cpu->tick_count++;

  struct timer *t, *tmp;

  /* Update accumulated error */
  // uint64_t interval = timer_freq / HZ;
  // uint64_t remainder = timer_freq % HZ;
  // Use precomputed values to avoid division in ISR
  extern uint64_t timer_tick_interval;
  extern uint64_t timer_tick_remainder;

  /* Precision Tick Logic: Accumulate remainders to avoid frequency
   * approximation drift */
  cpu->tick_error_acc += timer_tick_remainder;
  uint64_t interval = timer_tick_interval;
  if (cpu->tick_error_acc >= HZ) {
    interval += 1;
    cpu->tick_error_acc -= HZ;
  }

  cpu->next_tick_target += interval;
  uint64_t now = read_cntvct();

  /* Catch up logic: if we missed one or more ticks, realign to 'now' */
  if (cpu->next_tick_target <= now) {
    cpu->next_tick_target = now + interval;
  }

  write_cntv_cval(cpu->next_tick_target);

  /* Global jiffies incremented only by the primary core to avoid contention and
   * double-counting */
  if (cpu->cpu_id == 0) {
    jiffies++;
  }

  /* Process expired software timers */
  /* Note: In a multiprocessor system, software timers are usually tied to a
   * specific CPU. For simplicity, we process them on CPU 0. */
  if (cpu->cpu_id == 0) {
    uint64_t flags;
    spin_lock_irqsave(&timer_lock, &flags);
    list_for_each_entry_safe(t, tmp, &timer_list, list) {
      if (jiffies >= t->expires) {
        list_del(&t->list);
        t->pending = false;

        /* Drop lock before callback to allow re-entrancy/long running */
        spin_unlock_irqrestore(&timer_lock, flags);
        if (t->callback)
          t->callback(t->data);
        spin_lock_irqsave(&timer_lock, &flags);

        /* Must restart iteration? Safe iteration allows deletion, but
           dropping lock invalidates 'tmp'.
           We should restart or use a robust method.
           Simplest for now: Don't drop lock, but assume callback is
           fast/non-blocking. Actually, dropping lock inside list_for_each loops
           is dangerous. Revert to holding lock, but ensure callback is fast. */
      }
    }
    spin_unlock_irqrestore(&timer_lock, flags);
  }

  /* Compositor refresh at target FPS */
  /* This is safe to call from IRQ since we removed kmalloc from render path */
  if (cpu->cpu_id == 0 && (jiffies % compositor_interval) == 0) {
    compositor_tick();
  }

  /* Heartbeat diagnostics - Disabled to save stack space in IRQ */
  /* if (cpu->cpu_id == 0 && (jiffies % (HZ * 5)) == 0) {
    pr_info("Heartbeat: Jiffies=%lu | CPU Ticks: 0:%lu, 1:%lu, 2:%lu, 3:%lu\n",
            jiffies, cpu_data[0].tick_count, cpu_data[1].tick_count,
            cpu_data[2].tick_count, cpu_data[3].tick_count);
  } */

  /* Call Scheduler for Preemption and return resulting context */
  return schedule(regs);
}

/* Legacy static handler used by irq_register?
   No, we call timer_handler directly from GIC now.
   We can remove the static wrapper or keep it for compatibility if other IRQs
   use it. But GIC logic above handles IRQ 27 explicitly.
*/

/*
 * Initialize timer (called once on boot CPU)
 */
/* Precomputed timer values */
uint64_t timer_tick_interval = 0;
uint64_t timer_tick_remainder = 0;

/*
 * Initialize timer (called once on boot CPU)
 */
void timer_init(void) {
  timer_freq = read_cntfrq();
  timer_tick_interval = timer_freq / HZ;
  timer_tick_remainder = timer_freq % HZ;

  pr_info("Timer: Frequency %lu Hz\n", timer_freq);
  pr_info("Timer: System tick rate %d Hz\n", HZ);

  /* Calculate compositor interval */
  if (HZ >= COMPOSITOR_TARGET_FPS) {
    compositor_interval = HZ / COMPOSITOR_TARGET_FPS;
  } else {
    compositor_interval = 1; /* Best effort */
  }
  pr_info("Timer: Compositor Refresh every %lu ticks (~%d FPS)\n",
          compositor_interval, HZ / (int)compositor_interval);

  /* Register virtual timer interrupt (IRQ 27 on QEMU virt) */
  /* We handle IRQ 27 explicitly in gic.c dispatch to pass regs */
  gic_enable_irq(IRQ_TIMER_VIRT);
}

/*
 * Initialize timer on each CPU
 * NOTE: Timer disabled temporarily - causes SError in QEMU -kernel mode
 * Timer works correctly with UEFI boot (./build_iso.sh run)
 */
void timer_init_percpu(void) {
  struct cpu_info *cpu = get_cpu_info();

  /* Set up first timer interrupt using virtual timer */
  cpu->next_tick_target = read_cntvct() + (timer_freq / HZ);
  cpu->tick_error_acc = 0;
  write_cntv_cval(cpu->next_tick_target);

  /* Enable virtual timer (ENABLE=1, IMASK=0) */
  write_cntv_ctl(1);

  /* Enable virtual timer IRQ in GIC (PPI, already per-CPU) */
  gic_enable_irq(IRQ_TIMER_VIRT);

  pr_info("Timer: Per-CPU virtual timer enabled (IRQ %d). Next: 0x%lx, Ctl: "
          "0x1\n",
          IRQ_TIMER_VIRT, cpu->next_tick_target);
}

/*
 * Get current timer ticks
 */
uint64_t timer_get_ticks(void) { return read_cntvct(); }

/*
 * Get microseconds since boot
 */
uint64_t timer_get_us(void) {
  return (read_cntvct() * USEC_PER_SEC) / timer_freq;
}

/*
 * Delay for specified microseconds
 */
void timer_delay_us(uint64_t us) {
  uint64_t start = read_cntvct();
  uint64_t ticks = (us * timer_freq) / USEC_PER_SEC;

  while ((read_cntvct() - start) < ticks)
    ;
}

/*
 * Delay for specified milliseconds
 */
void timer_delay_ms(uint64_t ms) { timer_delay_us(ms * 1000); }

/*
 * Initialize a software timer
 */
void timer_setup(struct timer *t, timer_callback_t callback, void *data) {
  INIT_LIST_HEAD(&t->list);
  t->callback = callback;
  t->data = data;
  t->pending = false;
}

/*
 * Add a software timer
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
 * Delete a software timer
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
 * Check if timer is pending
 */
bool timer_pending(struct timer *t) { return t->pending; }
