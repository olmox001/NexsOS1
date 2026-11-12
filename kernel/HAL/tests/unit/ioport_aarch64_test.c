#include <hal/core.h>
#include <stdint.h>

static int test_unavailable_reads(void) {
  if (hal_ioport_available() != 0)
    return 1;
  if (hal_inb(UINT16_MAX) != 0U ||
      hal_inw(UINT16_MAX) != 0U ||
      hal_inl(UINT16_MAX) != 0U)
    return 2;
  return 0;
}

static int test_unavailable_writes(void) {
  hal_outb(UINT16_MAX, UINT8_MAX);
  hal_outw(UINT16_MAX, UINT16_MAX);
  hal_outl(UINT16_MAX, UINT32_MAX);
  if (hal_ioport_available() != 0 || hal_inb(0U) != 0U ||
      hal_inw(0U) != 0U || hal_inl(0U) != 0U)
    return 1;
  return 0;
}

int main(void) {
  int ret = test_unavailable_reads();
  if (ret != 0) return ret;
  ret = test_unavailable_writes();
  if (ret != 0) return ret + 10;
  return 0;
}
