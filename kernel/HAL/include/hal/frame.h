#ifndef NXHAL_FRAME_H
#define NXHAL_FRAME_H

#include <stddef.h>
#include <stdint.h>

/*
 * Opaque saved-execution frame crossing the HAL/kernel boundary.
 * The concrete structure and assembly offsets remain provider-private.
 */
struct pt_regs;
typedef struct pt_regs hal_frame_t;

enum hal_frame_origin {
  HAL_FRAME_ORIGIN_UNKNOWN = 0,
  HAL_FRAME_ORIGIN_USER = 1,
  HAL_FRAME_ORIGIN_KERNEL = 2,
};

struct hal_frame_snapshot {
  uint64_t pc;
  uint64_t sp;
  uint64_t fp;
  uint64_t lr;
  uint64_t status;
  enum hal_frame_origin origin;
};

size_t hal_frame_size(void);
uint64_t hal_frame_syscall_num(hal_frame_t *frame);
uint64_t hal_frame_arg(hal_frame_t *frame, int index);
void hal_frame_set_return(hal_frame_t *frame, long value);
uint64_t hal_frame_pc(hal_frame_t *frame);
void hal_frame_set_pc(hal_frame_t *frame, uint64_t value);
uint64_t hal_frame_user_sp(hal_frame_t *frame);
void hal_frame_set_user_sp(hal_frame_t *frame, uint64_t value);
void hal_frame_retry_syscall(hal_frame_t *frame);
void hal_frame_init_user_task(hal_frame_t *frame, uint64_t entry,
                              uint64_t user_sp);
void hal_frame_set_user_args(hal_frame_t *frame, uint64_t argc,
                             uint64_t argv);
void hal_frame_init_kernel_task(hal_frame_t *frame, uint64_t entry,
                                uint64_t kernel_sp);
void hal_frame_snapshot(const hal_frame_t *frame,
                        struct hal_frame_snapshot *snapshot);

#endif /* NXHAL_FRAME_H */
