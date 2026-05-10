void _start(void) {
  /* 1. Write 'T' (84) to UART (0x09000000) */
  asm volatile("mov x0, #0x09000000\n"
               "mov w1, #84\n"
               "str w1, [x0]\n"
               "ret\n"
               :
               :
               : "memory");
}
