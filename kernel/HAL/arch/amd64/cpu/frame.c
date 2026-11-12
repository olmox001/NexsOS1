#include <hal/frame.h>
#include <arch/pt_regs.h>

size_t hal_frame_size(void) { return sizeof(struct pt_regs); }

uint64_t hal_frame_syscall_num(hal_frame_t *frame) {
  return pt_regs_syscall_num(frame);
}

uint64_t hal_frame_arg(hal_frame_t *frame, int index) {
  return pt_regs_arg(frame, index);
}

void hal_frame_set_return(hal_frame_t *frame, long value) {
  pt_regs_set_return(frame, value);
}

uint64_t hal_frame_pc(hal_frame_t *frame) { return pt_regs_pc(frame); }

void hal_frame_set_pc(hal_frame_t *frame, uint64_t value) {
  pt_regs_set_pc(frame, value);
}

uint64_t hal_frame_user_sp(hal_frame_t *frame) {
  return pt_regs_user_sp(frame);
}

void hal_frame_set_user_sp(hal_frame_t *frame, uint64_t value) {
  pt_regs_set_user_sp(frame, value);
}

void hal_frame_retry_syscall(hal_frame_t *frame) {
  pt_regs_retry_syscall(frame);
}

void hal_frame_init_user_task(hal_frame_t *frame, uint64_t entry,
                              uint64_t user_sp) {
  pt_regs_init_user_task(frame, entry, user_sp);
}

void hal_frame_set_user_args(hal_frame_t *frame, uint64_t argc,
                             uint64_t argv) {
  pt_regs_set_user_args(frame, argc, argv);
}

void hal_frame_init_kernel_task(hal_frame_t *frame, uint64_t entry,
                                uint64_t kernel_sp) {
  pt_regs_init_kernel_task(frame, entry, kernel_sp);
}

void hal_frame_snapshot(const hal_frame_t *frame,
                        struct hal_frame_snapshot *snapshot) {
  if (!snapshot)
    return;

  snapshot->pc = 0;
  snapshot->sp = 0;
  snapshot->fp = 0;
  snapshot->lr = 0;
  snapshot->status = 0;
  snapshot->origin = HAL_FRAME_ORIGIN_UNKNOWN;
  if (!frame)
    return;

  snapshot->pc = frame->rip;
  snapshot->fp = frame->rbp;
  snapshot->lr = frame->rip;
  snapshot->status = frame->cs;
  if ((frame->cs & 3u) == 3u) {
    snapshot->origin = HAL_FRAME_ORIGIN_USER;
    snapshot->sp = frame->rsp;
  } else {
    snapshot->origin = HAL_FRAME_ORIGIN_KERNEL;
    snapshot->sp = (uint64_t)(uintptr_t)&frame->rsp;
  }
}
