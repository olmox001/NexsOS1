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

/* Functions */
void timer_init(void);
void timer_init_percpu(void);
uint64_t timer_get_ticks(void);
uint64_t timer_get_us(void);
void timer_delay_us(uint64_t us);
void timer_delay_ms(uint64_t ms);

/* Internal Handlers */
struct pt_regs;
struct pt_regs *timer_handler(struct pt_regs *regs);

/* Timer callback type */
typedef void (*timer_callback_t)(void *data);

/* Software timer structure */
struct timer {
  struct list_head list;
  uint64_t expires; /* Expiration time in jiffies */
  timer_callback_t callback;
  void *data;
  bool pending;
};

/* Software timer functions */
void timer_setup(struct timer *t, timer_callback_t callback, void *data);
void timer_add(struct timer *t, uint64_t expires);
void timer_del(struct timer *t);
bool timer_pending(struct timer *t);

#endif /* _DRIVERS_TIMER_H */
