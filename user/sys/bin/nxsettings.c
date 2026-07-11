/*
 * user/sys/bin/nxsettings.c
 * NEXS settings & system-info aggregator (ASTRA stratified service model).
 *
 * nxsettings groups, in one place, the read/write surfaces already exposed by
 * the other stratified helpers/tools (nxres, nxinfo.h, nxperm.h, nxproc.h,
 * nxmemstat.h, nxnotify/nxntfy_srv's registry log) instead of making the user
 * hunt across nxres/nxproc/nxperm/nxmemstat/nxnotify separately.  It holds NO
 * policy of its own: every value shown or changed goes through the same
 * kernel-gated syscall the standalone tool would use, so nxsettings is
 * "secure by caller" exactly like its siblings.
 *
 * Two front ends, one shared data layer:
 *
 *   GUI (default):  a normal, resizable compositor window (native titlebar,
 *     like nxlauncher) with a left sidebar of sections and a right content
 *     pane, drawn with the same custom-ARGB-framebuffer + font_lib approach
 *     as nxlauncher.c/nximage.c (no printf_win — this window owns its pixels).
 *
 *   CLI (`nxsettings -cli [section]`):  a one-shot textual dump on stdout,
 *     following the plain-text style of nxinfo/nxperm/nxproc/nxres.  With no
 *     section argument it prints everything; with one, only that section.
 *     Sections: display | style | system | perm | proc | mem | notify.
 *
 * Discovery / launch model (see nxexec.h, nxshell.c): nxsettings needs NO
 * special wiring.  Dropped into /sys/bin, it is found by nxlauncher's live
 * /sys/bin scan (grid tile) and by the shell's generic spawn_search_args()
 * fallback (typing `nxsettings` or `nxsettings -cli`).  run_foreground()'s
 * window-ownership debounce already tells the two modes apart: the GUI build
 * opens its own window and detaches; `-cli` opens none and stays hosted in
 * the caller's terminal — exactly like `nxproc` vs `nxproc kill <pid>` today.
 *
 * Sections and the module each one is backed by:
 *   Display       -> OS1_display_info/set_mode/set_zoom          (nxres.c)
 *   Style & Theme -> OS1_display_set_style                       (nxres.c)
 *   Background    -> OS1_display_set_background                  (nxres.c)
 *   System        -> nxinfo.h pure readers                       (nxinfo.h)
 *   Permissions   -> OS1_identity + nxperm.h tables               (nxperm.h)
 *   Processes     -> nxproc_snapshot/kill                        (nxproc.h)
 *   Memory        -> nxmemstat_snapshot (needs ROOT; see nxmemstat.h)
 *   Notifications -> sys.ntfy.log.* registry ring                (nxnotify.c)
 *
 * SECURITY MODEL: identical to every helper it wraps — nxsettings widens
 * nothing.  A caller without CAP_REG_WRITE/ROOT still sees its own identity
 * and whatever read-only data the kernel already grants; a failed
 * nxmemstat_snapshot() (non-ROOT caller) is reported inline instead of
 * crashing the section.
 */
#include <font_lib.h>
#include <input.h>
#include <os1.h>
#include <stdlib.h>
#include <string.h>

#include "nxinfo.h"
#include "nxmemstat.h"
#include "nxperm.h"
#include "nxproc.h"

/* ================================================================
 *                       Shared: notification log
 * ================================================================
 * Minimal reader over the sys.ntfy.log.* ring nxntfy_srv maintains (record
 * format "<pid>|<sev>|<state>|<text>" — see nxnotify.c/nxntfy_srv.c).  Kept
 * flat (no per-pid grouping) since both the GUI panel and the CLI dump only
 * need a quick recent-activity glance; `nxnotify list` remains the grouped,
 * authoritative view.
 */
#define NXS_NOTIFY_MAX 16

struct nxs_nrec {
  int pid;
  int sev;
  char state; /* 'U' unread, 'R' read */
  char text[48];
};

static int nxs_notify_fetch(struct nxs_nrec *out, int max) {
  char keys[640];
  int n = OS1_registry_enum_under("sys.ntfy.log.", keys, sizeof(keys));
  if (n <= 0)
    return 0;
  int cnt = 0;
  char key[64];
  int k = 0;
  for (const char *p = keys; cnt < max; p++) {
    if (*p == '\n' || *p == '\0') {
      if (k > 0) {
        key[k] = '\0';
        char val[80];
        if (OS1_registry_get(key, val, sizeof(val)) == 0) {
          const char *q = val;
          out[cnt].pid = atoi(q);
          while (*q && *q != '|')
            q++;
          if (*q)
            q++;
          out[cnt].sev = atoi(q);
          while (*q && *q != '|')
            q++;
          if (*q)
            q++;
          out[cnt].state = *q ? *q : 'U';
          while (*q && *q != '|')
            q++;
          if (*q)
            q++;
          int t = 0;
          for (; *q && t < 47; q++)
            out[cnt].text[t++] = *q;
          out[cnt].text[t] = '\0';
          cnt++;
        }
      }
      k = 0;
      if (*p == '\0')
        break;
    } else if (k < (int)sizeof(key) - 1) {
      key[k++] = *p;
    }
  }
  return cnt;
}

static const char *nxs_sev_str(int sev) {
  return sev == 2 ? "ERR " : sev == 1 ? "WARN" : "info";
}
static const char *nxs_state_str(char st) {
  return st == 'R' ? "read  " : "unread";
}

/* ================================================================
 *                        Shared: display presets
 * ================================================================ */
struct nxs_res {
  int w, h;
};
static const struct nxs_res NXS_RES_PRESETS[] = {
    {800, 600},
    {1024, 768},
    {1280, 720},
    {1920, 1080},
};
#define NXS_NRES (int)(sizeof(NXS_RES_PRESETS) / sizeof(NXS_RES_PRESETS[0]))

/* Style/theme/bg name tables — must mirror nxres.c's style_names/theme_names/
 * bg_names (which in turn mirror kernel/include/kernel/compositor_style.h). */
static const char *NXS_STYLES[] = {"nexs",  "classic", "material",
                                   "glass", "minimal", "retro"};
#define NXS_NSTYLES (int)(sizeof(NXS_STYLES) / sizeof(NXS_STYLES[0]))
static const char *NXS_THEMES[] = {"light", "dark"};
#define NXS_NTHEMES (int)(sizeof(NXS_THEMES) / sizeof(NXS_THEMES[0]))
static const char *NXS_BG[] = {
    "black", "red",     "green",  "yellow",  "blue" /* default */, "magenta",
    "cyan",  "white",   "gray",   "bred",    "bgreen",             "byellow",
    "bblue", "bmagenta", "bcyan", "bwhite",
};
#define NXS_NBG (int)(sizeof(NXS_BG) / sizeof(NXS_BG[0]))

/* ================================================================
 *                              CLI mode
 * ================================================================ */

static int nxs_streq(const char *a, const char *b) {
  return a && b && strncmp(a, b, 32) == 0;
}

static void cli_display(void) {
  long di = OS1_display_info();
  int w = (int)((di >> 16) & 0xFFFF);
  int h = (int)(di & 0xFFFF);
  printf("== Display ==\n");
  printf("resolution: %dx%d\n", w, h);
  printf("presets:    ");
  for (int i = 0; i < NXS_NRES; i++)
    printf("%dx%d%s", NXS_RES_PRESETS[i].w, NXS_RES_PRESETS[i].h,
           i + 1 < NXS_NRES ? "  " : "\n");
  printf("(nxsettings -cli does not change resolution/zoom; use the GUI or "
         "'nxres <w> <h>' / 'nxres zoom <pct>')\n");
}

static void cli_style(void) {
  printf("== Style & Theme ==\n");
  printf("styles: ");
  for (int i = 0; i < NXS_NSTYLES; i++)
    printf("%s%s", NXS_STYLES[i], i + 1 < NXS_NSTYLES ? " " : "\n");
  printf("themes: ");
  for (int i = 0; i < NXS_NTHEMES; i++)
    printf("%s%s", NXS_THEMES[i], i + 1 < NXS_NTHEMES ? " " : "\n");
  printf("backgrounds: ");
  for (int i = 0; i < NXS_NBG; i++)
    printf("%s%s", NXS_BG[i], i + 1 < NXS_NBG ? " " : "\n");
  printf("(no getter exists for the currently-applied style/theme/bg; use "
         "'nxres style|theme|bg <name>' or the GUI to change it)\n");
}

static void cli_system(void) { nxinfo_print_summary(-1); }

static void cli_perm(void) {
  printf("== Permissions ==\n");
  nxperm_print_identity(-1);
  printf("\n");
  nxperm_print_levels(-1);
  printf("\n");
  nxperm_print_services(-1);
}

static void cli_proc(void) {
  printf("== Processes ==\n");
  struct ps_info procs[NXPROC_MAX];
  int count = nxproc_snapshot(procs, NXPROC_MAX);
  if (count < 0) {
    printf("nxsettings: failed to fetch process list (err %d)\n", count);
    return;
  }
  nxproc_render_inline(procs, count);
}

static void cli_mem(void) {
  printf("== Memory ==\n");
  struct os1_sysstats s;
  long r = nxmemstat_snapshot(&s);
  if (r < 0) {
    printf("OS1_sys_stats denied (need ROOT) or failed (err %ld)\n", r);
    return;
  }
  unsigned long free_mb = (unsigned long)((s.pmm_free_pages * 4UL) / 1024UL);
  unsigned long tot_mb = (unsigned long)((s.pmm_total_pages * 4UL) / 1024UL);
  printf("pmm:    free %lu / %lu MB\n", free_mb, tot_mb);
  printf("kheap:  in-use %lu KB  hi %lu KB  live %lu\n",
         (unsigned long)(s.km_bytes_in_use / 1024UL),
         (unsigned long)(s.km_high_water_bytes / 1024UL),
         (unsigned long)s.km_live_allocs);
  printf("sched:  cpus %lu  runnable %lu  zombies %lu  ctxsw %lu\n",
         (unsigned long)s.sched_ncpu, (unsigned long)s.sched_runnable,
         (unsigned long)s.sched_zombie_count,
         (unsigned long)s.sched_ctx_switches);
  printf("objs:   FILE %lu  PROC %lu  REGKEY %lu  WINDOW %lu\n",
         (unsigned long)s.obj_live_by_type[OBJ_TYPE_FILE],
         (unsigned long)s.obj_live_by_type[OBJ_TYPE_PROCESS],
         (unsigned long)s.obj_live_by_type[OBJ_TYPE_REGKEY],
         (unsigned long)s.obj_live_by_type[OBJ_TYPE_WINDOW]);
}

static void cli_notify(void) {
  printf("== Notifications ==\n");
  struct nxs_nrec recs[NXS_NOTIFY_MAX];
  int n = nxs_notify_fetch(recs, NXS_NOTIFY_MAX);
  if (n == 0) {
    printf("no notifications\n");
    return;
  }
  for (int i = 0; i < n; i++)
    printf("PID %-5d [%s][%s] %s\n", recs[i].pid, nxs_sev_str(recs[i].sev),
           nxs_state_str(recs[i].state), recs[i].text);
}

static int cli_main(int argc, char **argv) {
  if (argc < 1) {
    cli_display();
    printf("\n");
    cli_style();
    printf("\n");
    cli_system();
    printf("\n");
    cli_perm();
    printf("\n");
    cli_proc();
    printf("\n");
    cli_mem();
    printf("\n");
    cli_notify();
    return 0;
  }
  const char *sec = argv[0];
  if (nxs_streq(sec, "display"))
    cli_display();
  else if (nxs_streq(sec, "style"))
    cli_style();
  else if (nxs_streq(sec, "system"))
    cli_system();
  else if (nxs_streq(sec, "perm"))
    cli_perm();
  else if (nxs_streq(sec, "proc"))
    cli_proc();
  else if (nxs_streq(sec, "mem"))
    cli_mem();
  else if (nxs_streq(sec, "notify"))
    cli_notify();
  else {
    printf("nxsettings: unknown section '%s'\n", sec);
    printf(
        "usage: nxsettings -cli [display|style|system|perm|proc|mem|notify]\n");
    return 1;
  }
  return 0;
}

/* ================================================================
 *                              GUI mode
 * ================================================================
 * Same rendering family as nxlauncher.c/nximage.c: an owned ARGB framebuffer
 * blitted each frame, glyphs drawn with font_lib directly onto that buffer
 * (no printf_win — this window is not a terminal).  A per-frame button table
 * doubles as the draw call and the hit-test source, rebuilt every redraw.
 */

enum nxs_section {
  SEC_DISPLAY = 0,
  SEC_STYLE,
  SEC_SYSTEM,
  SEC_PERM,
  SEC_PROC,
  SEC_MEM,
  SEC_NOTIFY,
  SEC_COUNT,
};

static const char *SEC_LABELS[SEC_COUNT] = {
    "Display",   "Style & Theme", "System",        "Permissions",
    "Processes", "Memory",        "Notifications",
};

enum nxs_action {
  ACT_NONE = 0,
  ACT_SECTION,
  ACT_RES_PRESET,
  ACT_ZOOM_DEC,
  ACT_ZOOM_INC,
  ACT_ZOOM_RESET,
  ACT_STYLE,
  ACT_THEME,
  ACT_BACKGROUND,
  ACT_KILL,
};

/* ------------------------- Window state --------------------------- */
static uint32_t *g_fb;
static int g_ww, g_wh;
static int g_win = -1;
static struct font_ctx *g_font;

static int g_section = SEC_DISPLAY;
static int g_zoom_pct = 100; /* tracked locally; no getter exists */
static int g_cur_style = -1; /* -1 = not set this session */
static int g_cur_theme = -1;
static int g_cur_bg = -1;

/* ------------------------- Layout ---------------------------------- */
#define SIDEBAR_W 168
#define ROW_H 34
#define PAD 16
#define LINE_H 18
#define CONTENT_X (SIDEBAR_W + PAD)
#define MIN_WIN_W 520
#define MIN_WIN_H 380

/* ------------------------- Palette ---------------------------------- */
#define COL_BG 0xFF1E1E24u
#define COL_SIDEBAR_BG 0xFF17171Cu
#define COL_SIDEBAR_SEL 0xFF33333Du
#define COL_TEXT 0xFFEAEAEEu
#define COL_TEXT_DIM 0xFF9A9AA4u
#define COL_ACCENT 0xFF4FA3FFu
#define COL_BTN 0xFF2C2C34u
#define COL_BTN_ACTIVE 0xFF3F7FD1u
#define COL_BTN_TEXT 0xFFFFFFFFu
#define COL_DANGER 0xFF7A2A2Au
#define COL_DANGER_TEXT 0xFFFFD0D0u

/* ------------------------- Button hit-table ------------------------- */
struct nxs_btn {
  int x, y, w, h;
  int action;
  long arg;
};
#define MAX_BTNS 64
static struct nxs_btn g_btns[MAX_BTNS];
static int g_btn_n;

static void add_btn(int x, int y, int w, int h, int action, long arg) {
  if (g_btn_n >= MAX_BTNS)
    return;
  g_btns[g_btn_n].x = x;
  g_btns[g_btn_n].y = y;
  g_btns[g_btn_n].w = w;
  g_btns[g_btn_n].h = h;
  g_btns[g_btn_n].action = action;
  g_btns[g_btn_n].arg = arg;
  g_btn_n++;
}

/* ============================================================
 *                        Pixel helpers
 * (identical technique to nxlauncher.c's fb_rrect/buf_draw_*)
 * ============================================================ */

static void fb_rrect(int x, int y, int w, int h, int r, uint32_t c) {
  if (w <= 0 || h <= 0)
    return;
  if (r < 0)
    r = 0;
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int ccx = -1, ccy = 0;
      if (i < r && j < r) {
        ccx = r;
        ccy = r;
      } else if (i >= w - r && j < r) {
        ccx = w - r - 1;
        ccy = r;
      } else if (i < r && j >= h - r) {
        ccx = r;
        ccy = h - r - 1;
      } else if (i >= w - r && j >= h - r) {
        ccx = w - r - 1;
        ccy = h - r - 1;
      }
      if (ccx >= 0) {
        int dx = i - ccx, dy = j - ccy;
        if (dx * dx + dy * dy > r * r)
          continue;
      }
      int px = x + i, py = y + j;
      if (px >= 0 && px < g_ww && py >= 0 && py < g_wh)
        g_fb[py * g_ww + px] = c;
    }
  }
}

static void buf_draw_glyph(int x, int y, uint32_t codepoint, uint32_t color) {
  if (!g_font)
    return;
  int idx = (int)codepoint - g_font->header.first_char;
  if (idx < 0 || idx >= g_font->header.num_chars)
    return;
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
  if (!g_font || !s)
    return 0;
  return font_string_width(g_font, s);
}

static void buf_draw_text(int x, int y, const char *s, uint32_t color) {
  if (!g_font || !s)
    return;
  uint32_t cp;
  int consumed, cursor = x;
  size_t rem = strlen(s);
  const char *p = s;
  while (*p) {
    consumed = utf8_decode(p, rem, &cp);
    if (consumed <= 0) {
      p++;
      rem--;
      continue;
    }
    buf_draw_glyph(cursor, y, cp, color);
    int idx = (int)cp - g_font->header.first_char;
    if (idx >= 0 && idx < g_font->header.num_chars)
      cursor += g_font->glyphs[idx].advance;
    p += consumed;
    rem -= consumed;
  }
}

/* ui_text: draw one line at (x, *y), then advance *y by LINE_H. */
static void ui_text(int x, int *y, const char *s, uint32_t color) {
  buf_draw_text(x, *y, s, color);
  *y += LINE_H;
}

/* ui_button: draw a rounded button + centred label AND register its hit
 * area, in one call — the button table is rebuilt every redraw so drawing
 * and hit-testing can never drift apart. */
static void ui_button(int x, int y, int w, int h, const char *label, int action,
                      long arg, int active) {
  fb_rrect(x, y, w, h, 6, active ? COL_BTN_ACTIVE : COL_BTN);
  if (g_font) {
    int tw = buf_text_width(label);
    int tx = x + (w - tw) / 2;
    if (tx < x + 2)
      tx = x + 2;
    int ty = y + (h - 16) / 2;
    buf_draw_text(tx, ty, label, COL_BTN_TEXT);
  }
  add_btn(x, y, w, h, action, arg);
}

/* ============================================================
 *                       Section renderers
 * ============================================================ */

static void render_sidebar(void) {
  fb_rrect(0, 0, SIDEBAR_W, g_wh, 0, COL_SIDEBAR_BG);
  int y = PAD;
  for (int i = 0; i < SEC_COUNT; i++) {
    int ry = y + i * ROW_H;
    if (i == g_section)
      fb_rrect(6, ry, SIDEBAR_W - 12, ROW_H - 6, 6, COL_SIDEBAR_SEL);
    buf_draw_text(18, ry + (ROW_H - 6 - 16) / 2, SEC_LABELS[i],
                  i == g_section ? COL_ACCENT : COL_TEXT);
    add_btn(6, ry, SIDEBAR_W - 12, ROW_H - 6, ACT_SECTION, i);
  }
}

static void render_display(void) {
  int cx = CONTENT_X, y = PAD;
  ui_text(cx, &y, "Display", COL_ACCENT);
  y += 6;

  long di = OS1_display_info();
  int w = (int)((di >> 16) & 0xFFFF), h = (int)(di & 0xFFFF);
  char line[128];
  snprintf(line, sizeof(line), "Current resolution: %dx%d", w, h);
  ui_text(cx, &y, line, COL_TEXT);
  y += 6;

  ui_text(cx, &y, "Presets:", COL_TEXT_DIM);
  int bx = cx;
  for (int i = 0; i < NXS_NRES; i++) {
    char lbl[24];
    snprintf(lbl, sizeof(lbl), "%dx%d", NXS_RES_PRESETS[i].w,
             NXS_RES_PRESETS[i].h);
    int active = (w == NXS_RES_PRESETS[i].w && h == NXS_RES_PRESETS[i].h);
    ui_button(bx, y, 92, 28, lbl, ACT_RES_PRESET, i, active);
    bx += 100;
  }
  y += 40;

  snprintf(line, sizeof(line), "Zoom: %d%%", g_zoom_pct);
  ui_text(cx, &y, line, COL_TEXT);
  ui_button(cx, y, 54, 28, "-10%", ACT_ZOOM_DEC, 0, 0);
  ui_button(cx + 62, y, 70, 28, "Reset", ACT_ZOOM_RESET, 0, 0);
  ui_button(cx + 140, y, 54, 28, "+10%", ACT_ZOOM_INC, 0, 0);
  y += 40;
}

static void render_style(void) {
  int cx = CONTENT_X, y = PAD;
  char line[96];

  ui_text(cx, &y, "Compositor style", COL_ACCENT);
  y += 4;
  int bx = cx;
  for (int i = 0; i < NXS_NSTYLES; i++) {
    ui_button(bx, y, 84, 28, NXS_STYLES[i], ACT_STYLE, i, g_cur_style == i);
    bx += 90;
  }
  y += 40;
  snprintf(line, sizeof(line), "applied: %s",
           g_cur_style >= 0 ? NXS_STYLES[g_cur_style] : "not set this session");
  ui_text(cx, &y, line, COL_TEXT_DIM);
  y += 16;

  ui_text(cx, &y, "Theme", COL_ACCENT);
  y += 4;
  bx = cx;
  for (int i = 0; i < NXS_NTHEMES; i++) {
    ui_button(bx, y, 84, 28, NXS_THEMES[i], ACT_THEME, i, g_cur_theme == i);
    bx += 90;
  }
  y += 40;
  snprintf(line, sizeof(line), "applied: %s",
           g_cur_theme >= 0 ? NXS_THEMES[g_cur_theme] : "not set this session");
  ui_text(cx, &y, line, COL_TEXT_DIM);
  y += 16;

  ui_text(cx, &y, "Background", COL_ACCENT);
  y += 4;
  bx = cx;
  int row_w = g_ww - cx - PAD;
  for (int i = 0; i < NXS_NBG; i++) {
    if (bx + 76 > cx + row_w) {
      bx = cx;
      y += 34;
    }
    ui_button(bx, y, 72, 28, NXS_BG[i], ACT_BACKGROUND, i, g_cur_bg == i);
    bx += 78;
  }
  y += 40;
  snprintf(line, sizeof(line), "applied: %s",
           g_cur_bg >= 0 ? NXS_BG[g_cur_bg] : "not set this session");
  ui_text(cx, &y, line, COL_TEXT_DIM);
}

static void render_system(void) {
  int cx = CONTENT_X, y = PAD;
  char line[160], cwd[128];

  ui_text(cx, &y, "System", COL_ACCENT);
  y += 6;

  snprintf(line, sizeof(line), "%s %s", NXINFO_OS_NAME, NXINFO_OS_VERSION);
  ui_text(cx, &y, line, COL_TEXT);

  long up_ms = nxinfo_uptime_ms();
  snprintf(line, sizeof(line), "Uptime: %ld ms (%ld s)", up_ms, up_ms / 1000);
  ui_text(cx, &y, line, COL_TEXT);

  int procs = nxinfo_proc_count();
  if (procs < 0)
    snprintf(line, sizeof(line), "Active processes: <unavailable> (err %d)",
             procs);
  else
    snprintf(line, sizeof(line), "Active processes: %d", procs);
  ui_text(cx, &y, line, COL_TEXT);

  int w = 0, h = 0;
  nxinfo_display(&w, &h);
  snprintf(line, sizeof(line), "Resolution: %dx%d", w, h);
  ui_text(cx, &y, line, COL_TEXT);

  snprintf(line, sizeof(line), "PID: %d", get_pid());
  ui_text(cx, &y, line, COL_TEXT);

  if (getcwd(cwd, sizeof(cwd)) != 0)
    cwd[0] = '\0';
  snprintf(line, sizeof(line), "CWD: %s", cwd[0] ? cwd : "?");
  ui_text(cx, &y, line, COL_TEXT);
}

static void render_perm(void) {
  int cx = CONTENT_X, y = PAD;
  char line[160], m[96];

  ui_text(cx, &y, "Permissions", COL_ACCENT);
  y += 6;

  int level = 0;
  unsigned int mask = 0;
  OS1_identity(&level, &mask);
  nxperm_mask_str(mask, m, (int)sizeof(m));
  snprintf(line, sizeof(line), "Level: %s", nxperm_level_name(level));
  ui_text(cx, &y, line, COL_TEXT);
  snprintf(line, sizeof(line), "Can:     %s", m);
  ui_text(cx, &y, line, COL_TEXT);
  y += 10;

  ui_text(cx, &y, "Available levels:", COL_ACCENT);
  for (int lv = 0; lv < PLVL_COUNT; lv++) {
    nxperm_mask_str(nxperm_level_mask(lv), m, (int)sizeof(m));
    snprintf(line, sizeof(line), "%-8s %s", nxperm_level_name(lv), m);
    ui_text(cx, &y, line, COL_TEXT_DIM);
  }
  y += 10;

  ui_text(cx, &y, "Services:", COL_ACCENT);
  for (int i = 0; i < NXPERM_NSERVICES; i++) {
    const struct nxperm_service *s = &NXPERM_SERVICES[i];
    snprintf(line, sizeof(line), "%-9s [%s] %s", s->name,
             (mask & s->cap) ? "si" : "no", s->desc);
    ui_text(cx, &y, line, (mask & s->cap) ? COL_TEXT : COL_TEXT_DIM);
  }
}

static void render_proc(void) {
  int cx = CONTENT_X, y = PAD;
  ui_text(cx, &y, "Processes", COL_ACCENT);
  y += 6;

  struct ps_info procs[NXPROC_MAX];
  int count = nxproc_snapshot(procs, NXPROC_MAX);
  if (count < 0) {
    char line[64];
    snprintf(line, sizeof(line), "error reading process table (%d)", count);
    ui_text(cx, &y, line, COL_TEXT_DIM);
    return;
  }

  char hdr[64];
  snprintf(hdr, sizeof(hdr), "%-5s %-14s %-9s %-4s", "PID", "NOME", "STATO",
           "PRIO");
  ui_text(cx, &y, hdr, COL_TEXT_DIM);
  y += 2;

  int row_h = 24;
  int max_rows = (g_wh - y - PAD) / row_h;
  if (max_rows > count)
    max_rows = count;
  if (max_rows < 0)
    max_rows = 0;

  for (int i = 0; i < max_rows; i++) {
    char line[80];
    snprintf(line, sizeof(line), "%-5d %-14s %-9s %-4d", procs[i].pid,
             procs[i].name, nxproc_state_str(procs[i].state),
             procs[i].priority);
    buf_draw_text(cx, y + 3, line, COL_TEXT);
    ui_button(g_ww - 74, y - 2, 58, 22, "Kill", ACT_KILL, procs[i].pid, 0);
    y += row_h;
  }
  if (count > max_rows) {
    char more[48];
    snprintf(more, sizeof(more), "... and %d more (resize the window)",
             count - max_rows);
    ui_text(cx, &y, more, COL_TEXT_DIM);
  }
}

static void render_mem(void) {
  int cx = CONTENT_X, y = PAD;
  ui_text(cx, &y, "Memory", COL_ACCENT);
  y += 6;

  struct os1_sysstats s;
  long r = nxmemstat_snapshot(&s);
  if (r < 0) {
    char line[80];
    snprintf(line, sizeof(line),
             "OS1_sys_stats denied (needs ROOT) or failed (%ld)", r);
    ui_text(cx, &y, line, COL_TEXT_DIM);
    return;
  }

  char line[128];
  unsigned long free_mb = (unsigned long)((s.pmm_free_pages * 4UL) / 1024UL);
  unsigned long tot_mb = (unsigned long)((s.pmm_total_pages * 4UL) / 1024UL);
  snprintf(line, sizeof(line), "PMM:   free %lu / %lu MB", free_mb, tot_mb);
  ui_text(cx, &y, line, COL_TEXT);

  snprintf(line, sizeof(line), "kheap: in-use %lu KB  hi %lu KB  live %lu",
           (unsigned long)(s.km_bytes_in_use / 1024UL),
           (unsigned long)(s.km_high_water_bytes / 1024UL),
           (unsigned long)s.km_live_allocs);
  ui_text(cx, &y, line, COL_TEXT);

  snprintf(line, sizeof(line), "sched: cpu %lu  runnable %lu  zombie %lu",
           (unsigned long)s.sched_ncpu, (unsigned long)s.sched_runnable,
           (unsigned long)s.sched_zombie_count);
  ui_text(cx, &y, line, COL_TEXT);

  snprintf(line, sizeof(line), "ctxsw: %lu totali",
           (unsigned long)s.sched_ctx_switches);
  ui_text(cx, &y, line, COL_TEXT);

  snprintf(line, sizeof(line),
           "objs:  FILE %lu  PROC %lu  REGKEY %lu  WINDOW %lu",
           (unsigned long)s.obj_live_by_type[OBJ_TYPE_FILE],
           (unsigned long)s.obj_live_by_type[OBJ_TYPE_PROCESS],
           (unsigned long)s.obj_live_by_type[OBJ_TYPE_REGKEY],
           (unsigned long)s.obj_live_by_type[OBJ_TYPE_WINDOW]);
  ui_text(cx, &y, line, COL_TEXT);
}

static void render_notify(void) {
  int cx = CONTENT_X, y = PAD;
  ui_text(cx, &y, "Recent notifications", COL_ACCENT);
  y += 6;

  struct nxs_nrec recs[NXS_NOTIFY_MAX];
  int n = nxs_notify_fetch(recs, NXS_NOTIFY_MAX);
  if (n == 0) {
    ui_text(cx, &y, "no notifications", COL_TEXT_DIM);
    return;
  }
  int max_rows = (g_wh - y - PAD) / LINE_H;
  if (max_rows > n)
    max_rows = n;
  for (int i = 0; i < max_rows; i++) {
    char line[96];
    snprintf(line, sizeof(line), "PID %-5d [%s][%s] %s", recs[i].pid,
             nxs_sev_str(recs[i].sev), nxs_state_str(recs[i].state),
             recs[i].text);
    uint32_t col = recs[i].sev == 2   ? 0xFFFF8080u
                   : recs[i].sev == 1 ? 0xFFFFD080u
                                      : COL_TEXT;
    ui_text(cx, &y, line, col);
  }
}

/* ============================================================
 *                       Redraw + input
 * ============================================================ */

static void redraw(void) {
  fb_rrect(0, 0, g_ww, g_wh, 0, COL_BG);
  g_btn_n = 0;
  render_sidebar();
  switch (g_section) {
  case SEC_DISPLAY:
    render_display();
    break;
  case SEC_STYLE:
    render_style();
    break;
  case SEC_SYSTEM:
    render_system();
    break;
  case SEC_PERM:
    render_perm();
    break;
  case SEC_PROC:
    render_proc();
    break;
  case SEC_MEM:
    render_mem();
    break;
  case SEC_NOTIFY:
    render_notify();
    break;
  default:
    break;
  }
}

static void dispatch(int action, long arg) {
  switch (action) {
  case ACT_SECTION:
    g_section = (int)arg;
    break;
  case ACT_RES_PRESET:
    if (arg >= 0 && arg < NXS_NRES)
      OS1_display_set_mode(NXS_RES_PRESETS[arg].w, NXS_RES_PRESETS[arg].h);
    break;
  case ACT_ZOOM_DEC:
    g_zoom_pct -= 10;
    if (g_zoom_pct < 50)
      g_zoom_pct = 50;
    OS1_display_set_zoom(g_zoom_pct);
    break;
  case ACT_ZOOM_INC:
    g_zoom_pct += 10;
    if (g_zoom_pct > 300)
      g_zoom_pct = 300;
    OS1_display_set_zoom(g_zoom_pct);
    break;
  case ACT_ZOOM_RESET:
    g_zoom_pct = 100;
    OS1_display_set_zoom(g_zoom_pct);
    break;
  case ACT_STYLE:
    if (arg >= 0 && arg < NXS_NSTYLES) {
      g_cur_style = (int)arg;
      OS1_display_set_style((int)arg, -1);
    }
    break;
  case ACT_THEME:
    if (arg >= 0 && arg < NXS_NTHEMES) {
      g_cur_theme = (int)arg;
      OS1_display_set_style(-1, (int)arg);
    }
    break;
  case ACT_BACKGROUND:
    if (arg >= 0 && arg < NXS_NBG) {
      g_cur_bg = (int)arg;
      OS1_display_set_background((int)arg);
    }
    break;
  case ACT_KILL:
    if (arg > 0)
      nxproc_kill((int)arg);
    break;
  default:
    break;
  }
}

static void handle_click(int mx, int my) {
  for (int i = g_btn_n - 1; i >= 0; i--) {
    struct nxs_btn *b = &g_btns[i];
    if (mx >= b->x && mx < b->x + b->w && my >= b->y && my < b->y + b->h) {
      dispatch(b->action, b->arg);
      return;
    }
  }
}

static void reinit_window(int ww, int wh) {
  if (ww < MIN_WIN_W)
    ww = MIN_WIN_W;
  if (wh < MIN_WIN_H)
    wh = MIN_WIN_H;
  uint32_t *nb = (uint32_t *)malloc((size_t)ww * (size_t)wh * 4u);
  if (nb) {
    free(g_fb);
    g_fb = nb;
  }
  g_ww = ww;
  g_wh = wh;
}

static int main_gui(void) {
  g_font = font_load("/fonts/Rewir-Light.off");

  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;

  int ww = (sw * 7) / 10;
  int wh = (sh * 7) / 10;
  if (ww > 760)
    ww = 760;
  if (wh > 520)
    wh = 520;
  if (ww < MIN_WIN_W)
    ww = MIN_WIN_W;
  if (wh < MIN_WIN_H)
    wh = MIN_WIN_H;
  int wx = (sw - ww) / 2, wy = (sh - wh) / 2;

  g_fb = (uint32_t *)malloc((size_t)ww * (size_t)wh * 4u);
  if (!g_fb)
    return 1;
  g_ww = ww;
  g_wh = wh;

  g_win = create_window(wx, wy, ww, wh, "nxsettings");
  if (g_win < 0)
    return 1;
  /* Normal window (no set_window_flags): native titlebar, free drag/resize,
   * same posture as nxlauncher — not top_most, not passive. */

  for (;;) {
    redraw();
    window_blit(g_win, 0, 0, g_ww, g_wh, g_fb);
    compositor_render();

    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_MOUSE && ev.mouse.button == MOUSE_BTN_LEFT &&
          ev.mouse.state == KEY_PRESSED) {
        handle_click(ev.mouse.x, ev.mouse.y);
      } else if (ev.type == INPUT_TYPE_RESIZE && ev.resize.w > 0 &&
                 ev.resize.h > 0) {
        reinit_window(ev.resize.w, ev.resize.h);
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
