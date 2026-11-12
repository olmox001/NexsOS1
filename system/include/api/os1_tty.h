#ifndef OS1_TTY_H
#define OS1_TTY_H
/* Versioned per-session VT-ANSI model; no syscall or new kernel opcode. */
#include <stddef.h>
#include <stdint.h>

#define OS1_TTY_ENV "NX_TTY_PORT"
#define OS1_TTY_PORT_PREFIX "OS1nx_tty_"
#define OS1_TTY_WRITE 0x54590001U
/* Fixed-capacity text cells are owned by the session model, independently
 * from its pixel surface and current visible window dimensions. Upper limits
 * cover a 1024x768 viewport at the smallest supported font cell (5x8). */
#define OS1_TTY_COLS_MAX 256
#define OS1_TTY_ROWS_MAX 128
#define OS1_TTY_SURFACE_W_MAX 1024
#define OS1_TTY_SURFACE_H_MAX 768
#define OS1_TTY_CSI_MAX 32
#define OS1_TTY_FEED_MAX 4096U
/* Independent bounded VT byte history used to reconstruct logical lines
 * on resize. This is not a pixel buffer and is private to each session.
 * The oldest complete lines are retired when the history is full. */
#define OS1_TTY_HISTORY_BYTES 65536U
#define OS1_TTY_CELL_FG_DEFAULT 1U
#define OS1_TTY_CELL_BG_DEFAULT 2U
#define OS1_TTY_CELL_REVERSED 4U

struct os1_tty_cell {
  uint32_t fg, bg;
  uint8_t ch;
  uint8_t defaults; /* flags indicate theme colours, not explicit ANSI SGR */
};

struct os1_tty_model {
  struct os1_tty_cell cells[OS1_TTY_ROWS_MAX][OS1_TTY_COLS_MAX];
  uint8_t dirty[OS1_TTY_ROWS_MAX];
  int cols, rows, x, y, saved_x, saved_y;
  int cursor_visible, escape_mode, escape_len, bold;
  int reverse, fg_uses_default, bg_uses_default;
  char escape[OS1_TTY_CSI_MAX];
  uint32_t fg, bg, default_fg, default_bg;
  size_t history_len;
  int replaying;
  char history[OS1_TTY_HISTORY_BYTES];
};

/* Validate viewport geometry before allocating a pixel surface or mutating
 * the independent text-cell model. No clamped/truncated cell counts. */
int os1_tty_grid_from_pixels(int width, int height,
                              int cell_width, int cell_height,
                              int *cols, int *rows);
int os1_tty_init(struct os1_tty_model *model, int cols, int rows,
                 uint32_t fg, uint32_t bg);
int os1_tty_resize(struct os1_tty_model *model, int cols, int rows);
int os1_tty_set_palette(struct os1_tty_model *model,
                        uint32_t default_fg, uint32_t default_bg);
int os1_tty_feed(struct os1_tty_model *model, const char *bytes, size_t count);
#endif
