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

/* Exercise the REAL userland compositor raster against independent source
 * and presentation sizes. The graphics client does not redraw in this case. */
static int test_source_scales_without_cropping(void) {
  uint32_t pixels[36], source_pixels[4] = {
      0xff102030U, 0xff304050U, 0xff607080U, 0xff90a0b0U
  };
  struct os1_gfx_surface frame = {pixels, 6U, 6U, 6U, 36U};
  struct os1_gfx_surface source = {source_pixels, 2U, 2U, 2U, 4U};
  struct os1_gfx_rect box = {1, 1, 4, 4};
  os1_gfx_clear(&frame, 0xff080808U);
  os1_gfx_composite_rounded(&frame, &source, &box, 0, 0);
  if (pixels[1U * 6U + 1U] != source_pixels[0] ||
      pixels[2U * 6U + 2U] != source_pixels[0] ||
      pixels[1U * 6U + 4U] != source_pixels[1] ||
      pixels[4U * 6U + 1U] != source_pixels[2] ||
      pixels[4U * 6U + 4U] != source_pixels[3]) return 1;
  if (pixels[0] != 0xff080808U || pixels[35] != 0xff080808U)
    return 2;
  return 0;
}

/* Rounded corners and client pixels are separate: scaling the client source
 * must not create a thick border or draw outside the destination viewport. */
static int test_scaled_rounded_frame_bounds(void) {
  uint32_t pixels[64], source_pixels[4] = {
      0xff111111U, 0xff333333U, 0xff555555U, 0xff777777U
  };
  struct os1_gfx_surface frame = {pixels, 8U, 8U, 8U, 64U};
  struct os1_gfx_surface source = {source_pixels, 2U, 2U, 2U, 4U};
  struct os1_gfx_rect box = {1, 1, 6, 6};
  os1_gfx_clear(&frame, 0xffaabbccU);
  os1_gfx_composite_rounded(&frame, &source, &box, 0, 2);
  if (pixels[0] != 0xffaabbccU || pixels[63] != 0xffaabbccU)
    return 1;
  if (pixels[3U * 8U + 3U] != source_pixels[0] ||
      pixels[5U * 8U + 5U] != source_pixels[3])
    return 2;
  box.width = 3;
  box.height = 3;
  os1_gfx_clear(&frame, 0xffaabbccU);
  os1_gfx_composite_rounded(&frame, &source, &box, 0, 1);
  if (pixels[3U * 8U + 3U] == 0xffaabbccU ||
      pixels[6U * 8U + 6U] != 0xffaabbccU)
    return 3;
  return 0;
}

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
                                 0xfffebc2eu, 0, 0xff101010u) == 0xfff4f5f7u);
      CHECK(os1_gfx_button_pixel(&g, shape, g.bg_cx,
                                 g.top + g.size / 2, 0xffef3340u,
                                 0xfffebc2eu, 0, 0xff101010u) == 0xfff4f5f7u);
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
  CHECK(!os1_gfx_rrect_contains(-1, 0, 14, 14, 0));
  CHECK(!os1_gfx_rrect_contains(0, 0, 0, 0, 0));
  unsigned partial = 0;
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      unsigned c = os1_gfx_rrect_coverage(x, y, 32, 32, 10);
      CHECK(c <= 255);
      CHECK(c == os1_gfx_rrect_coverage(31 - x, y, 32, 32, 10));
      CHECK(c == os1_gfx_rrect_coverage(x, 31 - y, 32, 32, 10));
      partial += (unsigned)(c > 0 && c < 255);
    }
  }
  CHECK(partial > 0);
  uint32_t pixels[66], src[64];
  for (int i = 0; i < 66; ++i) pixels[i] = 0xff123456U;
  for (int i = 0; i < 64; ++i) src[i] = 0xffffffffU;
  struct os1_gfx_surface dest = {pixels + 1, 8, 8, 8, 64};
  struct os1_gfx_surface source = {src, 8, 8, 8, 64};
  struct os1_gfx_rect content = {0, 0, 8, 8};
  os1_gfx_composite_rounded(&dest, &source, &content, 0, 4);
  CHECK(pixels[1] == 0xff123456U && pixels[28] == 0xffffffffU);
  os1_compositor_style_t style = {.titlebar = 1, .titlebar_height = 20,
      .rounded_corners = 1, .border_radius = 10, .shadows = 1,
      .shadow_size = 6, .window_borders = 1, .button_shape = 2};
  os1_compositor_theme_t theme = {.title_active = 0xff303038U,
      .border = 0xff606070U, .close_btn = 0xffff6058U};
  content.x = -4;
  os1_gfx_window_decoration(&dest, &content, &style, &theme, 1, 0);
  content.x = INT_MAX; content.y = INT_MIN;
  os1_gfx_window_decoration(&dest, &content, &style, &theme, 1, 0);
  os1_gfx_composite_rounded(&dest, &source, &content, 20, 10);
  CHECK(pixels[0] == 0xff123456U && pixels[65] == 0xff123456U);
  /* A clipped view may end in the middle of the last physical scanline. */
  dest.pixels = pixels + 3; dest.width = 4; dest.capacity = 60;
  CHECK(os1_gfx_surface_valid(&dest));
  dest.capacity = 59;
  CHECK(!os1_gfx_surface_valid(&dest));
  int scale = test_source_scales_without_cropping();
  if (scale != 0) return 100 + scale;
  int rounded = test_scaled_rounded_frame_bounds();
  if (rounded != 0) return 200 + rounded;
  if (puts("GFX chrome: geometry, scaled source, clipped rounded resize PASS") < 0) return 300;
  return 0;
}
