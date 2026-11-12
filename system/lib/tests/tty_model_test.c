/* Real VT/ANSI state-machine regression tests, no assert macros or mocks. */
#include <os1_tty.h>
#include <os1_material_colors.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int test_plain_and_wrap(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 4, 2, 0xffeeeeeeU, 0xff000000U) != 0) return 1;
  if (os1_tty_feed(&t, "ABCDz", 5U) != 0) return 2;
  if (t.cells[0][3].ch != 'D' || t.cells[1][0].ch != 'z') return 3;
  if (t.x != 1 || t.y != 1) return 4;
  return 0;
}

static int test_control_and_backspace(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 20, 4, 0xffeeeeeeU, 0xff000000U)) return 1;
  if (os1_tty_feed(&t, "ABC\bZ\rQ", 7U)) return 2;
  if (t.cells[0][0].ch != 'Q' || t.cells[0][2].ch != 'Z') return 3;
  if (t.x != 1 || t.y != 0) return 4;
  return 0;
}

static int test_scroll_and_clear(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 4, 2, 0xffffffffU, 0xff000000U)) return 1;
  if (os1_tty_feed(&t, "1\n2\n3", 5U)) return 2;
  if (t.cells[0][0].ch != '2' || t.cells[1][0].ch != '3') return 3;
  if (os1_tty_feed(&t, "\033[2J", 4U)) return 4;
  if (t.cells[0][0].ch != ' ' || t.cells[1][0].ch != ' ') return 5;
  if (t.x != 0 || t.y != 0) return 6;
  return 0;
}

static int test_sgr_and_256(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 20, 4, 0xffeeeeeeU, 0xff000000U)) return 1;
  const char *text = "\033[31mR\033[38;5;196mB\033[0mX";
  if (os1_tty_feed(&t, text, strlen(text))) return 2;
  if (t.cells[0][0].fg != 0xffbb0000U) return 3;
  if (t.cells[0][1].fg != 0xffff0000U) return 4;
  if (t.cells[0][2].fg != 0xffeeeeeeU) return 5;
  return 0;
}

static int test_truecolor_and_background(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 20, 4, 0xffeeeeeeU, 0xff000000U)) return 1;
  const char *text = "\033[38;2;1;2;3mX\033[48;2;4;5;6mY";
  if (os1_tty_feed(&t, text, strlen(text))) return 2;
  if (t.cells[0][0].fg != 0xff010203U) return 3;
  if (t.cells[0][1].bg != 0xff040506U) return 4;
  return 0;
}

static int test_split_escape_and_erase(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 20, 4, 0xffeeeeeeU, 0xff000000U)) return 1;
  if (os1_tty_feed(&t, "ABCDE\033[", 7U)) return 2;
  if (os1_tty_feed(&t, "3D\033[K", 5U)) return 3;
  if (t.x != 2 || t.cells[0][2].ch != ' ') return 4;
  if (t.cells[0][0].ch != 'A' || t.cells[0][1].ch != 'B') return 5;
  return 0;
}

static int test_cursor_save_and_position(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 20, 4, 0xffeeeeeeU, 0xff000000U)) return 1;
  const char *s = "\033[2;5H\033[s\033[1;1H!\033[uX";
  if (os1_tty_feed(&t, s, strlen(s))) return 2;
  if (t.cells[0][0].ch != '!' || t.cells[1][4].ch != 'X') return 3;
  if (t.x != 5 || t.y != 1) return 4;
  return 0;
}

static int test_resize_and_bounds(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 4, 3, 0xffeeeeeeU, 0xff000000U)) return 1;
  if (os1_tty_feed(&t, "ABCD", 4U)) return 2;
  if (os1_tty_resize(&t, 6, 3)) return 3;
  if (t.cells[0][0].ch != 'A' || t.cells[0][4].ch != ' ') return 4;
  if (os1_tty_resize(&t, OS1_TTY_COLS_MAX+1, 2) == 0) return 5;
  if (os1_tty_feed(&t, "X", OS1_TTY_FEED_MAX+1U) == 0) return 6;
  return 0;
}

static int test_cursor_visible_and_bad_args(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 8, 2, 0xffeeeeeeU, 0xff000000U)) return 1;
  if (os1_tty_feed(&t, "\033[?25l", 6U)) return 2;
  if (t.cursor_visible) return 3;
  if (os1_tty_feed(&t, "\033[?25h", 6U)) return 4;
  if (!t.cursor_visible) return 5;
  if (os1_tty_feed(&t, NULL, 1U) == 0) return 6;
  return 0;
}

/* Kilo's full-screen editor uses SGR 7/27 to invert its status bar.
 * Its streamed VT escape sequences can span independent PORT messages. */
static int test_kilo_reverse_and_reset(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 8, 2, 0xffededf3U, 0xff1a1a22U)) return 1;
  if (os1_tty_feed(&t, "\033[", 2U)) return 2;
  if (os1_tty_feed(&t, "7mR\033[27mN", 9U)) return 3;
  if (t.cells[0][0].ch != 'R' || t.cells[0][0].fg != 0xff1a1a22U ||
      t.cells[0][0].bg != 0xffededf3U) return 4;
  if (t.cells[0][1].ch != 'N' || t.cells[0][1].fg != 0xffededf3U ||
      t.cells[0][1].bg != 0xff1a1a22U) return 5;
  if (t.cells[0][0].defaults !=
      (OS1_TTY_CELL_FG_DEFAULT|OS1_TTY_CELL_BG_DEFAULT|
       OS1_TTY_CELL_REVERSED)) return 6;
  if (os1_tty_feed(&t, "\033[0mX", 5U)) return 7;
  if (t.reverse || t.cells[0][2].fg != t.default_fg) return 8;
  return 0;
}

/* A live dark->light change updates existing default-text cells and blank
 * backgrounds while retaining user/app SGR 31;44 foreground/background. */
static int test_theme_preserves_explicit_ansi(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 8, 2, 0xffededf3U, 0xff1a1a22U)) return 1;
  const char *stream="\033[31;44mC\033[0mD";
  if (os1_tty_feed(&t, stream, strlen(stream))) return 2;
  uint32_t explicit_fg=t.cells[0][0].fg, explicit_bg=t.cells[0][0].bg;
  if (t.cells[0][0].defaults != 0) return 3;
  if (os1_tty_set_palette(&t, 0xff1c1c1eU, 0xfff5f5f7U)) return 4;
  if (t.cells[0][0].fg != explicit_fg ||
      t.cells[0][0].bg != explicit_bg) return 5;
  if (t.cells[0][1].fg != 0xff1c1c1eU ||
      t.cells[0][1].bg != 0xfff5f5f7U) return 6;
  if (t.fg != t.default_fg || t.bg != t.default_bg) return 7;
  if (!t.dirty[0]) return 8;
  return 0;
}

/* Reverse video swaps the source of default foreground/background. Updating
 * the theme recolours those sources correctly, not merely the raw RGBA. */
static int test_theme_recolors_reversed_default(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 4, 2, 0xffeeeeeeU, 0xff111111U)) return 1;
  if (os1_tty_feed(&t, "\033[7mR", 5U)) return 2;
  if (os1_tty_set_palette(&t, 0xff202020U, 0xfff0f0f0U)) return 3;
  if (t.cells[0][0].fg != 0xfff0f0f0U ||
      t.cells[0][0].bg != 0xff202020U) return 4;
  if (os1_tty_feed(&t, "X\033[27mY", 7U)) return 5;
  if (t.cells[0][1].fg != 0xfff0f0f0U ||
      t.cells[0][1].bg != 0xff202020U) return 6;
  if (t.cells[0][2].fg != 0xff202020U ||
      t.cells[0][2].bg != 0xfff0f0f0U) return 7;
  return 0;
}

/* Truecolor cells never alias the terminal's theme defaults, even when
 * their current numeric values happen to match the dark theme. */
static int test_truecolor_theme_provenance(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 8, 2, 0xffededf3U, 0xff1a1a22U)) return 1;
  const char *stream="\033[38;2;237;237;243;48;2;26;26;34mT\033[mD";
  if (os1_tty_feed(&t, stream, strlen(stream))) return 2;
  if (t.cells[0][0].defaults != 0) return 3;
  if (os1_tty_set_palette(&t, 0xff1c1c1eU, 0xfff5f5f7U)) return 4;
  if (t.cells[0][0].fg != 0xffededf3U ||
      t.cells[0][0].bg != 0xff1a1a22U) return 5;
  if (t.cells[0][1].fg != 0xff1c1c1eU ||
      t.cells[0][1].bg != 0xfff5f5f7U) return 6;
  if (os1_tty_set_palette(NULL, 0U, 0U) == 0) return 7;
  return 0;
}


/* Pixel size and font metrics produce exact cell counts, not an upper-bound
 * clamp; two independent sessions may have different fonts/resolutions. */
static int test_grid_geometry_bounds(void) {
  int cols = 0, rows = 0;
  if (os1_tty_grid_from_pixels(640, 432, 8, 16, &cols, &rows)) return 1;
  if (cols != 80 || rows != 27) return 2;
  if (os1_tty_grid_from_pixels(1024, 768, 5, 8, &cols, &rows)) return 3;
  if (cols != 204 || rows != 96) return 4;
  if (os1_tty_grid_from_pixels(1024, 768, 1, 1, &cols, &rows) == 0) return 5;
  if (os1_tty_grid_from_pixels(0, 432, 8, 16, &cols, &rows) == 0) return 6;
  if (os1_tty_grid_from_pixels(640, 432, 0, 16, &cols, &rows) == 0) return 7;
  if (os1_tty_grid_from_pixels(640, 432, 8, 16, NULL, &rows) == 0) return 8;
  return 0;
}

static int test_shrink_expand_retains_text(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 6, 4, 0xffeeeeeeU, 0xff222222U)) return 1;
  if (os1_tty_feed(&t, "ABCDEF\r\nGHIJKL", 14U)) return 2;
  if (os1_tty_resize(&t, 3, 1)) return 3;
  if (t.cols != 3 || t.rows != 1 || t.x > t.cols || t.y >= t.rows) return 4;
  /* Reflow, not hidden original columns: newest line occupies the viewport. */
  if (t.cells[0][0].ch != 'J' || t.cells[0][2].ch != 'L') return 5;
  if (os1_tty_resize(&t, 6, 4)) return 6;
  if (t.cells[0][5].ch != 'F' || t.cells[1][5].ch != 'L') return 7;
  if (!t.dirty[0] || !t.dirty[3]) return 8;
  return 0;
}

static int test_scrolled_text_retains_hidden_columns(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 6, 2, 0xffeeeeeeU, 0xff222222U)) return 1;
  if (os1_tty_feed(&t, "ABCDEF\r\n123456", 14U)) return 2;
  if (os1_tty_resize(&t, 3, 2)) return 3;
  if (t.cells[0][0].ch != '1' || t.cells[1][0].ch != '4') return 4;
  if (os1_tty_feed(&t, "\r\n", 2U)) return 5;
  if (os1_tty_resize(&t, 6, 2)) return 6;
  if (t.cells[0][0].ch != '1' || t.cells[0][5].ch != '6' ||
      t.cells[1][0].ch != ' ') return 7;
  return 0;
}

static int test_multiple_sessions_isolation(void) {
  struct os1_tty_model one, two;
  if (os1_tty_init(&one, 8, 3, 0xffeeeeeeU, 0xff111111U)) return 1;
  if (os1_tty_init(&two, 5, 2, 0xff333333U, 0xffddddddU)) return 2;
  if (os1_tty_feed(&one, "\033[31mONE", 8U)) return 3;
  if (os1_tty_feed(&two, "TWO", 3U)) return 4;
  if (os1_tty_resize(&one, 3, 2)) return 5;
  if (os1_tty_set_palette(&two, 0xff222222U, 0xffeeeeeeU)) return 6;
  if (one.cols != 3 || two.cols != 5) return 7;
  if (one.cells[0][2].ch != 'E' || two.cells[0][2].ch != 'O') return 8;
  if (one.default_fg != 0xffeeeeeeU ||
      two.default_fg != 0xff222222U) return 9;
  return 0;
}

static int test_newline_bottom_scroll_bounds(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 4, 2, 0xffeeeeeeU, 0xff000000U)) return 1;
  const char *sequence = "A\nB\n\n\n\033[K";
  if (os1_tty_feed(&t, sequence, strlen(sequence))) return 2;
  if (t.y < 0 || t.y >= t.rows || t.x < 0 || t.x > t.cols) return 3;
  if (t.cells[t.y][0].ch != ' ') return 4;
  return 0;
}


/* Both theme modes keep window chrome, TTY field, text and caret separate.
 * Material white is always warm/off-white, and gray is lighter than before. */
static int test_material_surface_palette_roles(void) {
  struct os1_material_tty_palette light = os1_material_tty_palette(1);
  struct os1_material_tty_palette dark = os1_material_tty_palette(0);
  if (light.window_surface == light.text_background ||
      dark.window_surface == dark.text_background) return 1;
  if (light.text_background == light.text_foreground ||
      dark.text_background == dark.text_foreground) return 2;
  if (light.caret == dark.caret || light.prompt == dark.prompt) return 3;
  if (light.text_background == 0xffffffffU ||
      OS1_MATERIAL_BG_WHITE == 0xffffffffU ||
      OS1_MATERIAL_BG_BRIGHT_WHITE == 0xffffffffU) return 4;
  if ((OS1_MATERIAL_BG_GRAY & 0xffU) <= 0x80U) return 5;
  if (light.text_background == dark.text_background ||
      light.text_foreground == dark.text_foreground) return 6;
  return 0;
}

static int test_theme_recolours_hidden_cells(void) {
  struct os1_tty_model t;
  struct os1_material_tty_palette light = os1_material_tty_palette(1);
  struct os1_material_tty_palette dark = os1_material_tty_palette(0);
  if (os1_tty_init(&t, 9, 4, light.text_foreground,
                   light.text_background)) return 1;
  if (os1_tty_feed(&t, "ABCDEFGH", 8U)) return 2;
  if (os1_tty_resize(&t, 3, 2)) return 3;
  if (os1_tty_set_palette(&t, dark.text_foreground,
                          dark.text_background)) return 4;
  if (os1_tty_resize(&t, 9, 4)) return 5;
  if (t.cells[0][7].ch != 'H' ||
      t.cells[0][7].fg != dark.text_foreground ||
      t.cells[0][7].bg != dark.text_background) return 6;
  return 0;
}

/* Stress both viewport counts and stored content without reallocating the
 * text model, bypassing cell bounds or coupling it to a pixel allocation. */
static int test_repeated_resize_retains_hidden_columns(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 12, 5, 0xffeeeeeeU, 0xff222222U)) return 1;
  if (os1_tty_feed(&t, "abcdefghijkl", 12U)) return 2;
  for (int iteration = 0; iteration < 1000; ++iteration) {
    int cols = 1 + (iteration % 12);
    int rows = 1 + (iteration % 5);
    if (os1_tty_resize(&t, cols, rows)) return 3;
    if (t.cols != cols || t.rows != rows) return 4;
    if (t.y < 0 || t.y >= rows || t.x < 0 || t.x > cols) return 5;
  }
  if (os1_tty_resize(&t, 12, 5)) return 6;
  if (t.cells[0][0].ch != 'a' || t.cells[0][11].ch != 'l') return 7;
  return 0;
}

/* Hard newlines and auto-wrapped visual rows must be distinguished. The
 * original text and caret return to their logical position after expansion. */
static int test_reflow_softwrap_and_hard_newlines(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 8, 4, 0xffffffffU, 0xff171717U)) return 1;
  const char *stream = "ABCDEFGH\r\n123456";
  if (os1_tty_feed(&t, stream, strlen(stream))) return 2;
  if (os1_tty_resize(&t, 4, 4)) return 3;
  if (t.cells[0][0].ch != 'A' || t.cells[0][3].ch != 'D' ||
      t.cells[1][0].ch != 'E' || t.cells[1][3].ch != 'H' ||
      t.cells[2][0].ch != '1' || t.cells[3][0].ch != '5') return 4;
  if (os1_tty_resize(&t, 8, 4)) return 5;
  if (t.cells[0][7].ch != 'H' || t.cells[1][5].ch != '6' ||
      t.y != 1 || t.x != 6) return 6;
  return 0;
}

/* Terminal colours are stored with the characters, not raster pixels.
 * Both explicit ANSI styles and default Material colours survive reflow. */
static int test_reflow_preserves_ansi_provenance(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 8, 4, 0xffddddddU, 0xff222222U)) return 1;
  const char *stream = "\033[31mABCDEFGH\033[0m!\r\nEND";
  if (os1_tty_feed(&t, stream, strlen(stream))) return 2;
  if (os1_tty_resize(&t, 4, 4)) return 3;
  if (t.cells[0][0].fg != 0xffbb0000U ||
      t.cells[1][3].fg != 0xffbb0000U ||
      t.cells[2][0].ch != '!') return 4;
  if (os1_tty_set_palette(&t, 0xff111111U, 0xffeeeeeeU)) return 5;
  if (os1_tty_resize(&t, 8, 4)) return 6;
  if (t.cells[0][0].fg != 0xffbb0000U ||
      t.cells[1][0].ch != '!' ||
      t.cells[1][0].fg != 0xff111111U ||
      t.cells[2][0].ch != 'E') return 7;
  return 0;
}

/* Long-running sessions retire old scrollback at a whole-line boundary.
 * Rapid resize after rollover must never exceed independent text bounds. */
static int test_reflow_history_bounded_rollover(void) {
  struct os1_tty_model t;
  if (os1_tty_init(&t, 10, 3, 0xffffffffU, 0xff000000U)) return 1;
  const char *line = "abcdefghi\r\n";
  for (int i = 0; i < 8000; ++i)
    if (os1_tty_feed(&t, line, strlen(line))) return 2;
  if (t.history_len > OS1_TTY_HISTORY_BYTES ||
      t.history_len < OS1_TTY_HISTORY_BYTES / 2U) return 3;
  if (os1_tty_resize(&t, 4, 2)) return 4;
  if (t.y < 0 || t.y >= 2 || t.x < 0 || t.x > 4) return 5;
  if (os1_tty_resize(&t, 10, 3)) return 6;
  if (t.cells[1][0].ch != 'a' || t.cells[1][8].ch != 'i') return 7;
  return 0;
}

int main(void) {
  int cases[24] = {
    test_plain_and_wrap(), test_control_and_backspace(),
    test_scroll_and_clear(), test_sgr_and_256(),
    test_truecolor_and_background(), test_split_escape_and_erase(),
    test_cursor_save_and_position(), test_resize_and_bounds(),
    test_cursor_visible_and_bad_args(),
    test_kilo_reverse_and_reset(), test_theme_preserves_explicit_ansi(),
    test_theme_recolors_reversed_default(),
    test_truecolor_theme_provenance(),
    test_grid_geometry_bounds(), test_shrink_expand_retains_text(),
    test_scrolled_text_retains_hidden_columns(),
    test_multiple_sessions_isolation(), test_newline_bottom_scroll_bounds(),
    test_material_surface_palette_roles(), test_theme_recolours_hidden_cells(),
    test_repeated_resize_retains_hidden_columns(),
    test_reflow_softwrap_and_hard_newlines(),
    test_reflow_preserves_ansi_provenance(),
    test_reflow_history_bounded_rollover()
  };
  int failures = 0;
  for (int i = 0; i < 24; ++i) {
    if (cases[i] == 0) continue;
    int written = printf("TTY MODEL CASE %d FAIL %d\n", i, cases[i]);
    if (written <= 0) return 100;
    ++failures;
  }
  return failures;
}
