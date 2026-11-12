/* Isolated competing process; uses the same architecture test engine. */
#include <stdint.h>
long os1_simd_context_probe(uint32_t seed);
int main(void) {
  return (int)os1_simd_context_probe(UINT32_C(0xc17e492b));
}
