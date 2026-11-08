#ifndef _KERNEL_TIMER_H
#define _KERNEL_TIMER_H

#include <hal/frame.h>
#include <kernel/sched.h>

void timer_init(void);
hal_frame_t *timer_handler(hal_frame_t *regs);

#endif
