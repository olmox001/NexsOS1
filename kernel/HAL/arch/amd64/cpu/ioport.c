#include <hal/core.h>
#include <arch/ioport.h>
#include <stdint.h>

/* Native AMD64 port-I/O: transport, not device policy. */
int hal_ioport_available(void) { return 1; }
uint8_t hal_inb(uint16_t port) { return inb(port); }
uint16_t hal_inw(uint16_t port) { return inw(port); }
uint32_t hal_inl(uint16_t port) { return inl(port); }
void hal_outb(uint16_t port, uint8_t value) { outb(port, value); }
void hal_outw(uint16_t port, uint16_t value) { outw(port, value); }
void hal_outl(uint16_t port, uint32_t value) { outl(port, value); }
