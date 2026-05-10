/*
 * kernel/lib/stack_protector.c
 * Stack smashing protection (SSP) support
 */
#include <kernel/printk.h>
#include <stdint.h>

/* Random stack canary - should be randomized at boot */
uintptr_t __stack_chk_guard = 0x595e9fbd94fda766;

void __stack_chk_fail(void);

/*
 * Called when stack corruption is detected
 */
void __stack_chk_fail(void) {
  uint64_t ret_addr = (uint64_t)__builtin_return_address(0);
  panic("SSP: Stack Smash at 0x%lx\n", ret_addr);
}
