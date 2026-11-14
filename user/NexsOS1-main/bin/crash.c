/*
 * user/bin/crash.c
 * Kernel Fault Handler Test — intentional null pointer dereference
 *
 * This process deliberately dereferences address 0 to trigger a hardware
 * exception (data abort on AArch64, page fault on x86-64).  Its purpose is
 * to verify that the kernel's fault handler correctly catches the exception,
 * terminates the offending process, and does not corrupt the rest of the
 * system.
 *
 * The `volatile` qualifier on p prevents the compiler from optimising away
 * the write even in optimised builds (-O2).
 *
 * Known issues:
 *   USR-BLOAT-01 (W2 BAD-IMPL·PERF) crash.elf is ~502KB [verified], almost
 *                entirely DWARF debug data and stb_image from lib.o — none of
 *                which is used by this 9-line binary.
 *   USR-BLOAT-02 (W2 BAD-IMPL) -g debug symbols and -fno-omit-frame-pointer
 *                are retained with no strip step.
 */
#include <os1.h>

/*
 * main - triggers a null pointer dereference and does not return.
 *
 * After the write to address 0 the kernel should fault-terminate this process.
 * The `return 0` is unreachable dead code.
 */
int main(void) {
  printf("Crash test starting...\n");
  /* Trigger a null pointer dereference to test kernel fault handling */
  volatile int *p = (int *)0;
  *p = 123;   /* Expected: data abort / page fault at address 0x0 */
  return 0;
}
