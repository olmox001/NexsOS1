#include <os1_gfx.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  static const int expected_size[3] = {16, 14, 14};
  for (int shape = 0; shape < 3; ++shape) {
    for (int side = 0; side < 2; ++side) {
      struct os1_gfx_button_geometry g = {0};
      os1_gfx_button_geometry(100, 300, 200, 24, shape, side, &g);
      CHECK(g.size == expected_size[shape]);
      CHECK(g.top == 200 + (24 - expected_size[shape]) / 2);
      if (side == 1) {
        CHECK(g.close_cx == 396 - expected_size[shape] / 2);
        CHECK(g.bg_cx == g.close_cx - expected_size[shape] - 6);
      } else {
        CHECK(g.close_cx == 100 + 4 + expected_size[shape] / 2);
        CHECK(g.bg_cx == g.close_cx + expected_size[shape] + 6);
      }
      CHECK(os1_gfx_button_hit(&g, g.close_cx, g.top) == OS1_GFX_BUTTON_CLOSE);
      CHECK(os1_gfx_button_hit(&g, g.bg_cx, g.top) == OS1_GFX_BUTTON_BACKGROUND);
      CHECK(os1_gfx_button_hit(&g, (g.close_cx + g.bg_cx) / 2, g.top) ==
            OS1_GFX_BUTTON_NONE);
      CHECK(os1_gfx_button_hit(&g, g.close_cx, g.top - 1) ==
            OS1_GFX_BUTTON_NONE);
      CHECK(os1_gfx_button_pixel(&g, shape, g.close_cx,
                                 g.top + g.size / 2, 0xffef3340u,
                                 0xfffebc2eu, 0, 0xff101010u) == 0xffef3340u);
      CHECK(os1_gfx_button_pixel(&g, shape, g.bg_cx,
                                 g.top + g.size / 2, 0xffef3340u,
                                 0xfffebc2eu, 0, 0xff101010u) == 0xfffebc2eu);
    }
  }
  CHECK(os1_gfx_rrect_contains(0, 0, 14, 14, 6) == 0);
  CHECK(os1_gfx_rrect_contains(6, 0, 14, 14, 6) == 1);
  CHECK(os1_gfx_rrect_contains(0, 0, 14, 14, 0) == 1);
  struct os1_gfx_button_geometry invalid = {1, 2, 3, 4};
  os1_gfx_button_geometry(INT_MAX, 8, INT_MAX, 24, 0, 1, &invalid);
  CHECK(invalid.size == 0);
  CHECK(os1_gfx_button_hit(&invalid, INT_MAX, INT_MAX) == OS1_GFX_BUTTON_NONE);
  CHECK(os1_gfx_button_pixel(&invalid, 0, INT_MAX, INT_MIN, 0, 0, 1,
                             0xff101010u) == 0xff101010u);
  CHECK(os1_gfx_titlebar_tint(0, 0, 24, 0xff304050u) == 0xff304050u);
  puts("GFX chrome: geometry, both buttons, all shapes/sides, hit and tint PASS");
  return 0;
}
