#ifndef NX_KERNEL_NXPAX_H
#define NX_KERNEL_NXPAX_H

#include <stdint.h>

/* Core disposition; HAL remains the exception fact recorder. */
void nxpax_kernel_record(int32_t pid, uint32_t kind, uint64_t pc,
                         uint64_t address, uint64_t syndrome,
                         uint32_t flags);

#endif
