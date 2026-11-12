/* Pixel-format regressions run unchanged on the host and both guest ISAs. */
#include <os1_gfx.h>
#include <stdio.h>

static int exact_cases(void) {
  if (os1_gfx_argb_over(0x80ff0000U, 0) != 0x80ff0000U) return 1;
  if (os1_gfx_argb_over(0x80ff0000U, 0x800000ffU) != 0xbfaa0054U) return 2;
  if (os1_gfx_argb_premultiply(0x80ff0000U) != 0x80800000U) return 3;
  if (os1_gfx_premult_over(0x80800000U, 0x80000080U) != 0xbf80003fU) return 4;
  if (os1_gfx_argb_over(0x00ffffffU, 0x12345678U) != 0x12345678U) return 5;
  if (os1_gfx_argb_over(0xff987654U, 0) != 0xff987654U) return 6;
  if (os1_gfx_argb_premultiply(0x00ffffffU) != 0) return 7;
  if (os1_gfx_premult_over(0x01ffffffU, 0) != 0x01010101U) return 8;
  return 0;
}

static int channel_invariants(void) {
  for (uint32_t sa = 0; sa < 256; ++sa) {
    for (uint32_t da = 0; da < 256; ++da) {
      uint32_t source = (sa << 24) | 0x00ff7f39U;
      uint32_t destination = (da << 24) | 0x00398bffU;
      uint32_t out = os1_gfx_premult_over(os1_gfx_argb_premultiply(source),
          os1_gfx_argb_premultiply(destination));
      uint32_t alpha = out >> 24;
      if (alpha < sa || alpha < da || alpha > 255) return 9;
      if (((out >> 16) & 255) > alpha || ((out >> 8) & 255) > alpha ||
          (out & 255) > alpha) return 10;
      if ((os1_gfx_argb_over(source, destination) >> 24) != alpha) return 11;
    }
  }
  return 0;
}

int main(void) {
  int rc = exact_cases();
  if (rc == 0) rc = channel_invariants();
  if (printf("[gfxalphatest] straight/premultiplied alpha and 65536 pairs: %s (%d)\n",
             rc == 0 ? "PASS" : "FAIL", rc) < 0) return 1;
  if (printf("[gfxalphatest] done: %d/1 passed, %d failure(s)\n",
             rc == 0 ? 1 : 0, rc == 0 ? 0 : 1) < 0) return 1;
  return rc;
}
