#ifndef _KERNEL_HAL_CORE_ADAPTER_H
#define _KERNEL_HAL_CORE_ADAPTER_H

#include <stdint.h>

/* Cold boot ABI implementation called by NX-HAL boot assembly. */
void nxcore_hal_boot_entry(uint64_t boot_arg0, uint64_t boot_arg1)
    __attribute__((noreturn));

#endif /* _KERNEL_HAL_CORE_ADAPTER_H */
