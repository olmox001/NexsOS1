/*
 * kernel/include/drivers/timer.h
 * ARM Generic Timer
 */
#ifndef _DRIVERS_TIMER_H
#define _DRIVERS_TIMER_H

#include <kernel/list.h>
#include <kernel/types.h>

/* Timer frequency (will be read from CNTFRQ_EL0) */
extern uint64_t timer_freq;

/* System ticks since boot */
extern volatile uint64_t jiffies;

/* Timer configuration */
#define HZ                                                                     \
  100 /* Reduced frequency for better performance in non-optimized kernel */

/* Time conversion macros */
#define MSEC_PER_SEC 1000UL
#define USEC_PER_SEC 1000000UL
#define NSEC_PER_SEC 1000000000UL

#define msecs_to_jiffies(m) ((uint64_t)(m) * HZ / MSEC_PER_SEC)
#define jiffies_to_msecs(j) ((uint64_t)(j) * MSEC_PER_SEC / HZ)

/* Nanoseconds per scheduler tick (the jiffies granularity). The whole 3-tier
 * model (docs/TIMER-MODEL.md) reconciles the tick clocks against this real-time
 * stride derived from the hardware counter. */
#define NS_PER_TICK (NSEC_PER_SEC / HZ)

/* Functions */
void timer_init(void);
void timer_init_percpu(void);
uint64_t timer_get_ticks(void);
uint64_t timer_get_us(void);
void timer_delay_us(uint64_t us);
void timer_delay_ms(uint64_t ms);

/* mono_ns - monotonic nanoseconds since boot, derived from the free-running
 * hardware counter (arch_timer_get_count / arch_timer_get_freq). This is the
 * arch-neutral REAL-TIME reference every timer tier compares against to recover
 * lost ticks; jiffies is a cheap integer view of it. Defined in core/timer.c. */
uint64_t mono_ns(void);
/* timer_get_ns - alias of mono_ns() (the canonical real-time clock); replaces
 * the per-arch timer_get_us() path so amd64 stops returning jiffies*1000. */
uint64_t timer_get_ns(void);
/* timer_counts_to_ns - scale a raw hardware-counter delta to nanoseconds.
 * The scheduler accumulates per-process CPU time in raw counter units (a cheap
 * subtraction, no divide in the hot path) and converts via this only on read. */
uint64_t timer_counts_to_ns(uint64_t counts);

/* Tier 2 per-CPU clock — HAL-driven, arch-neutral (docs/TIMER-MODEL.md §3).
 * timer_percpu_tick(): per-tick drift accounting + compare reprogram against the
 *   hardware counter (arch_timer_get_freq/count/set_compare); catch-up clamp
 *   recovers lost ticks. Called from each arch's timer IRQ before the scheduler
 *   tick. arch_timer_set_compare() reprograms a one-shot timer (aarch64) and is
 *   a no-op for a periodic timer (amd64), so the one routine serves both.
 * timer_percpu_arm(): seed the per-CPU schedule on this core's timer bring-up. */
struct cpu_info;
void timer_percpu_tick(struct cpu_info *cpu);
void timer_percpu_arm(struct cpu_info *cpu);

/* Internal Handlers */
struct pt_regs;
struct pt_regs *timer_handler(struct pt_regs *regs);
struct pt_regs *kernel_timer_tick(struct pt_regs *regs);

/* Timer callback type */
typedef void (*timer_callback_t)(void *data);

/* Software timer structure */
struct timer {
  struct list_head list;
  uint64_t expires; /* Expiration time in jiffies (global monotonic base) */
  timer_callback_t callback;
  void *data;
  bool pending;
  int cpu; /* CPU whose per-CPU timer_list this timer is armed on (-1 = none) */
};

/* Software timer functions */
void timer_setup(struct timer *t, timer_callback_t callback, void *data);
void timer_add(struct timer *t, uint64_t expires);
void timer_del(struct timer *t);
bool timer_pending(struct timer *t);

#endif /* _DRIVERS_TIMER_H */
