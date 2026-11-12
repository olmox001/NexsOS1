#ifndef OS1DRIVER_TIMER_H
#define OS1DRIVER_TIMER_H

#include <kernel/timer.h>

#define HZ OS1LIBKERNEL_TIMER_HZ
#define MSEC_PER_SEC UINT64_C(1000)
#define USEC_PER_SEC UINT64_C(1000000)
#define NSEC_PER_SEC OS1LIBKERNEL_NSEC_PER_SEC
#define NS_PER_TICK OS1LIBKERNEL_NSEC_PER_TICK

extern volatile uint64_t jiffies;

/* Transitional provider-local utilities. New Core code must use kernel/timer.h. */
void timer_init(void);
uint64_t timer_get_ticks(void);
void timer_delay_us(uint64_t us);
void timer_delay_ms(uint64_t ms);

#endif /* OS1DRIVER_TIMER_H */
