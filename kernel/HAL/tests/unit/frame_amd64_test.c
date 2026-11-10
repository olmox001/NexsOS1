#include "../../arch/amd64/include/arch/pt_regs.h"
#include <hal/frame.h>
#include <stdint.h>
#include <string.h>

static int frame_user_checks(void) {
  struct pt_regs frame;
  struct hal_frame_snapshot snapshot;
  if (memset(&frame, 0, sizeof(frame)) != &frame)
    return 99;
  if (hal_frame_size() != sizeof(frame))
    return 1;
  hal_frame_init_user_task(&frame, UINT64_C(0x4000), UINT64_C(0x9000));
  hal_frame_set_user_args(&frame, 7U, 9U);
  if (frame.rdi != 7U || frame.rsi != 9U || frame.cs != UINT64_C(0x23))
    return 2;
  frame.rax = 51U;
  frame.rdx = 53U;
  frame.r10 = 57U;
  if (hal_frame_syscall_num(&frame) != 51U ||
      hal_frame_arg(&frame, 0) != 7U ||
      hal_frame_arg(&frame, 1) != 9U ||
      hal_frame_arg(&frame, 2) != 53U ||
      hal_frame_arg(&frame, 3) != 57U ||
      hal_frame_arg(&frame, -1) != 0U)
    return 3;
  hal_frame_set_return(&frame, -5L);
  if (frame.rax != (uint64_t)-5L)
    return 4;
  hal_frame_snapshot(&frame, &snapshot);
  if (snapshot.origin != HAL_FRAME_ORIGIN_USER ||
      snapshot.pc != UINT64_C(0x4000) ||
      snapshot.sp != UINT64_C(0x9000))
    return 5;
  hal_frame_set_pc(&frame, UINT64_C(0x4020));
  hal_frame_retry_syscall(&frame);
  hal_frame_set_user_sp(&frame, UINT64_C(0x9050));
  if (hal_frame_pc(&frame) != UINT64_C(0x401E) ||
      hal_frame_user_sp(&frame) != UINT64_C(0x9050))
    return 6;
  return 0;
}

static int frame_kernel_checks(void) {
  struct pt_regs frame;
  struct hal_frame_snapshot snapshot;
  if (memset(&frame, 0, sizeof(frame)) != &frame)
    return 99;
  hal_frame_init_kernel_task(&frame, UINT64_C(0x8000), UINT64_C(0xB000));
  frame.rbp = UINT64_C(0xBA00);
  hal_frame_snapshot(&frame, &snapshot);
  if (snapshot.origin != HAL_FRAME_ORIGIN_KERNEL ||
      snapshot.pc != UINT64_C(0x8000) ||
      snapshot.fp != UINT64_C(0xBA00) ||
      snapshot.sp != (uint64_t)(uintptr_t)&frame.rsp)
    return 1;
  hal_frame_snapshot(NULL, &snapshot);
  if (snapshot.origin != HAL_FRAME_ORIGIN_UNKNOWN ||
      snapshot.pc != 0U || snapshot.sp != 0U)
    return 2;
  hal_frame_snapshot(&frame, NULL);
  return 0;
}

int main(void) {
  int result = frame_user_checks();
  if (result != 0)
    return result;
  result = frame_kernel_checks();
  if (result != 0)
    return 100 + result;
  return 0;
}
