#include <os1.h>

int main(void) {
  printf("Crash test starting...\n");
  /* Trigger a null pointer dereference to test kernel fault handling */
  volatile int *p = (int *)0;
  *p = 123;
  return 0;
}
