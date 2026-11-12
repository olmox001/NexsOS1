/*
 * user/sys/bin/nxpower.c
 * NEXS power control (nxpower) — the dedicated shutdown/restart program.
 *
 * Usage:
 *   nxpower                 open a small confirm dialog (Shutdown/Restart/
 *                            Cancel), launched by nxbar's [X] root menu's
 *                            "Power..." item (or directly from the shell).
 *   nxpower -cli status      report whether the kernel actually supports a
 *                            power action yet (see "Known gap" below).
 *   nxpower -cli shutdown    attempt a shutdown.
 *   nxpower -cli reboot      attempt a reboot.
 *
 * KNOWN GAP — read before wiring this into anything (issue: no ticket yet):
 *   Every OTHER piece of this program is a real, complete implementation:
 *   the confirm-dialog UI, the two-step "click again to confirm" arm/disarm
 *   flow (mirrors the deliberate friction of a real power switch), the CLI
 *   parsing, and the plumbing that will carry the actual request down to the
 *   kernel.  What does NOT exist anywhere in os1.h or any other file in this
 *   codebase is a syscall that actually powers the machine off or resets it
 *   — there is no OS1_power_*, no ACPI/PSCI wrapper, nothing.  Rather than
 *   fabricate a call that would fail to link (a "real" API that silently
 *   does nothing, or worse, misuse an unrelated syscall like killing pid 1
 *   to fake it), nxpower declares the ONE symbol it needs,
 *   `_sys_power_action`, as a WEAK extern (GCC/GNU-ld `__attribute__((weak))`
 *   — a real, standard technique, not a hack specific to this file): if the
 *   kernel/syscall-shim library does not yet define it, the symbol resolves
 *   to a null function pointer at link time and nxpower reports "not
 *   available" instead of crashing or lying; the day a real
 *   `_sys_power_action(int action)` (0=shutdown, 1=reboot) lands in the
 *   syscall shim — alongside the other `_sys_*` externs already declared in
 *   os1.h, e.g. `_sys_window_enum`, `_sys_get_time` — nxpower starts working
 *   with NO changes to this file. Suggested kernel-side contract:
 *     - one new syscall number, handler gated by PLVL_ROOT/PLVL_MACHINE
 *       (rebooting/halting the whole machine is strictly more privileged
 *       than anything CAP_* currently gates — see nxperm.h's level model);
 *     - action 0 = orderly shutdown, action 1 = reboot;
 *     - return 0 on success, a negative errno if unsupported/denied, so
 *       nxpower_action() below (which already handles a negative return)
 *       needs no changes either way.
 *
 * SECURITY MODEL: nxpower adds no ambient authority of its own — the moment
 * the kernel wires `_sys_power_action`, the same PLVL gate written above
 * decides who may actually use it, exactly like every other stratified
 * helper in this codebase; nxpower only ever relays the outcome.
 */
#include <font_lib.h>
#include <input.h>
#include <os1.h>
#include <string.h>

/* See "KNOWN GAP" above: real, standard weak-symbol bridge to a kernel
 * syscall that does not exist yet.  `__attribute__((weak))` on an extern
 * function means the linker accepts an undefined reference and resolves it
 * to NULL instead of failing, IF nothing else in the link provides it. */
extern long _sys_power_action(int action) __attribute__((weak));

#define NXPOWER_SHUTDOWN 0
#define NXPOWER_REBOOT 1

/* nxpower_action - relay to the kernel if it exists; otherwise -1 (ENOSYS-
 * shaped: "not available"), never fabricated success. */
static long nxpower_action(int action) {
  if (!_sys_power_action)
    return -1;
  return _sys_power_action(action);
}

static int nxpower_available(void) { return _sys_power_action != 0; }

/* ================================================================
 *                              CLI mode
 * ================================================================ */
static int nxs_streq(const char *a, const char *b) {
  return a && b && strncmp(a, b, 32) == 0;
}

static int cli_main(int argc, char **argv) {
  if (argc < 1) {
    printf("usage: nxpower -cli status|shutdown|reboot\n");
    return 1;
  }
  if (nxs_streq(argv[0], "status")) {
    if (nxpower_available())
      printf("power control: available (kernel provides _sys_power_action)\n");
    else
      printf("power control: NOT available in this kernel build — see the "
             "\"KNOWN GAP\" note at the top of nxpower.c\n");
    return 0;
  }
  if (nxs_streq(argv[0], "shutdown")) {
    long r = nxpower_action(NXPOWER_SHUTDOWN);
    if (r == 0) { printf("nxpower: shutdown requested\n"); return 0; }
    printf("nxpower: shutdown not available (err %ld) — kernel has no power "
           "syscall yet\n", r);
    return 1;
  }
  if (nxs_streq(argv[0], "reboot")) {
    long r = nxpower_action(NXPOWER_REBOOT);
    if (r == 0) { printf("nxpower: reboot requested\n"); return 0; }
    printf("nxpower: reboot not available (err %ld) — kernel has no power "
           "syscall yet\n", r);
    return 1;
  }
  printf("nxpower: unknown command '%s'\n", argv[0]);
  printf("usage: nxpower -cli status|shutdown|reboot\n");
  return 1;
}

/* ================================================================
 *                              GUI mode
 * ================================================================
 * Same self-rendered-ARGB-framebuffer + font_lib technique as
 * nxlauncher.c/nximage.c/nxsettings.c/nxbar.c — no shared header for it
 * exists in this codebase yet, so it is duplicated here, consistent with
 * every other GUI tool.
 */
#define WIN_W 260
#define WIN_H 210
#define BTN_H 34
#define BTN_GAP 10
#define BTN_MARGIN 16

#define CONFIRM_WINDOW_MS 4000 /* the armed state expires after this long */

#define COL_BG 0xFF232328u
#define COL_TEXT 0xFFEAEAEEu
#define COL_TEXT_DIM 0xFF9A9AA4u
#define COL_BTN 0xFF34343Cu
#define COL_BTN_ARMED 0xFF8A2A2Au
#define COL_BTN_CANCEL 0xFF3C3C46u
#define COL_BTN_TEXT 0xFFFFFFFFu

static uint32_t *g_fb;
static int g_ww = WIN_W, g_wh = WIN_H;
static int g_win = -1;
static struct font_ctx *g_font;

static void fb_rect(int x, int y, int w, int h, uint32_t c) {
  if (w <= 0 || h <= 0) return;
  int x2 = x + w, y2 = y + h;
  if (x2 > g_ww) x2 = g_ww;
  if (y2 > g_wh) y2 = g_wh;
  for (int j = (y < 0 ? 0 : y); j < y2; j++)
    for (int i = (x < 0 ? 0 : x); i < x2; i++)
      g_fb[j * g_ww + i] = c;
}

static void buf_draw_glyph(int x, int y, uint32_t codepoint, uint32_t color) {
  if (!g_font) return;
  int idx = (int)codepoint - g_font->header.first_char;
  if (idx < 0 || idx >= g_font->header.num_chars) return;
  struct font_glyph_info *gi = &g_font->glyphs[idx];
  uint8_t *bitmap = g_font->bitmap + gi->data_offset;
  int start_x = x + gi->x0;
  int start_y = y + g_font->header.ascent + gi->y0;
  for (int gy = 0; gy < gi->height; gy++) {
    for (int gx = 0; gx < gi->width; gx++) {
      uint8_t alpha = bitmap[gy * gi->width + gx];
      if (alpha > 64) {
        int px = start_x + gx, py = start_y + gy;
        if (px >= 0 && px < g_ww && py >= 0 && py < g_wh)
          g_fb[py * g_ww + px] = color;
      }
    }
  }
}

static int buf_text_width(const char *s) {
  if (!g_font || !s) return 0;
  return font_string_width(g_font, s);
}

static void buf_draw_text(int x, int y, const char *s, uint32_t color) {
  if (!g_font || !s) return;
  uint32_t cp;
  int consumed, cursor = x;
  size_t rem = strlen(s);
  const char *p = s;
  while (*p) {
    consumed = utf8_decode(p, rem, &cp);
    if (consumed <= 0) { p++; rem--; continue; }
    buf_draw_glyph(cursor, y, cp, color);
    int idx = (int)cp - g_font->header.first_char;
    if (idx >= 0 && idx < g_font->header.num_chars)
      cursor += g_font->glyphs[idx].advance;
    p += consumed;
    rem -= consumed;
  }
}

static void draw_button(int x, int y, int w, int h, const char *label,
                        uint32_t bg) {
  fb_rect(x, y, w, h, bg);
  if (g_font) {
    int tw = buf_text_width(label);
    buf_draw_text(x + (w - tw) / 2, y + (h - 16) / 2, label, COL_BTN_TEXT);
  }
}

enum { ARM_NONE = 0, ARM_SHUTDOWN, ARM_REBOOT };
static int g_armed = ARM_NONE;
static long g_armed_at_ms;
static char g_status_line[80] = "";

static int g_btn_shutdown_y, g_btn_reboot_y, g_btn_cancel_y;
#define BTN_X BTN_MARGIN
#define BTN_W (WIN_W - 2 * BTN_MARGIN)

static void redraw(void) {
  /* disarm automatically once the confirm window has passed */
  if (g_armed != ARM_NONE && get_time() - g_armed_at_ms > CONFIRM_WINDOW_MS) {
    g_armed = ARM_NONE;
  }

  fb_rect(0, 0, g_ww, g_wh, COL_BG);
  if (g_font)
    buf_draw_text(BTN_MARGIN, 14, "System Power", COL_TEXT);

  int y = 44;
  g_btn_shutdown_y = y;
  draw_button(BTN_X, y, BTN_W, BTN_H,
             g_armed == ARM_SHUTDOWN ? "Click again to confirm" : "Shut Down",
             g_armed == ARM_SHUTDOWN ? COL_BTN_ARMED : COL_BTN);
  y += BTN_H + BTN_GAP;

  g_btn_reboot_y = y;
  draw_button(BTN_X, y, BTN_W, BTN_H,
             g_armed == ARM_REBOOT ? "Click again to confirm" : "Restart",
             g_armed == ARM_REBOOT ? COL_BTN_ARMED : COL_BTN);
  y += BTN_H + BTN_GAP;

  g_btn_cancel_y = y;
  draw_button(BTN_X, y, BTN_W, BTN_H, "Cancel", COL_BTN_CANCEL);
  y += BTN_H + BTN_GAP;

  if (g_status_line[0] && g_font)
    buf_draw_text(BTN_MARGIN, y, g_status_line, COL_TEXT_DIM);

  window_blit(g_win, 0, 0, g_ww, g_wh, g_fb);
}

static void do_action(int action) {
  long r = nxpower_action(action);
  if (r == 0) {
    /* A real kernel would already have cut power/reset by the time this
     * would run; if we are still here, treat it as success and just wait
     * to be torn down. */
    snprintf(g_status_line, sizeof(g_status_line), "requested.");
  } else {
    snprintf(g_status_line, sizeof(g_status_line),
             "not available in this kernel build (err %ld)", r);
  }
  g_armed = ARM_NONE;
}

static void handle_click(int my) {
  if (my >= g_btn_shutdown_y && my < g_btn_shutdown_y + BTN_H) {
    if (g_armed == ARM_SHUTDOWN) do_action(NXPOWER_SHUTDOWN);
    else { g_armed = ARM_SHUTDOWN; g_armed_at_ms = get_time(); g_status_line[0] = '\0'; }
    return;
  }
  if (my >= g_btn_reboot_y && my < g_btn_reboot_y + BTN_H) {
    if (g_armed == ARM_REBOOT) do_action(NXPOWER_REBOOT);
    else { g_armed = ARM_REBOOT; g_armed_at_ms = get_time(); g_status_line[0] = '\0'; }
    return;
  }
  if (my >= g_btn_cancel_y && my < g_btn_cancel_y + BTN_H) {
    destroy_window(g_win);
    exit(0);
  }
}

static int main_gui(void) {
  g_font = font_load("/fonts/Rewir-Light.off");

  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF), sh = (int)((di) & 0xFFFF);
  if (sw <= 0) sw = 800;
  if (sh <= 0) sh = 600;
  int wx = (sw - WIN_W) / 2, wy = (sh - WIN_H) / 2;

  g_fb = (uint32_t *)malloc((size_t)WIN_W * WIN_H * 4);
  if (!g_fb) return 1;
  g_win = create_window(wx, wy, WIN_W, WIN_H, "nxpower");
  if (g_win < 0) return 1;
  /* Normal dialog window (no set_window_flags): native titlebar, closable
   * like any other app — Cancel and the titlebar close both just quit. */

  if (!nxpower_available())
    snprintf(g_status_line, sizeof(g_status_line),
             "note: kernel has no power syscall yet (see nxpower.c)");

  for (;;) {
    redraw();
    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_MOUSE && ev.mouse.button == MOUSE_BTN_LEFT &&
          ev.mouse.state == KEY_PRESSED) {
        handle_click(ev.mouse.y);
      }
    }
    OS1_sleep(40);
  }
  return 0;
}

/* ================================================================
 *                               main
 * ================================================================ */
int main(int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    if (nxs_streq(argv[i], "-cli"))
      return cli_main(argc - (i + 1), &argv[i + 1]);
  }
  return main_gui();
}
