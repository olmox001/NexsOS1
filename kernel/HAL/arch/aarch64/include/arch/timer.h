#ifndef NXHAL_ARM64_TIMER_H
#define NXHAL_ARM64_TIMER_H

#include <stdint.h>

#define NXHAL_ARM64_VIRTUAL_TIMER_IRQ UINT32_C(27)

void nxhal_arm64_timer_global_init(void);

#endif /* NXHAL_ARM64_TIMER_H */
