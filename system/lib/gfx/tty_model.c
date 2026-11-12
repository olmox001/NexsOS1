/*
 * Portable ring-3 VT/ANSI model migrated from kernel/graphics/term.c.
 * No kernel headers, syscalls, font rasterization, dynamic allocation,
 * recursion or mutable globals. Reflow reconstructs cells from text history.
 * The renderer and this model share a
 * bounded cell grid; resize, colours, scroll and cursor work the same way
 * irrespective of the compositor's scheduling.
 */
#include <os1_tty.h>
#include <string.h>

static const uint32_t ansi16[16] = {
  0xff000000U, 0xffbb0000U, 0xff00bb00U, 0xffbbbb00U,
  0xff0000bbU, 0xffbb00bbU, 0xff00bbbbU, 0xffbbbbbbU,
  0xff555555U, 0xffff5555U, 0xff55ff55U, 0xffffff55U,
  0xff5555ffU, 0xffff55ffU, 0xff55ffffU, 0xffffffffU
};

static uint32_t tty_color(int n) {
  if (n < 0) n = 0;
  if (n < 16) return ansi16[n];
  if (n < 232) {
    int c = n - 16, r = c / 36, g = (c / 6) % 6, b = c % 6;
    uint32_t red = (uint32_t)(r ? 55 + r * 40 : 0);
    uint32_t green = (uint32_t)(g ? 55 + g * 40 : 0);
    uint32_t blue = (uint32_t)(b ? 55 + b * 40 : 0);
    return 0xff000000U | (red << 16) | (green << 8) | blue;
  }
  if (n > 255) n = 255;
  uint32_t v = (uint32_t)(8 + (n - 232) * 10);
  return 0xff000000U | (v << 16) | (v << 8) | v;
}

static int tty_clamp(int n, int max) {
  if (n < 0) return 0;
  if (n >= max) return max - 1;
  return n;
}

static void tty_set_cell(struct os1_tty_model *t, int x, int y, uint8_t ch) {
  struct os1_tty_cell *cell = &t->cells[y][x];
  int fg_default = t->reverse ? t->bg_uses_default : t->fg_uses_default;
  int bg_default = t->reverse ? t->fg_uses_default : t->bg_uses_default;
  cell->ch = ch;
  cell->fg = t->reverse ? t->bg : t->fg;
  cell->bg = t->reverse ? t->fg : t->bg;
  cell->defaults = (uint8_t)((fg_default ? OS1_TTY_CELL_FG_DEFAULT : 0U) |
                             (bg_default ? OS1_TTY_CELL_BG_DEFAULT : 0U) |
                             (t->reverse ? OS1_TTY_CELL_REVERSED : 0U));
  t->dirty[y] = 1;
}

static void tty_blank(struct os1_tty_model *t, int x, int y) {
  tty_set_cell(t, x, y, ' ');
}

static void tty_all_dirty(struct os1_tty_model *t) {
  for (int y = 0; y < OS1_TTY_ROWS_MAX; ++y) t->dirty[y] = 1;
}

int os1_tty_grid_from_pixels(int width, int height,
                              int cell_width, int cell_height,
                              int *cols, int *rows) {
  if (!cols || !rows || width <= 0 || height <= 0 ||
      width > OS1_TTY_SURFACE_W_MAX ||
      height > OS1_TTY_SURFACE_H_MAX ||
      cell_width <= 0 || cell_height <= 0) return -1;
  int c = width / cell_width;
  int r = height / cell_height;
  if (c < 1 || r < 1 || c > OS1_TTY_COLS_MAX ||
      r > OS1_TTY_ROWS_MAX) return -1;
  *cols = c;
  *rows = r;
  return 0;
}

int os1_tty_init(struct os1_tty_model *t, int cols, int rows,
                 uint32_t fg, uint32_t bg) {
  if (!t || cols < 1 || cols > OS1_TTY_COLS_MAX ||
      rows < 1 || rows > OS1_TTY_ROWS_MAX) return -1;
  if (memset(t, 0, sizeof(*t)) != t) return -1;
  t->cols = cols;
  t->rows = rows;
  t->default_fg = fg;
  t->default_bg = bg;
  t->fg = fg;
  t->bg = bg;
  t->fg_uses_default = 1;
  t->bg_uses_default = 1;
  t->cursor_visible = 1;
  /* Initialize the complete retained text store once. Subsequent viewport
   * resizes never reallocate or erase hidden rows/columns. */
  for (int y = 0; y < OS1_TTY_ROWS_MAX; ++y)
    for (int x = 0; x < OS1_TTY_COLS_MAX; ++x)
      tty_blank(t, x, y);
  return 0;
}

/* Reflow in-place using the independent, immutable-while-replaying byte
 * history. No allocation or extra copy of the 64-KiB journal is required.
 * The recorded stream contains the actual VT control sequences, which means
 * a hard newline remains hard and auto-wrap adapts to the new column count.
 * No kernel or compositor logic belongs in this userland terminal model. */
int os1_tty_resize(struct os1_tty_model *t, int cols, int rows) {
  if (!t || cols < 1 || cols > OS1_TTY_COLS_MAX ||
      rows < 1 || rows > OS1_TTY_ROWS_MAX) return -1;
  if (t->cols == cols && t->rows == rows) return 0;
  t->cols = cols;
  t->rows = rows;
  t->x = 0;
  t->y = 0;
  t->saved_x = 0;
  t->saved_y = 0;
  t->escape_mode = 0;
  t->escape_len = 0;
  t->cursor_visible = 1;
  t->bold = 0;
  t->reverse = 0;
  t->fg = t->default_fg;
  t->bg = t->default_bg;
  t->fg_uses_default = 1;
  t->bg_uses_default = 1;
  for (int row = 0; row < OS1_TTY_ROWS_MAX; ++row) {
    for (int col = 0; col < OS1_TTY_COLS_MAX; ++col)
      tty_blank(t, col, row);
  }
  t->replaying = 1;
  int status = 0;
  for (size_t block = 0U;
       block < OS1_TTY_HISTORY_BYTES / OS1_TTY_FEED_MAX &&
       block * OS1_TTY_FEED_MAX < t->history_len && status == 0; ++block) {
    size_t offset = block * OS1_TTY_FEED_MAX;
    size_t take = t->history_len - offset;
    if (take > OS1_TTY_FEED_MAX) take = OS1_TTY_FEED_MAX;
    if (os1_tty_feed(t, t->history + offset, take) != 0) {
      status = -1;
    }
  }
  t->replaying = 0;
  tty_all_dirty(t);
  return status;
}

/* Change only theme-derived colours. Explicit ANSI colours retain their
 * identities; reverse-video cells track which underlying channel used the
 * default palette. A full row redraw is required even if no new text arrives. */
int os1_tty_set_palette(struct os1_tty_model *t,
                        uint32_t foreground, uint32_t background) {
  if (!t || t->cols < 1 || t->cols > OS1_TTY_COLS_MAX ||
      t->rows < 1 || t->rows > OS1_TTY_ROWS_MAX) return -1;
  if (t->default_fg == foreground && t->default_bg == background) return 0;
  t->default_fg = foreground;
  t->default_bg = background;
  if (t->fg_uses_default) t->fg = foreground;
  if (t->bg_uses_default) t->bg = background;
  for (int y = 0; y < OS1_TTY_ROWS_MAX; ++y) {
    for (int x = 0; x < OS1_TTY_COLS_MAX; ++x) {
      struct os1_tty_cell *cell = &t->cells[y][x];
      int reversed = (cell->defaults & OS1_TTY_CELL_REVERSED) != 0U;
      if ((cell->defaults & OS1_TTY_CELL_FG_DEFAULT) != 0U)
        cell->fg = reversed ? background : foreground;
      if ((cell->defaults & OS1_TTY_CELL_BG_DEFAULT) != 0U)
        cell->bg = reversed ? foreground : background;
    }
  }
  tty_all_dirty(t);
  return 0;
}

static void tty_scroll(struct os1_tty_model *t) {
  if (t->y < t->rows) return;
  for (int row = 1; row < OS1_TTY_ROWS_MAX && row < t->rows; ++row) {
    /* Retain the full logical row (including currently hidden columns) so
     * a later window expansion never resurrects mismatched fragments. */
    if (memcpy(t->cells[row-1], t->cells[row],
               sizeof(t->cells[0])) != t->cells[row-1])
      return;
  }
  for (int x = 0; x < OS1_TTY_COLS_MAX; ++x)
    tty_blank(t, x, t->rows-1);
  t->y = t->rows - 1;
  tty_all_dirty(t);
}

static int tty_parse_params(const struct os1_tty_model *t, int v[16]) {
  int index = 0, current = 0, have = 0;
  int full = 0;
  for (int i = 0; i < OS1_TTY_CSI_MAX && i < t->escape_len && !full; ++i) {
    char c = t->escape[i];
    if (c >= '0' && c <= '9') {
      if (current < 100000) current = current * 10 + (c - '0');
      have = 1;
    } else if (c == ';') {
      if (index >= 15) full = 1;
      else {
        v[index++] = current;
        current = 0;
        have = 1;
      }
    }
  }
  if (have && index < 16) v[index++] = current;
  return index;
}

static void tty_reset_sgr(struct os1_tty_model *t) {
  t->fg = t->default_fg;
  t->bg = t->default_bg;
  t->fg_uses_default = 1;
  t->bg_uses_default = 1;
  t->bold = 0;
  t->reverse = 0;
}

static void tty_sgr(struct os1_tty_model *t) {
  int values[16] = {0};
  int n = tty_parse_params(t, values);
  if (n == 0) { tty_reset_sgr(t); return; }
  int skip = 0;
  for (int i = 0; i < 16 && i < n; ++i) {
    if (skip > 0) { --skip; continue; }
    int v = values[i];
    if (v == 0) tty_reset_sgr(t);
    else if (v == 1) t->bold = 1;
    else if (v == 22) t->bold = 0;
    else if (v == 7) t->reverse = 1;
    else if (v == 27) t->reverse = 0;
    else if (v == 39) { t->fg = t->default_fg; t->fg_uses_default = 1; }
    else if (v == 49) { t->bg = t->default_bg; t->bg_uses_default = 1; }
    else if (v >= 30 && v <= 37) {
      t->fg = tty_color(v - 30 + (t->bold ? 8 : 0));
      t->fg_uses_default = 0;
    } else if (v >= 90 && v <= 97) {
      t->fg = tty_color(v - 90 + 8);
      t->fg_uses_default = 0;
    } else if (v >= 40 && v <= 47) {
      t->bg = tty_color(v - 40);
      t->bg_uses_default = 0;
    } else if (v >= 100 && v <= 107) {
      t->bg = tty_color(v - 100 + 8);
      t->bg_uses_default = 0;
    } else if ((v == 38 || v == 48) && i + 2 < n && values[i+1] == 5) {
      uint32_t color = tty_color(values[i+2]);
      if (v == 38) { t->fg = color; t->fg_uses_default = 0; }
      else { t->bg = color; t->bg_uses_default = 0; }
      skip = 2;
    } else if ((v == 38 || v == 48) && i + 4 < n && values[i+1] == 2) {
      uint32_t color = 0xff000000U |
          ((uint32_t)(values[i+2] & 255) << 16) |
          ((uint32_t)(values[i+3] & 255) << 8) |
          (uint32_t)(values[i+4] & 255);
      if (v == 38) { t->fg = color; t->fg_uses_default = 0; }
      else { t->bg = color; t->bg_uses_default = 0; }
      skip = 4;
    }
  }
}

static void tty_csi_navigation(struct os1_tty_model *t, char final) {
  int values[16] = {0};
  if (t->escape_len > 0 && t->escape[0] == '?') {
    if (t->escape_len >= 3 && t->escape[1] == '2' &&
        t->escape[2] == '5' && (final == 'h' || final == 'l'))
      t->cursor_visible = final == 'h';
    return;
  }
  if (final == 'm') { tty_sgr(t); return; }
  int n = tty_parse_params(t, values);
  int a = n > 0 ? values[0] : 0;
  int b = n > 1 ? values[1] : 0;
  if (final == 'H' || final == 'f') {
    t->y = tty_clamp(a > 0 ? a-1 : 0, t->rows);
    t->x = tty_clamp(b > 0 ? b-1 : 0, t->cols);
  } else if (final == 'A' || final == 'B') {
    int step = a > 0 ? a : 1;
    t->y = tty_clamp(t->y + (final == 'A' ? -step : step), t->rows);
  } else if (final == 'C' || final == 'D') {
    int step = a > 0 ? a : 1;
    int pos = t->y * t->cols + t->x;
    int end = t->rows * t->cols - 1;
    if (step > end) step = end;
    pos += final == 'C' ? step : -step;
    if (pos < 0) pos = 0;
    if (pos > end) pos = end;
    t->y = pos / t->cols;
    t->x = pos % t->cols;
  } else if (final == 'G') {
    t->x = tty_clamp(a > 0 ? a - 1 : 0, t->cols);
  } else if (final == 'd') {
    t->y = tty_clamp(a > 0 ? a - 1 : 0, t->rows);
  } else if (final == 's') {
    t->saved_x = t->x;
    t->saved_y = t->y;
  } else if (final == 'u') {
    t->x = t->saved_x;
    t->y = t->saved_y;
  }
}

static void tty_csi(struct os1_tty_model *t, char final) {
  if (final == 'K' || final == 'J') {
    int values[16] = {0};
    int count = tty_parse_params(t, values);
    int mode = count > 0 ? values[0] : 0;
    if (final == 'K') {
      int start = mode == 1 || mode == 2 ? 0 : t->x;
      int finish = mode == 0 ? t->cols - 1 :
                   mode == 1 ? t->x : t->cols - 1;
      for (int x = 0; x < OS1_TTY_COLS_MAX; ++x)
        if (x >= start && x <= finish && x < t->cols)
          tty_blank(t, x, t->y);
    } else {
      for (int y = 0; y < OS1_TTY_ROWS_MAX; ++y)
        for (int x = 0; x < OS1_TTY_COLS_MAX; ++x)
          tty_blank(t, x, y);
      t->x = 0;
      t->y = 0;
    }
    return;
  }
  tty_csi_navigation(t, final);
}

/* Keep recent complete logical lines when a terminal emits more than its
 * bounded history capacity. Remove a full newline-delimited prefix rather
 * than an arbitrary partial UTF/ANSI sequence. No framebuffer is involved. */
static void tty_record_stream(struct os1_tty_model *t,
                              const char *bytes, size_t count) {
  if (t->replaying || count == 0U) return;
  if (t->history_len + count > OS1_TTY_HISTORY_BYTES) {
    size_t discard = t->history_len + count - OS1_TTY_HISTORY_BYTES;
    int found = 0;
    for (size_t i = 0; i < OS1_TTY_HISTORY_BYTES && !found; ++i) {
      if (i >= discard && i < t->history_len && t->history[i] == '\n') {
        discard = i + 1U;
        found = 1;
      }
    }
    if (discard > t->history_len) discard = t->history_len;
    size_t retained = t->history_len - discard;
    for (size_t i = 0; i < OS1_TTY_HISTORY_BYTES && i < retained; ++i) {
      t->history[i] = t->history[discard + i];
    }
    t->history_len = retained;
  }
  for (size_t i = 0; i < OS1_TTY_FEED_MAX && i < count; ++i) {
    t->history[t->history_len + i] = bytes[i];
  }
  t->history_len += count;
}

int os1_tty_feed(struct os1_tty_model *t, const char *bytes, size_t count) {
  if (!t || (!bytes && count > 0U) || count > OS1_TTY_FEED_MAX ||
      t->cols < 1 || t->cols > OS1_TTY_COLS_MAX ||
      t->rows < 1 || t->rows > OS1_TTY_ROWS_MAX) return -1;
  tty_record_stream(t, bytes, count);
  int old_y = tty_clamp(t->y, t->rows);
  for (size_t i = 0U; i < OS1_TTY_FEED_MAX && i < count; ++i) {
    unsigned char c = (unsigned char)bytes[i];
    if (t->escape_mode == 1) {
      t->escape_mode = c == '[' ? 2 : 0;
      continue;
    }
    if (t->escape_mode == 2) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        tty_csi(t, (char)c);
        t->escape_mode = 0;
      } else if (t->escape_len < OS1_TTY_CSI_MAX - 1) {
        t->escape[t->escape_len++] = (char)c;
      } else {
        t->escape_mode = 0;
      }
      continue;
    }
    if (c == 27U) {
      t->escape_mode = 1;
      t->escape_len = 0;
    } else if (c == '\r') {
      t->x = 0;
    } else if (c == '\n') {
      t->x = 0;
      if (t->y < t->rows) ++t->y;
      tty_scroll(t);
    } else if (c == '\b' || c == 127U) {
      if (t->x > 0) --t->x;
    } else if (c >= 32U && c < 127U) {
      if (t->x >= t->cols) { t->x = 0; ++t->y; }
      tty_scroll(t);
      tty_set_cell(t, t->x, t->y, c);
      ++t->x;
    }
  }
  t->dirty[old_y] = 1;
  t->dirty[tty_clamp(t->y, t->rows)] = 1;
  return 0;
}
