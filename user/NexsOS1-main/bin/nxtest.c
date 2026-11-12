/*
 * user/bin/nxtest.c
 * W^X user-space test (ELF-02): attempts to execute code from the stack.
 *
 * Expected behaviour with W^X enforced: the indirect call below takes an
 * instruction-fetch fault (UXN on aarch64, NX on amd64), the kernel kills
 * THIS process and the shell survives — the FAIL line below never prints.
 * If the FAIL line appears, user stack pages are executable again.
 */
#include <os1.h>

int main(void) {
  /* A single return instruction, written into a stack buffer. */
  unsigned char code[8];
#ifdef __aarch64__
  /* ret = 0xd65f03c0 (little endian) */
  code[0] = 0xc0; code[1] = 0x03; code[2] = 0x5f; code[3] = 0xd6;
#else
  code[0] = 0xc3; /* ret */
#endif

  printf("[nxtest] executing from the stack (expect: this process dies)\n");
  flush();

  /* Object→function pointer via uintptr_t (ISO C pedantic-clean). */
  uintptr_t addr = (uintptr_t)code;
  void (*f)(void) = (void (*)(void))addr;
  f();

  printf("[nxtest] FAIL: stack code executed - W^X is NOT enforced\n");
  return 1;
}
