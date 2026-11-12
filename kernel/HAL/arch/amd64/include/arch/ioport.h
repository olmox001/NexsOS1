#ifndef NXHAL_AMD64_IOPORT_H
#define NXHAL_AMD64_IOPORT_H

#include <stdint.h>

/* Authoritative x86 port-I/O primitives, also used by the reboot provider.
 * Keep assembly in one ISA-owned translation point; public HAL API is
 * implemented by arch/amd64/cpu/ioport.c. */

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
  uint8_t val;
  __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
  return val;
}
static inline void outw(uint16_t port, uint16_t val) {
  __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
  uint16_t val;
  __asm__ __volatile__("inw %1, %0" : "=a"(val) : "Nd"(port));
  return val;
}
static inline void outl(uint16_t port, uint32_t val) {
  __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
  uint32_t val;
  __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
  return val;
}

#endif /* NXHAL_AMD64_IOPORT_H */
