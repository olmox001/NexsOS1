/*
 * kernel/HAL/arch/aarch64/cpu/syscall.c
 * System Call Handler
 */
#include <hal/diagnostic_protocol.h>
#include <hal/fault_protocol.h>
#include <hal/core_protocol.h>
#include <arch/pt_regs.h>
#include <hal/core.h>
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/ext4.h>
#include <kernel/fault.h>
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>

extern int process_terminate(int pid);

extern int compositor_get_window_by_pid(int pid);
extern void compositor_window_write(int win_id, const char *buf, size_t count);
extern void compositor_blit(int win_id, int x, int y, int w, int h,
                            const uint32_t *buf, int pid);
extern int compositor_create_window(int x, int y, int w, int h,
                                    const char *title, struct process *owner);
extern void compositor_draw_rect(int window_id, int x, int y, int w, int h,
                                 uint32_t color, int caller_pid);
extern void compositor_set_window_flags(int window_id, int flags);




/* Copy null-terminated string from user space safely with Page Table Switching
 */

struct pt_regs *syscall_handler(struct pt_regs *frame) {
  /* Check Exception Syndrome to distinguish SVC from Aborts */
  uint64_t esr = hal_fault_status();
  uint64_t ec = (esr >> 26) & 0x3F;

  /* EC 0x15 = SVC from AArch64 */
  if (ec != 0x15) {
    uint64_t far = hal_fault_address();
    uint64_t iss = esr & 0x1FFFFFF;

    /* Recursion guard (Phase A step 7): a fault while handling this EL0
     * fault arrives as an EL1 sync abort, but guard this path too so e.g. a
     * fault inside process_terminate stops cleanly. */
    if (nxhal_fault_enter() > 1) {
      nxhal_diag_fault("\n[FATAL] NESTED EXCEPTION in EL0-fault path EC=0x%lx ELR=%016lx — halting\n",
                   ec, frame->elr);
      hal_cpu_halt();
    }

    /* Decode the abort class → the human-readable desc passed to the common
     * fault handler (which logs the ONE unified "[FAULT] <class>: PID … pc …
     * addr … cause=0x<ESR>" line — the user-fault debug, now IDENTICAL across
     * arches; the ESR/EC/ISS detail rides in cause=0x<ESR>, FAR in addr, ELR in
     * pc).  The raw register dump is reserved for the kernel/corruption path
     * below, mirroring amd64 (DIR-06 HAL conformance: one reporting path). */
    const char *ec_name = "Unknown";
    switch (ec) {
    case 0x00:
      ec_name = "Unknown/Uncategorized";
      break;
    case 0x01:
      ec_name = "WFI/WFE";
      break;
    case 0x20:
      ec_name = "Instruction Abort (Lower EL)";
      break;
    case 0x21:
      ec_name = "Instruction Abort (Same EL)";
      break;
    case 0x24:
      ec_name = "Data Abort (Lower EL)";
      break;
    case 0x25:
      ec_name = "Data Abort (Same EL)";
      break;
    default:
      break;
    }

    /* This vector (el0_64_sync) only fires for EL0-origin exceptions: always
     * user-attributable.  A NULL return = no current task on this vector =
     * scheduler-state corruption: dump the raw syndrome and panic. */
    {
      struct pt_regs *next =
          nxhal_fault_handle(frame, 1, far, frame->elr, ec_name, esr);
      if (next)
        return next;
      nxhal_diag_error(
          "EL0 EXCEPTION: EC=0x%lx ESR=0x%lx FAR=0x%lx ELR=0x%lx ISS=0x%lx\n",
          ec, esr, far, frame->elr, iss);
      nxhal_diag_panic("Fatal EL0 exception with no current task (EC=0x%lx)", ec);
    }
  }

  /* Dispatch via agnostic core */
  return nxhal_core_syscall(frame);
}

