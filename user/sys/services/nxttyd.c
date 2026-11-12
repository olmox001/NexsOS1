/*
 * nxttyd — per-session graphical terminal service.
 *
 * Moves the terminal protocol's normal executable-facing path out of
 * kernel/graphics/term.c. The legacy kernel parser stays only for old direct
 * console writers during the compatibility migration. This executable owns
 * the TTY surface, parses VT/ANSI via libos1's tty_model, renders with the
 * existing userland font/raster library, and forwards input IPC to the job.
 *
 * No UART graphics, focus-derived stdout routing, new syscall or PID-derived
 * grant. Writers acquire a PORT send capability and are additionally checked
 * against the live process ancestry of the one child launched here.
 */
#include "../bin/nxexec.h"
#include "../bin/nxres.h"
#include <font_lib.h>
#include <input.h>
#include <os1.h>
#include <os1_gfx.h>
#include <os1_gfx_buffer.h>
#include <os1_tty.h>
#include <os1_material_colors.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define TTY_WIDTH 640
#define TTY_HEIGHT 432
#define TTY_GRACE_MS 500
#define TTY_DISMISS_MS 10000
#define TTY_POLL_MS 15
#define TTY_MESSAGES_PER_FRAME 48U
#define TTY_INPUT_PER_FRAME 16U
#define TTY_MAX_PIXELS (1024U * 768U)

struct tty_session {
  struct os1_tty_model vt;
  struct font_ctx *font;
  /* This reuses the existing capability-backed GFX buffer service.
   * No duplicated backing storage, unbounded allocation or private VM model.
   * The active/retired ownership transitions are managed by libos1. */
  struct os1_gfx_buffer backing;
  uint32_t *pixels;
  size_t capacity;
  int win, port, width, height, cell_w, cell_h;
  uint32_t caret_color; /* independent of cell FG and window surface */
  int pid, shell, visible, finished, stopped, status;
  long last_theme_check;
  char name[64];
};

/* Repaint only the session's capability-owned pixel surface. Character
 * storage remains independent, in vt.cells, across any pixel allocation. */
static void tty_redraw_view(struct tty_session *s) {
  if (s->pixels && s->width > 0 && s->height > 0 &&
      s->capacity >= (size_t)s->width * (size_t)s->height) {
    struct os1_gfx_surface frame = {
      s->pixels, (size_t)s->width, (size_t)s->height,
      (size_t)s->width, s->capacity
    };
    os1_gfx_clear(&frame, s->vt.default_bg);
    /* Publish only after tty_flush has rendered every dirty row. */
  }
  for (int row = 0; row < OS1_TTY_ROWS_MAX; ++row)
    s->vt.dirty[row] = 1U;
}

/* The gfx service may swap active and then fail to retire the old mapping.
 * Recover ownership from the active capability, never reuse a stale pointer. */
static void tty_recover_backing(struct tty_session *s) {
  uint32_t *active = (uint32_t *)s->backing.active.view.address;
  size_t available = s->backing.active.view.bytes / sizeof(uint32_t);
  if (active && active != s->pixels && s->width > 0 && s->height > 0 &&
      available >= (size_t)s->width * (size_t)s->height) {
    s->pixels = active;
    s->capacity = available;
    tty_redraw_view(s);
  }
}

/* Transaction: validate cell grid, reserve pixels, commit viewport; on
 * failure old columns/rows remain valid and the service stays alive. */
static int tty_bounded_geometry(struct tty_session *s, int w, int h) {
  if (w <= 0 || h <= 0 || w > OS1_TTY_SURFACE_W_MAX ||
      h > OS1_TTY_SURFACE_H_MAX ||
      (size_t)w * (size_t)h > TTY_MAX_PIXELS) return -EINVAL;
  int cols = 0, rows = 0;
  if (os1_tty_grid_from_pixels(w, h, s->cell_w, s->cell_h,
                               &cols, &rows) != 0) return -EINVAL;
  if (s->width == w && s->height == h &&
      s->vt.cols == cols && s->vt.rows == rows) {
    tty_redraw_view(s); /* Paint after a kernel-side resize rollback. */
    return 0;
  }
  int reserved = OS1_gfx_buffer_reserve(&s->backing,
                                        (size_t)w * (size_t)h);
  if (reserved != 0) {
    tty_recover_backing(s);
    return reserved;
  }
  uint32_t *active = (uint32_t *)s->backing.active.view.address;
  size_t available = s->backing.active.view.bytes / sizeof(uint32_t);
  if (!active || available < (size_t)w * (size_t)h) return -EIO;
  int old_cols = s->vt.cols, old_rows = s->vt.rows;
  if (os1_tty_resize(&s->vt, cols, rows) != 0) return -EINVAL;
  /* A window-manager resize only changed presentation bounds. Adopt the
   * new logical buffer explicitly through our own WINDOW object before the
   * first new-size blit; otherwise text is cropped into stale source pixels. */
  if (s->win > 0) {
    int adopted = OS1_window_resize(s->win, w, h);
    if (adopted != 0) {
      int restored = os1_tty_resize(&s->vt, old_cols, old_rows);
      if (restored != 0)
        OS1_report_error("nxttyd.reflow_rollback", restored);
      tty_recover_backing(s);
      return adopted;
    }
  }
  s->pixels = active;
  s->capacity = available;
  s->width = w;
  s->height = h;
  tty_redraw_view(s);
  return 0;
}

static int tty_renderer_init(struct tty_session *s, int width, int height) {
  OS1_gfx_buffer_init(&s->backing);
  s->font = font_load("/fonts/Rewir-Light.off");
  if (!s->font) return -ENOENT;
  s->cell_w = font_string_width(s->font, "M");
  s->cell_h = (int)s->font->header.ascent + s->font->header.descent + 2;
  if (s->cell_w < 5 || s->cell_w > 32 ||
      s->cell_h < 8 || s->cell_h > 48) return -EINVAL;
  struct os1_material_tty_palette palette =
      os1_material_tty_palette(nxres_theme_is_light());
  int cols = 0, rows = 0;
  if (os1_tty_grid_from_pixels(width, height, s->cell_w, s->cell_h,
                               &cols, &rows) != 0) return -EINVAL;
  s->caret_color = palette.caret;
  if (os1_tty_init(&s->vt, cols, rows, palette.text_foreground,
                   palette.text_background) != 0) return -EINVAL;
  return tty_bounded_geometry(s, width, height);
}

static int tty_apply_theme(struct tty_session *s) {
  struct os1_material_tty_palette palette =
      os1_material_tty_palette(nxres_theme_is_light());
  if (s->vt.default_fg == palette.text_foreground &&
      s->vt.default_bg == palette.text_background &&
      s->caret_color == palette.caret) return 0;
  if (os1_tty_set_palette(&s->vt, palette.text_foreground,
                          palette.text_background) != 0) return -EINVAL;
  s->caret_color = palette.caret;
  tty_redraw_view(s);
  return 0;
}

static void tty_render_row(struct tty_session *s, int row) {
  if (!s->pixels || !s->font || row < 0 || row >= s->vt.rows) return;
  int y = row * s->cell_h;
  if (y >= s->height) return;
  int h = s->cell_h;
  if (y + h > s->height) h = s->height - y;
  size_t offset = (size_t)y * (size_t)s->width;
  struct os1_gfx_surface surface = {
    s->pixels + offset, (size_t)s->width, (size_t)h,
    (size_t)s->width, s->capacity - offset
  };
  struct os1_gfx_rect rect = {0, 0, s->width, h};
  os1_gfx_fill_rect(&surface, &rect, s->vt.default_bg);
  for (int col = 0; col < OS1_TTY_COLS_MAX && col < s->vt.cols; ++col) {
    const struct os1_tty_cell *cell = &s->vt.cells[row][col];
    int x = col * s->cell_w;
    struct os1_gfx_rect bg = {x, 0, s->cell_w, h};
    if (cell->bg != s->vt.default_bg)
      os1_gfx_fill_rect(&surface, &bg, cell->bg);
    if (cell->ch >= 32U && cell->ch < 127U && cell->ch != ' ') {
      char glyph[2] = {(char)cell->ch, 0};
      font_draw_string_surface(&surface, s->font, x, 0, glyph, cell->fg);
    }
  }
  if (s->vt.cursor_visible && !s->finished && s->vt.x < s->vt.cols &&
      s->vt.y == row) {
    struct os1_gfx_rect caret = {
      s->vt.x * s->cell_w, h > 2 ? h - 2 : 0, s->cell_w, 2
    };
    os1_gfx_fill_rect(&surface, &caret, s->caret_color);
  }
  s->vt.dirty[row] = 0U;
}

static void tty_flush(struct tty_session *s) {
  if (s->win <= 0 || s->pixels == NULL) return;
  int first = -1, last = -1;
  for (int row = 0; row < OS1_TTY_ROWS_MAX && row < s->vt.rows; ++row) {
    if (s->vt.dirty[row]) {
      tty_render_row(s, row);
      if (first < 0) first = row;
      last = row;
    }
  }
  if (first < 0) return;
  int y = first * s->cell_h;
  int height = (last - first + 1) * s->cell_h;
  if (last == s->vt.rows - 1) height = s->height - y;
  if (y < 0 || height <= 0 || y + height > s->height) return;
  /* One contiguous dirty band, rendered entirely in the existing mapped
   * backing first. Never expose a cleared frame or one publication per row. */
  size_t offset = (size_t)y * (size_t)s->width;
  OS1_window_blit(s->win, 0, y, s->width, height, s->pixels + offset);
}

static void tty_text(struct tty_session *s, const char *text, size_t len) {
  if (!s || os1_tty_feed(&s->vt, text, len) != 0) return;
}

/* SEND is acquired through a FAMILY-scoped capability. The kernel checked
 * the live ancestry at handle acquisition; the queued message remains
 * valid even after a short-lived command has exited and been reaped.
 * There is deliberately NO receiver-side lookup of a dead sender PID. */
static int tty_process_messages(struct tty_session *s) {
  for (unsigned step = 0U; step < TTY_MESSAGES_PER_FRAME; ++step) {
    struct os1_port_packet packet;
    int received = OS1_port_packet_receive(s->port, &packet, 1);
    if (received == -EAGAIN) return 0;
    if (received != 0) return received;
    if (packet.cap_count != 0U) {
      int closed = OS1_port_packet_close_caps(&packet);
      if (closed != 0) return closed;
      continue;
    }
    struct ipc_message msg = packet.message;
    if (msg.type == OS1_TTY_WRITE && msg.data1 <= sizeof(msg.payload)) {
      /*
       * Diagnostic SERIAL mirror (not rendering). The service has already
       * blitted its own surface, so self_rendered=1; the kernel is explicitly
       * forbidden to borrow keyboard focus to select a window. This mirrors
       * the original terminal diagnostic stream without duplicating pixels
       * or making UART authoritative for the GUI.
       *
       * OS1_object_write is intentionally the low-level object operation:
       * calling libc write() here would re-enter the TTY PORT transport.
       */
      long mirrored = OS1_object_write(1, msg.payload, (size_t)msg.data1);
      if (mirrored != (long)msg.data1)
        OS1_report_error("nxttyd.debug_mirror", (int)mirrored);
      tty_text(s, msg.payload, (size_t)msg.data1);
    }
  }
  return 0;
}

static int tty_handle_input(struct tty_session *s, int on_finished) {
  for (unsigned step = 0U; step < TTY_INPUT_PER_FRAME; ++step) {
    struct ipc_message msg = {0};
    long received = OS1low_ipc_try_recv(-1, &msg);
    if (received == -EAGAIN) return 0;
    if (received != 0) return -EIO;
    if (msg.type == IPC_TYPE_RESIZE) {
      int w = (int)msg.data1, h = (int)msg.data2;
      int resized = tty_bounded_geometry(s, w, h);
      if (resized != 0) {
        OS1_report_error("nxttyd.resize", resized);
        /* The window manager retains the last logical framebuffer and
         * userland nxcompositor scales it until a valid resize can commit. */
        continue;
      }
      if (!on_finished) {
        long sent = OS1low_ipc_send(s->pid, &msg);
        if (sent != 0) return -EIO;
      }
    } else if (msg.type == IPC_TYPE_INPUT && msg.data2 != 0U) {
      char c = msg.payload[0];
      if (on_finished && (c == '\r' || c == '\n')) return 1;
      if (!on_finished) {
        if (!s->shell && c == 0x03) {
          if (kill_process(s->pid) < 0) return -EIO;
          s->finished = 1;
          tty_text(s, "^C\r\n", 4U);
          continue;
        }
        long sent = OS1low_ipc_send(s->pid, &msg);
        if (sent != 0) return -EIO;
        if (!s->shell && c >= 32 && c < 127)
          tty_text(s, &c, 1U);
        if (!s->shell && (c == '\n' || c == '\r'))
          tty_text(s, "\r\n", 2U);
      }
    } else if (msg.type == IPC_TYPE_NOTIFY &&
               msg.data2 == IPC_LOOK_PING_MAGIC) {
      /* Keep explicit SGR colours; update theme-derived cells in place. */
      if (tty_apply_theme(s) != 0) return -EIO;
    }
  }
  return 0;
}

static int tty_step(struct tty_session *s, int finished) {
  /* Sessions are not global nxres singletons, so a broadcast cannot name
   * every one. Poll the same canonical theme key at a bounded 350 ms cadence
   * as well as accepting an immediate LOOK ping when one is delivered. */
  long now = OS1_time_now();
  if (s->last_theme_check <= 0 || now < s->last_theme_check ||
      now - s->last_theme_check >= 350) {
    s->last_theme_check = now;
    if (tty_apply_theme(s) != 0) return -EIO;
  }
  int result = tty_process_messages(s);
  if (result != 0) return result;
  result = tty_handle_input(s, finished);
  tty_flush(s);
  OS1_sleep(TTY_POLL_MS);
  return result;
}

static void tty_release(struct tty_session *s) {
  if (s->port >= 0) {
    long closed = OS1low_handle_close(s->port);
    if (closed != 0) OS1_report_error("nxttyd.port_close", (int)closed);
  }
  if (s->win > 0) destroy_window(s->win);
  if (s->font) font_free(s->font);
  int released = OS1_gfx_buffer_release(&s->backing);
  if (released != 0) OS1_report_error("nxttyd.buffer_release", released);
}

/*
 * The NEXS ELF startup contract passes argc and a pointer to a contiguous
 * array of argument addresses, with the first array word at argv offset 0.
 * This descriptor is an ABI view of that same raw address vector; it does not
 * add indirection, copy argv or reinterpret the contents as another object.
 * It is explicitly freestanding OS ABI, not a hosted C main declaration.
 */
struct tty_argv_vector {
  char *values[NXEXEC_ARGV_MAX];
};

static int tty_open_session(struct tty_session *s, const char *title) {
  int server_pid = get_pid();
  int written = snprintf(s->name, sizeof(s->name), "%s%d",
                         OS1_TTY_PORT_PREFIX, server_pid);
  if (written <= 0 || (size_t)written >= sizeof(s->name)) return -EOVERFLOW;
  s->port = OS1_port_create_family(s->name);
  if (s->port < 0) return s->port;
  int result = tty_renderer_init(s, TTY_WIDTH, TTY_HEIGHT);
  if (result != 0) return result;
  long dimensions = OS1_display_info();
  int sw = (int)((dimensions >> 16) & 0xffff);
  int sh = (int)(dimensions & 0xffff);
  int wx = (sw - TTY_WIDTH) / 2 + ((server_pid * 31) % 160);
  int wy = (sh - TTY_HEIGHT) / 2 + ((server_pid * 13) % 110);
  if (wx < 0) wx = 45;
  if (wy < 0) wy = 65;
  s->win = create_window(wx, wy, TTY_WIDTH, TTY_HEIGHT, title);
  if (s->win <= 0) return -EIO;
  set_window_flags(s->win, 4);
  /* First paint must cover every pixel, not just full text-cell rows. */
  OS1_window_blit(s->win, 0, 0, s->width, s->height, s->pixels);
  /* Establish self_rendered before the first child message arrives. The
   * surface stays hidden until nxexec has classified its ELF behavior. */
  tty_flush(s);
  /* Environment propagation is the EXISTING process-create mechanism.
   * stdout/stderr remain ordinary CONSOLE descriptor types; file/pipe
   * redirections continue bypassing the TTY renderer. */
  return setenv(OS1_TTY_ENV, s->name, 1) == 0 ? 0 : -EIO;
}

static int tty_spawn_job(struct tty_session *s, int argc,
                         const struct tty_argv_vector *args,
                         const char *program, const char *title) {
  char path[NXEXEC_PATH_MAX];
  char *job_argv[NXEXEC_ARGV_MAX];
  for (int i = 0; i < NXEXEC_ARGV_MAX; ++i)
    job_argv[i] = i + 1 < argc ? args->values[i + 1] : NULL;
  job_argv[0] = (char *)program;
  int job_argc = argc - 1;
  struct spawn_redir redirection[SPAWN_MAX_REDIR];
  int fds[SPAWN_MAX_REDIR], nredir = 0, nfds = 0;
  int parsed = nxexec_strip_redirections(&job_argc, job_argv, redirection,
                                         &nredir, fds, &nfds);
  if (parsed != 0 || job_argc <= 0) {
    const char *error = "Invalid command or redirection\r\n";
    tty_text(s, error, strlen(error));
    s->finished = 1;
  } else {
    s->pid = execsvc_launch(job_argc, job_argv, EXECSVC_MODE_FOREGROUND,
                            redirection, nredir, path, sizeof(path));
  }
  for (int i = 0; i < SPAWN_MAX_REDIR && i < nfds; ++i) {
    int rc = close(fds[i]);
    if (rc != 0) OS1_report_error("nxttyd.redir_close", errno);
  }
  if (s->pid <= 0) {
    const char *message = "Unable to execute: ";
    tty_text(s, message, strlen(message));
    tty_text(s, program, strlen(program));
    tty_text(s, "\r\n", 2U);
    s->finished = 1;
    return 0; /* Show the actual error in the owned graphical TTY. */
  }
  nxexec_register_identity(s->pid, title);
  return 0;
}

static int tty_wait_job(struct tty_session *s) {
  if (s->finished) {
    set_window_flags(s->win, 2);
    set_focus(get_pid());
    tty_flush(s);
    return 0;
  }
  int last_win = -1, stable = 0;
  for (unsigned probe = 0U; probe < TTY_GRACE_MS / TTY_POLL_MS && !s->finished; ++probe) {
    if (s->pid > 0 && nxexec_window_stable(s->pid, &last_win, &stable))
      return 1;
    if (s->pid > 0 && OS1low_process_wait_status(s->pid, &s->status) != -1) {
      s->finished = 1;
    }
    if (!s->finished && tty_step(s, 0) < 0) return -EIO;
  }
  set_window_flags(s->win, 2);
  set_focus(get_pid());
  s->visible = 1;
  if (s->pid > 0 && !s->finished) {
    for (uint64_t step = 0U; step < UINT64_MAX && !s->finished; ++step) {
      if (nxexec_window_stable(s->pid, &last_win, &stable)) return 1;
      if (OS1low_process_wait_status(s->pid, &s->status) != -1) {
        s->finished = 1;
      }
      if (!s->finished && tty_step(s, 0) < 0) return -EIO;
    }
  }
  return 0;
}

static void tty_dismiss(struct tty_session *s) {
  if (s->pid > 0) nxexec_unregister_identity(s->pid);
  s->finished = 1;
  const char *end = "\r\n[process terminated - press Enter to close]\r\n";
  tty_text(s, end, strlen(end));
  int result = 0;
  for (unsigned step = 0U; step < TTY_DISMISS_MS / TTY_POLL_MS &&
                          result == 0; ++step) {
    result = tty_step(s, 1);
  }
  tty_flush(s);
}

int main(int argc, const struct tty_argv_vector *args) {
  if (argc < 2 || argc > NXEXEC_ARGV_MAX || args == NULL ||
      args->values[1] == NULL) return 1;
  static struct tty_session session;
  struct tty_session *s = &session;
  s->port = -1;
  s->win = -1;
  s->pid = -1;
  s->shell = strcmp(args->values[1], "/sys/bin/nxshell") == 0 ||
             strcmp(args->values[1], "nxshell") == 0;
  char clean[NXEXEC_PATH_MAX];
  const char *program = nxexec_strip_path_quotes(
      args->values[1], clean, sizeof(clean));
  if (!program || program[0] == '\0') return 1;
  char title[64];
  int written = snprintf(title, sizeof(title), "%s",
                         nxexec_basename(program));
  if (written <= 0 || (size_t)written >= sizeof(title)) return 1;
  int status = tty_open_session(s, title);
  if (status == 0) status = tty_spawn_job(s, argc, args, program, title);
  if (status == 0) status = tty_wait_job(s);
  if (status == 0) tty_dismiss(s);
  tty_release(s);
  return status < 0 ? 1 : 0;
}
