#include <hal/core.h>
#include <stdint.h>

/* Preserve the existing absence-of-port-I/O contract on AArch64. */
int hal_ioport_available(void) { return 0; }
uint8_t hal_inb(uint16_t port) { (void)port; return 0U; }
uint16_t hal_inw(uint16_t port) { (void)port; return 0U; }
uint32_t hal_inl(uint16_t port) { (void)port; return 0U; }
void hal_outb(uint16_t port, uint8_t value) { (void)port; (void)value; }
void hal_outw(uint16_t port, uint16_t value) { (void)port; (void)value; }
void hal_outl(uint16_t port, uint32_t value) { (void)port; (void)value; }
