/*
 * kernel/graphics/term.c
 * VT/ANSI terminal emulator (extracted from compositor.c — GFX-DYN-01, #123).
 *
 * Operates on a struct terminal (cell model) + a struct gl_surface (pixel
 * target).  No knowledge of windows, PIDs or the compositor: the compositor
 * owns the terminal and supplies the surface.  Supports the ECMA-48 subset a
 * full-screen TTY app (kilo) needs: cursor positioning (H/f), erase in line
 * (K), erase screen (J), SGR colours (m) and DECTCEM caret show/hide (?25 h/l).
 */
#include <kernel/term.h>

#include <graphics/gl.h>
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>

/* Fill a cell-sized rectangle directly into the surface, clipped to bounds.
 * Replaces the compositor's draw_rect_internal for terminal-local drawing:
 * the terminal owns its surface, so no ownership check or window lookup. */
static void term_fill_rect(struct gl_surface *surf, int x, int y, int w, int h,
                           uint32_t color) {
  for (int dy = 0; dy < h; dy++) {
    int py = y + dy;
    if (py < 0 || py >= surf->height)
      continue;
    for (int dx = 0; dx < w; dx++) {
      int px = x + dx;
      if (px < 0 || px >= surf->width)
        continue;
      surf->buffer[py * surf->stride + px] = color;
    }
  }
}

/* xterm 256-colour palette → ARGB (16 base + 6x6x6 cube + 24 grays). */
static uint32_t term_xterm256(int n) {
  static const uint32_t base16[16] = {
      0xFF000000, 0xFFBB0000, 0xFF00BB00, 0xFFBBBB00, 0xFF0000BB, 0xFFBB00BB,
      0xFF00BBBB, 0xFFBBBBBB, 0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
      0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF};
  if (n < 0)
    n = 0;
  if (n < 16)
    return base16[n];
  if (n < 232) {
    int c = n - 16;
    int r = c / 36, g = (c / 6) % 6, b = c % 6;
    int R = r ? 55 + r * 40 : 0, G = g ? 55 + g * 40 : 0, B = b ? 55 + b * 40 : 0;
    return 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
  }
  if (n > 255)
    n = 255;
  int v = 8 + (n - 232) * 10;
  return 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
}

/* Process an ANSI SGR (Select Graphic Rendition) parameter list. */
static void handle_sgr(struct terminal *t) {
  int vals[16];
  int n = 0, cur = 0, have = 0;
  for (int i = 0; i < t->escape_len && n < 16; i++) {
    char c = t->escape_buf[i];
    if (c >= '0' && c <= '9') {
      cur = cur * 10 + (c - '0');
      have = 1;
    } else if (c == ';') {
      vals[n++] = cur;
      cur = 0;
    }
  }
  if (n < 16 && (have || t->escape_len == 0))
    vals[n++] = cur;
  if (n == 0) { /* ESC[m == reset */
    t->fg_color = t->default_fg;
    t->curr_bg_color = t->bg_color;
    t->bold = 0;
    return;
  }

  for (int i = 0; i < n; i++) {
    int v = vals[i];
    if (v == 0) {
      t->fg_color = t->default_fg;
      t->curr_bg_color = t->bg_color;
      t->bold = 0;
    } else if (v == 1) {
      t->bold = 1;
    } else if (v == 22) {
      t->bold = 0;
    } else if (v == 39) {
      t->fg_color = t->default_fg;
    } else if (v == 49) {
      t->curr_bg_color = t->bg_color;
    } else if (v >= 30 && v <= 37) {
      t->fg_color = term_xterm256(v - 30 + (t->bold ? 8 : 0));
    } else if (v >= 90 && v <= 97) {
      t->fg_color = term_xterm256(v - 90 + 8);
    } else if (v >= 40 && v <= 47) {
      t->curr_bg_color = term_xterm256(v - 40);
    } else if (v >= 100 && v <= 107) {
      t->curr_bg_color = term_xterm256(v - 100 + 8);
    } else if (v == 38 || v == 48) {
      uint32_t *tgt = (v == 38) ? &t->fg_color : &t->curr_bg_color;
      if (i + 2 < n && vals[i + 1] == 5) { /* 256-colour: 38;5;N */
        *tgt = term_xterm256(vals[i + 2]);
        i += 2;
      } else if (i + 4 < n && vals[i + 1] == 2) { /* truecolour: 38;2;r;g;b */
        *tgt = 0xFF000000u | ((uint32_t)(vals[i + 2] & 0xFF) << 16) |
               ((uint32_t)(vals[i + 3] & 0xFF) << 8) | (uint32_t)(vals[i + 4] & 0xFF);
        i += 4;
      }
    }
  }
}

/* Parse up to two decimal CSI parameters separated by ';' from buf[0..len).
 * Returns how many parameters were present (0, 1 or 2); a and b receive their
 * values (0 when omitted). */
static int csi_params(const char *buf, int len, int *a, int *b) {
  int vals[2] = {0, 0};
  int which = 0, have = 0;
  for (int i = 0; i < len; i++) {
    char c = buf[i];
    if (c >= '0' && c <= '9') {
      vals[which] = vals[which] * 10 + (c - '0');
      have = 1;
    } else if (c == ';' && which < 1) {
      which++;
    }
  }
  *a = vals[0];
  *b = vals[1];
  return have ? which + 1 : 0;
}

/* Clear one terminal cell to the current background (pixels + grids). */
static void term_clear_cell(struct terminal *t, struct gl_surface *surf, int cx,
                            int cy, int char_w, int char_h) {
  if (cx < 0 || cy < 0 || cx >= t->cols || cy >= t->rows)
    return;
  term_fill_rect(surf, cx * char_w, cy * char_h, char_w, char_h,
                 t->curr_bg_color);
  if (t->text_grid && t->attr_grid) {
    int idx = cy * t->cols + cx;
    t->text_grid[idx] = ' ';
    t->attr_grid[idx] = t->curr_bg_color;
  }
}

/* Erase a previously painted caret by repainting its cell from the text/attr
 * grid (background + glyph).  Idempotent; clears caret_shown.  This is what
 * prevents stray cursor blocks after the cursor moves (newline/scroll). */
static void term_erase_caret(struct terminal *t, struct gl_surface *surf,
                             int char_w, int char_h) {
  if (!t->caret_shown)
    return;
  int px = t->caret_px, py = t->caret_py;
  t->caret_shown = 0;
  if (px < 0 || py < 0 || px >= t->cols || py >= t->rows)
    return;
  term_fill_rect(surf, px * char_w, py * char_h, char_w, char_h, t->bg_color);
  if (t->text_grid && t->attr_grid) {
    int idx = py * t->cols + px;
    char ch = (char)t->text_grid[idx];
    if (ch >= 32 && ch < 127)
      gl_draw_char(surf, px * char_w, py * char_h, ch, t->attr_grid[idx]);
  }
}

/* Draw the text caret as a translucent block at the cursor cell, blended over
 * the cell so the glyph stays readable.  Only a focused, caret-visible terminal
 * shows one.  The previous caret is erased first so movement leaves no trail. */
static void term_draw_cursor(struct terminal *t, struct gl_surface *surf,
                             int char_w, int char_h) {
  term_erase_caret(t, surf, char_w, char_h);

  if (!t->focused || !t->cursor_visible)
    return;
  int cx = t->cursor_x, cy = t->cursor_y;
  if (cx < 0 || cy < 0 || cx >= t->cols || cy >= t->rows)
    return;

  const uint32_t caret = TERM_CARET_COLOR;
  int px0 = cx * char_w, py0 = cy * char_h;
  for (int y = 0; y < char_h; y++) {
    int sy = py0 + y;
    if (sy < 0 || sy >= surf->height)
      continue;
    for (int x = 0; x < char_w; x++) {
      int sx = px0 + x;
      if (sx < 0 || sx >= surf->width)
        continue;
      uint32_t *p = &surf->buffer[sy * surf->stride + sx];
      *p = gl_blend_pixel(caret, *p);
    }
  }
  t->caret_px = cx;
  t->caret_py = cy;
  t->caret_shown = 1;
}

/* Dispatch a complete CSI sequence (ESC [ ... <final>). */
static void handle_csi(struct terminal *t, struct gl_surface *surf, char final,
                       int char_w, int char_h) {
  const char *eb = t->escape_buf;
  int len = t->escape_len;

  /* Private mode: ESC [ ? Pn h/l — we honour 25 (show/hide caret). */
  if (len > 0 && eb[0] == '?') {
    int a, b;
    csi_params(eb + 1, len - 1, &a, &b);
    if (a == 25)
      t->cursor_visible = (final == 'h');
    return;
  }

  if (final == 'm') {
    handle_sgr(t);
    return;
  }

  int a, b;
  int n = csi_params(eb, len, &a, &b);

  if (final == 'H' || final == 'f') {
    int row = (n >= 1 && a > 0) ? a - 1 : 0;
    int col = (n >= 2 && b > 0) ? b - 1 : 0;
    if (row >= t->rows)
      row = t->rows - 1;
    if (col >= t->cols)
      col = t->cols - 1;
    t->cursor_y = row < 0 ? 0 : row;
    t->cursor_x = col < 0 ? 0 : col;
  } else if (final == 'K') {
    int mode = (n >= 1) ? a : 0;
    int x0 = (mode == 1) ? 0 : t->cursor_x;
    int x1 = (mode == 0) ? t->cols - 1 : t->cursor_x;
    if (mode == 2) {
      x0 = 0;
      x1 = t->cols - 1;
    }
    for (int x = x0; x <= x1; x++)
      term_clear_cell(t, surf, x, t->cursor_y, char_w, char_h);
  } else if (final == 'J') {
    term_fill_rect(surf, 0, 0, surf->width, surf->height, t->curr_bg_color);
    if (t->text_grid && t->attr_grid) {
      memset(t->text_grid, ' ', t->cols * t->rows);
      for (int p = 0; p < t->cols * t->rows; p++)
        t->attr_grid[p] = t->curr_bg_color;
    }
    t->cursor_x = 0;
    t->cursor_y = 0;
  } else if (final == 'A' || final == 'B') {
    /* Cursor up/down by N (default 1) — pure vertical movement, clamped to
     * the grid. Unlike C/D below, up/down never "wraps" horizontally: that
     * would only make sense for something like a wide virtual scrollback,
     * which this terminal doesn't have. */
    int d = (n >= 1 && a > 0) ? a : 1;
    t->cursor_y += (final == 'A') ? -d : d;
    if (t->cursor_y < 0)
      t->cursor_y = 0;
    if (t->cursor_y >= t->rows)
      t->cursor_y = t->rows - 1;
  } else if (final == 'C' || final == 'D') {
    /*
     * Cursor forward/back by N cells, WRAPPING across row boundaries.
     *
     * Plain column clamping used to strand the cursor at column 0 (for 'D')
     * or cols-1 (for 'C') the instant a relative move reached the edge of
     * the current row, even when more of the move remained and an adjacent
     * row held the rest of the same logical line — e.g. a shell command
     * long enough to auto-wrap during typing. nxline.h's line editor
     * (user/sys/bin/nxline.h) repositions the caret with exactly these
     * relative CSI C/D sequences after every insert/delete/history-replace,
     * so any input line that wraps during editing would desync the caret
     * from the real insertion point without this fix — the visible cursor
     * and the byte actually being edited would silently drift apart.
     *
     * This grid has no per-row "this break was an autowrap, not a real
     * newline" flag to consult, so the fix wraps unconditionally across
     * every row boundary, not only autowrap ones. That's a deliberate
     * simplification: it's harmless for every current caller (nxline.h
     * only ever moves the caret within the single line it is actively
     * editing, never across an Enter/newline boundary — Enter always
     * submits and resets), and matches how most real terminals treat
     * relative cursor motion in practice.
     */
    int d = (n >= 1 && a > 0) ? a : 1;
    int pos = t->cursor_y * t->cols + t->cursor_x;
    pos += (final == 'C') ? d : -d;
    int max_pos = t->rows * t->cols - 1;
    if (pos < 0)
      pos = 0;
    if (pos > max_pos)
      pos = max_pos;
    t->cursor_y = pos / t->cols;
    t->cursor_x = pos % t->cols;
  } else if (final == 'G') {
    /* Cursor to column N (1-based). */
    int col = (n >= 1 && a > 0) ? a - 1 : 0;
    t->cursor_x = col < 0 ? 0 : (col >= t->cols ? t->cols - 1 : col);
  } else if (final == 'd') {
    /* Cursor to row N (1-based). */
    int row = (n >= 1 && a > 0) ? a - 1 : 0;
    t->cursor_y = row < 0 ? 0 : (row >= t->rows ? t->rows - 1 : row);
  } else if (final == 's') {
    t->saved_x = t->cursor_x;
    t->saved_y = t->cursor_y;
  } else if (final == 'u') {
    t->cursor_x = t->saved_x;
    t->cursor_y = t->saved_y;
  }
}

int term_init(struct terminal *t, int cols, int rows, uint32_t fg,
              uint32_t bg) {
  if (cols < 1)
    cols = 1;
  if (rows < 1)
    rows = 1;
  t->cols = cols;
  t->rows = rows;
  t->cursor_x = 0;
  t->cursor_y = 0;
  t->cursor_visible = 1;
  t->caret_px = 0;
  t->caret_py = 0;
  t->caret_shown = 0;
  t->focused = 0;
  t->fg_color = fg;
  t->default_fg = fg;
  t->bg_color = bg;
  t->curr_bg_color = bg;
  t->bold = 0;
  t->saved_x = 0;
  t->saved_y = 0;
  t->escape_state = 0;
  t->escape_len = 0;

  size_t cells = (size_t)cols * rows;
  t->text_grid = (uint8_t *)kmalloc(cells);
  t->attr_grid = (uint32_t *)kmalloc(cells * 4);
  if (!t->text_grid || !t->attr_grid) {
    term_free(t);
    return -1;
  }
  memset(t->text_grid, ' ', cells);
  for (size_t i = 0; i < cells; i++)
    t->attr_grid[i] = fg;
  return 0;
}

void term_free(struct terminal *t) {
  if (t->text_grid) {
    kfree(t->text_grid);
    t->text_grid = NULL;
  }
  if (t->attr_grid) {
    kfree(t->attr_grid);
    t->attr_grid = NULL;
  }
}

int term_resize(struct terminal *t, int cols, int rows) {
  if (cols < 1)
    cols = 1;
  if (rows < 1)
    rows = 1;
  uint8_t *ntext = (uint8_t *)kmalloc((size_t)cols * rows);
  uint32_t *nattr = (uint32_t *)kmalloc((size_t)cols * rows * 4);
  if (!ntext || !nattr) {
    if (ntext)
      kfree(ntext);
    if (nattr)
      kfree(nattr);
    return -1;
  }
  /* Fresh grids cleared to defaults; pixel reflow is the caller's concern. */
  memset(ntext, ' ', (size_t)cols * rows);
  for (size_t i = 0; i < (size_t)cols * rows; i++)
    nattr[i] = t->default_fg;
  term_free(t);
  t->text_grid = ntext;
  t->attr_grid = nattr;
  t->cols = cols;
  t->rows = rows;
  if (t->cursor_x >= cols)
    t->cursor_x = cols - 1;
  if (t->cursor_y >= rows)
    t->cursor_y = rows - 1;
  t->caret_shown = 0;
  return 0;
}

void term_write(struct terminal *t, struct gl_surface *surf, const char *buf,
                size_t count) {
  if (!surf || !surf->buffer)
    return;

  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  int cols = t->cols;
  int rows = t->rows;

  for (size_t i = 0; i < count; i++) {
    char c = buf[i];

    if (t->escape_state == 0) {
      if (c == '\033') {
        t->escape_state = 1;
        t->escape_len = 0;
      } else if (c == '\n') {
        t->cursor_x = 0;
        t->cursor_y++;
      } else if (c == '\r') {
        t->cursor_x = 0;
      } else if (c == '\b' || c == 127) {
        if (t->cursor_x > 0)
          t->cursor_x--;
      } else if (c >= 32 && c < 127) {
        /* Check bounds and wrap BEFORE writing */
        if (t->cursor_x < 0)
          t->cursor_x = 0;
        if (t->cursor_y < 0)
          t->cursor_y = 0;
        if (t->cursor_x >= cols) {
          t->cursor_x = 0;
          t->cursor_y++;
        }
        if (t->cursor_y >= rows) {
          /* Scroll pixel buffer up by one text line */
          if (surf->height > char_h) {
            size_t line_size = (size_t)surf->stride * char_h;
            memmove(surf->buffer, surf->buffer + line_size,
                    (size_t)surf->stride * (surf->height - char_h) * 4);
          }
          /* Clear the freed last line */
          for (size_t p = (size_t)surf->stride * (surf->height - char_h);
               p < (size_t)surf->stride * surf->height; p++) {
            surf->buffer[p] = t->bg_color;
          }

          /* Scroll text grids */
          if (t->text_grid && t->attr_grid) {
            memmove(t->text_grid, t->text_grid + cols, (size_t)cols * (rows - 1));
            memmove(t->attr_grid, t->attr_grid + cols,
                    (size_t)cols * (rows - 1) * 4);
            int last_row_start = cols * (rows - 1);
            memset(t->text_grid + last_row_start, ' ', cols);
            for (int p = 0; p < cols; p++)
              t->attr_grid[last_row_start + p] = t->default_fg;
          }
          t->cursor_y = rows - 1;
        }

        /* Clear the glyph cell to the current background, then draw the glyph. */
        term_fill_rect(surf, t->cursor_x * char_w, t->cursor_y * char_h, char_w,
                       char_h, t->curr_bg_color);
        gl_draw_char(surf, t->cursor_x * char_w, t->cursor_y * char_h, c,
                     t->fg_color);

        if (t->text_grid && t->attr_grid) {
          int idx = t->cursor_y * cols + t->cursor_x;
          if (idx < cols * rows) {
            t->text_grid[idx] = c;
            t->attr_grid[idx] = t->fg_color;
          }
        }

        t->cursor_x++;
      }
    } else if (t->escape_state == 1) {
      if (c == '[')
        t->escape_state = 2;
      else
        t->escape_state = 0;
    } else if (t->escape_state == 2) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        handle_csi(t, surf, c, char_w, char_h);
        t->escape_state = 0;
      } else if (t->escape_len < 31) {
        t->escape_buf[t->escape_len++] = c;
      } else {
        t->escape_state = 0;
      }
    }
  }

  /* Draw the caret at its final position for this batch. */
  term_draw_cursor(t, surf, char_w, char_h);
}

void term_set_focus(struct terminal *t, struct gl_surface *surf, int focused) {
  if (focused == t->focused)
    return;
  if (!focused)
    term_clear_caret(t, surf);
  t->focused = focused;
}

void term_clear_caret(struct terminal *t, struct gl_surface *surf) {
  if (!surf || !surf->buffer)
    return;
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  term_erase_caret(t, surf, char_w, char_h);
}
