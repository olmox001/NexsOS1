/*
 * kernel/include/kernel/term.h
 * VT/ANSI terminal emulator — extracted from the compositor (GFX-DYN-01, #123).
 *
 * The terminal is a self-contained ECMA-48 subset emulator that operates on a
 * generic pixel target (struct gl_surface) plus its own backing cell grid.  It
 * has NO dependency on the window/compositor layer: the compositor owns the
 * struct terminal embedded in each window and hands the terminal the window's
 * content surface for each operation.  This is the seam on which the modern
 * terminal protocol (#123 problem 2) and terminal reflow on resize are built.
 */
#ifndef _KERNEL_TERM_H
#define _KERNEL_TERM_H

#include <kernel/types.h>
#include <stddef.h>

struct gl_surface;

/* Caret colour: 25%-alpha systemBlue block, blended over the cell so the glyph
 * stays readable.  Phase 5 (Style/Theme) will source this from the theme. */
#define TERM_CARET_COLOR 0x40007AFF

struct terminal {
  int cols, rows;         /* grid size in cells (derived from surface/font)   */
  int cursor_x, cursor_y; /* cursor cell position                              */
  int cursor_visible;     /* DECTCEM (\x1b[?25h/l); 1 = caret may be drawn     */
  int caret_px, caret_py; /* cell where the caret was last painted            */
  int caret_shown;        /* 1 = a caret is currently baked at caret_px/py     */
  int focused;            /* 1 = this terminal owns input → draw the caret     */
  uint8_t *text_grid;     /* cols*rows characters (persistence/repaint)        */
  uint32_t *attr_grid;    /* cols*rows per-cell foreground colours             */
  uint32_t fg_color;      /* current ANSI foreground                           */
  uint32_t default_fg;    /* SGR-reset / default foreground                    */
  uint32_t bg_color;      /* default (window) background                       */
  uint32_t curr_bg_color; /* current ANSI background                          */
  int escape_state;       /* 0=normal, 1=saw ESC, 2=in CSI                     */
  char escape_buf[32];    /* CSI parameter bytes                               */
  int escape_len;
};

/* Allocate the cell grids and initialise VT state.  Returns 0 on success, -1 if
 * a grid allocation failed (caller should treat the terminal as unusable). */
int term_init(struct terminal *t, int cols, int rows, uint32_t fg, uint32_t bg);

/* Free the cell grids (idempotent; safe on a zeroed terminal). */
void term_free(struct terminal *t);

/* Reconfigure the grid to new cols/rows: reallocates and clears the grids.
 * Pixel content is the caller's concern (it owns the surface).  Returns 0/-1. */
int term_resize(struct terminal *t, int cols, int rows);

/* Feed bytes to the terminal, rendering glyphs / handling ANSI escapes into
 * surf, and (re)drawing the caret at the end of the batch. */
void term_write(struct terminal *t, struct gl_surface *surf, const char *buf,
                size_t count);

/* Update focus: on focus loss the caret is erased; the focus flag gates whether
 * term_write draws a caret. */
void term_set_focus(struct terminal *t, struct gl_surface *surf, int focused);

/* Erase the caret if currently shown (idempotent). */
void term_clear_caret(struct terminal *t, struct gl_surface *surf);

#endif /* _KERNEL_TERM_H */
