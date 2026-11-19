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
#include "nxinfo.h"
#include "nxmemstat.h"
#include "nxperm.h"
#include "nxproc.h"
#include <nxres.h>
#include <font_lib.h>
#include <input.h>
#include <os1.h>
#include <os1_surface.h>
#include <stdlib.h>
#include <string.h>

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

/* Style/theme/bg name tables: the shared os1_style_names/os1_theme_names/
 * os1_bg_names (style_names.h, pulled in via nxres.h) — single source of
 * truth also used by nxres.c and the kernel's SYS_SET_STYLE registry
 * mirror, so this file no longer keeps its own copy to fall out of sync. */
#define NXS_STYLES os1_style_names
#define NXS_NSTYLES OS1_STYLE_COUNT
#define NXS_THEMES os1_theme_names
#define NXS_NTHEMES OS1_THEME_COUNT
#define NXS_BG os1_bg_names
#define NXS_NBG OS1_BG_COUNT

#define NXS_MODE_PAGE_SIZE 8U
static const int NXS_SCALE_OPTIONS[] = {100, 125, 150, 175, 200, 250, 300, 400};
#define NXS_SCALE_COUNT \
  ((int)(sizeof(NXS_SCALE_OPTIONS) / sizeof(NXS_SCALE_OPTIONS[0])))

static int nxs_mode_query(uint32_t index, struct os1_display_mode_v1 *mode) {
  return (int)OS1_display_mode_query(index, mode);
}

static int nxs_current_mode(struct os1_display_mode_v1 *current) {
  if (current == NULL)
    return -EINVAL;
  struct os1_display_mode_v1 mode;
  if (nxs_mode_query(0U, &mode) != 0)
    return -ENODEV;
  uint32_t count = mode.count > OS1_DISPLAY_MODE_MAX
                       ? OS1_DISPLAY_MODE_MAX
                       : mode.count;
  for (uint32_t i = 0U; i < count; ++i) {
    if (i != 0U && nxs_mode_query(i, &mode) != 0)
      return -EIO;
    if (mode.flags & OS1_DISPLAY_MODE_CURRENT) {
      *current = mode;
      return 0;
    }
  }
  return -ENODEV;
}

static int nxs_active_scale(void) {
  struct os1_display_workspace_v1 workspace;
  if (OS1_display_workspace_query(&workspace) != 0)
    return OS1_DISPLAY_ZOOM_DEFAULT_PERCENT;
  return (int)workspace.zoom_percent;
}

static void nxs_mode_label(const struct os1_display_mode_v1 *mode,
                           char *label, size_t capacity) {
  if (mode == NULL || label == NULL || capacity == 0U)
    return;
  const char *preferred =
      (mode->flags & OS1_DISPLAY_MODE_PREFERRED) ? " preferred" : "";
  if (mode->refresh_millihz != 0U) {
    snprintf(label, capacity, "%ux%u @ %u.%03u Hz%s", mode->width,
             mode->height, mode->refresh_millihz / 1000U,
             mode->refresh_millihz % 1000U, preferred);
  } else {
    snprintf(label, capacity, "%ux%u%s", mode->width, mode->height, preferred);
  }
}

/* ================================================================
 *                              CLI mode
 * ================================================================ */

static int nxs_streq(const char *a, const char *b) {
  return a && b && strncmp(a, b, 32) == 0;
}

static void cli_display(void) {
  int logical_w = 0, logical_h = 0;
  printf("== Display ==\n");
  int display_status = OS1_display_get_size(&logical_w, &logical_h);
  if (display_status == 0)
    printf("logical desktop: %dx%d\n", logical_w, logical_h);
  else
    printf("logical desktop: unavailable (%d)\n", display_status);

  struct os1_display_mode_v1 mode;
  if (nxs_mode_query(0U, &mode) != 0) {
    printf("hardware modes: unavailable\n");
    return;
  }
  uint32_t count = mode.count > OS1_DISPLAY_MODE_MAX
                       ? OS1_DISPLAY_MODE_MAX
                       : mode.count;
  printf("hardware modes: %u, generation %llu\n", count,
         (unsigned long long)mode.generation);
  for (uint32_t i = 0U; i < count; ++i) {
    if (i != 0U && nxs_mode_query(i, &mode) != 0)
      break;
    char label[96] = {0};
    nxs_mode_label(&mode, label, sizeof(label));
    printf("  %u: %s%s\n", i, label,
           (mode.flags & OS1_DISPLAY_MODE_CURRENT) ? " [current]" : "");
  }
  struct os1_display_workspace_v1 workspace;
  if (OS1_display_workspace_query(&workspace) == 0)
    printf("Logical policy: %s base %ux%u zoom %u%%\n",
           workspace.mode == OS1_DISPLAY_WORKSPACE_AUTO ? "auto" : "manual",
           workspace.base_width, workspace.base_height, workspace.zoom_percent);
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

  char sbuf[32] = "unknown", tbuf[32] = "unknown", bbuf[32] = "unknown";
  nxres_get_style(sbuf, sizeof(sbuf));
  nxres_get_theme(tbuf, sizeof(tbuf));
  nxres_get_background(bbuf, sizeof(bbuf));
  printf("applied: style=%s theme=%s background=%s\n", sbuf, tbuf, bbuf);
  printf("(use 'nxres style|theme|bg <name>' or the GUI to change it)\n");
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
  ACT_MODE_MENU,
  ACT_MODE_SELECT,
  ACT_MODE_PAGE_PREV,
  ACT_MODE_PAGE_NEXT,
  ACT_LOGICAL_AUTO,
  ACT_LOGICAL_MANUAL,
  ACT_LOGICAL_MENU,
  ACT_LOGICAL_SELECT,
  ACT_LOGICAL_PREV,
  ACT_LOGICAL_NEXT,
  ACT_SCALE_MENU,
  ACT_SCALE_SELECT,
  ACT_STYLE,
  ACT_THEME,
  ACT_BACKGROUND,
  ACT_KILL,
};

/* ------------------------- Window state --------------------------- */
static uint32_t *g_fb;
static int g_ww, g_wh;
static int g_display_w, g_display_h;
static int g_win = -1;
static struct font_ctx *g_font;

static int g_section = SEC_DISPLAY;
static int g_zoom_pct = OS1_DISPLAY_ZOOM_DEFAULT_PERCENT;
static int g_mode_menu_open;
static int g_scale_menu_open;
static int g_logical_menu_open;
static uint32_t g_mode_page;
static uint32_t g_logical_page;
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
#define NXS_GFX_MAX_DIM 4096

/* ------------------------- Dynamic palette -------------------------- */
static uint32_t g_col_bg, g_col_sidebar_bg, g_col_sidebar_sel;
static uint32_t g_col_text, g_col_text_dim, g_col_accent;
static uint32_t g_col_btn, g_col_btn_active, g_col_btn_text;
static uint32_t g_col_danger, g_col_danger_text;

static void load_theme_colors(int is_light) {
  if (is_light) {
    /* Light palette aligned with the compositor's light surface family. */
    g_col_bg = 0xFFF5F5F7;
    g_col_sidebar_bg = 0xFFE5E5EA;
    g_col_sidebar_sel = 0xFFD1D1D6;
    g_col_text = 0xFF1C1C1E;
    g_col_text_dim = 0xFF6E6E73;
    g_col_accent = 0xFF007AFF;
    g_col_btn = 0xFFE0E0E0;
    g_col_btn_active = 0xFF007AFF;
    g_col_btn_text = 0xFFFFFFFF;
    g_col_danger = 0xFFFFD0D0;
    g_col_danger_text = 0xFF7A2A2A;
  } else {
    /* Dark palette retained from the original settings surface. */
    g_col_bg = 0xFF1E1E24u;
    g_col_sidebar_bg = 0xFF17171Cu;
    g_col_sidebar_sel = 0xFF33333Du;
    g_col_text = 0xFFEAEAEEu;
    g_col_text_dim = 0xFF9A9AA4u;
    g_col_accent = 0xFF4FA3FFu;
    g_col_btn = 0xFF2C2C34u;
    g_col_btn_active = 0xFF3F7FD1u;
    g_col_btn_text = 0xFFFFFFFFu;
    g_col_danger = 0xFF7A2A2Au;
    g_col_danger_text = 0xFFFFD0D0u;
  }
}

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
    size_t step = (size_t)consumed;
    if (step > rem) {
      ++p;
      --rem;
      continue;
    }
    buf_draw_glyph(cursor, y, cp, color);
    int idx = (int)cp - g_font->header.first_char;
    if (idx >= 0 && idx < g_font->header.num_chars)
      cursor += g_font->glyphs[idx].advance;
    p += step;
    rem -= step;
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
  fb_rrect(x, y, w, h, 6, active ? g_col_btn_active : g_col_btn);
  if (g_font) {
    int tw = buf_text_width(label);
    int tx = x + (w - tw) / 2;
    if (tx < x + 2)
      tx = x + 2;
    int ty = y + (h - 16) / 2;
    buf_draw_text(tx, ty, label, g_col_btn_text);
  }
  add_btn(x, y, w, h, action, arg);
}

#define NXS_SELECT_W 320
#define NXS_SELECT_H 28
#define NXS_MENU_ROW_H 24

static void render_menu_row(int x, int y, int w, const char *label,
                            int action, long arg, int active) {
  if (active)
    fb_rrect(x + 1, y + 1, w - 2, NXS_MENU_ROW_H - 2, 4, g_col_sidebar_sel);
  buf_draw_text(x + 10, y + 4, label, active ? g_col_accent : g_col_text);
  add_btn(x, y, w, NXS_MENU_ROW_H, action, arg);
}

static void render_mode_dropdown(int x, int y) {
  struct os1_display_mode_v1 head;
  if (nxs_mode_query(0U, &head) != 0)
    return;
  uint32_t count = head.count > OS1_DISPLAY_MODE_MAX
                       ? OS1_DISPLAY_MODE_MAX
                       : head.count;
  uint32_t pages =
      (count + NXS_MODE_PAGE_SIZE - 1U) / NXS_MODE_PAGE_SIZE;
  if (pages == 0U)
    return;
  if (g_mode_page >= pages)
    g_mode_page = pages - 1U;
  uint32_t start = g_mode_page * NXS_MODE_PAGE_SIZE;
  uint32_t shown = count - start;
  if (shown > NXS_MODE_PAGE_SIZE)
    shown = NXS_MODE_PAGE_SIZE;
  int footer = pages > 1U ? 2 : 0;
  int height = ((int)shown + footer) * NXS_MENU_ROW_H;
  fb_rrect(x, y, NXS_SELECT_W, height, 5, g_col_btn);
  for (uint32_t row = 0U; row < shown; ++row) {
    uint32_t index = start + row;
    struct os1_display_mode_v1 mode;
    if (nxs_mode_query(index, &mode) != 0)
      continue;
    char label[104] = {0};
    nxs_mode_label(&mode, label, sizeof(label));
    render_menu_row(x, y + (int)row * NXS_MENU_ROW_H, NXS_SELECT_W, label,
                    ACT_MODE_SELECT, (long)index,
                    (mode.flags & OS1_DISPLAY_MODE_CURRENT) != 0U);
  }
  if (pages > 1U) {
    int row_y = y + (int)shown * NXS_MENU_ROW_H;
    render_menu_row(x, row_y, NXS_SELECT_W, "< previous modes",
                    ACT_MODE_PAGE_PREV, 0, 0);
    render_menu_row(x, row_y + NXS_MENU_ROW_H, NXS_SELECT_W, "next modes >",
                    ACT_MODE_PAGE_NEXT, 0, 0);
  }
}

static void render_logical_dropdown(int x, int y,
                                    const struct os1_display_workspace_v1 *workspace) {
  struct os1_display_mode_v1 head;
  if (nxs_mode_query(0U, &head) != 0) return;
  uint32_t count = head.count > OS1_DISPLAY_MODE_MAX
                       ? OS1_DISPLAY_MODE_MAX : head.count;
  uint32_t pages = (count + NXS_MODE_PAGE_SIZE - 1U) / NXS_MODE_PAGE_SIZE;
  if (pages == 0U) return;
  if (g_logical_page >= pages) g_logical_page = pages - 1U;
  uint32_t start = g_logical_page * NXS_MODE_PAGE_SIZE;
  uint32_t shown = count - start;
  if (shown > NXS_MODE_PAGE_SIZE) shown = NXS_MODE_PAGE_SIZE;
  int cell_w = NXS_SELECT_W / 2;
  int rows = ((int)shown + 1) / 2;
  int extra = pages > 1U ? 1 : 0;
  fb_rrect(x, y, NXS_SELECT_W, (rows + extra) * NXS_MENU_ROW_H, 5, g_col_btn);
  for (uint32_t row = 0U; row < NXS_MODE_PAGE_SIZE && row < shown; ++row) {
    struct os1_display_mode_v1 mode;
    if (nxs_mode_query(start + row, &mode) != 0) continue;
    char label[32];
    int written = snprintf(label, sizeof(label), "%ux%u", mode.width, mode.height);
    if (written <= 0) continue;
    int px = x + (int)(row % 2U) * cell_w;
    int py = y + (int)(row / 2U) * NXS_MENU_ROW_H;
    render_menu_row(px, py, cell_w, label, ACT_LOGICAL_SELECT,
                    (long)(start + row),
                    workspace->mode == OS1_DISPLAY_WORKSPACE_MANUAL &&
                    workspace->base_width == mode.width &&
                    workspace->base_height == mode.height);
  }
  if (extra != 0) {
    int py = y + rows * NXS_MENU_ROW_H;
    render_menu_row(x, py, cell_w, "< previous", ACT_LOGICAL_PREV, 0, 0);
    render_menu_row(x + cell_w, py, cell_w, "next >", ACT_LOGICAL_NEXT, 0, 0);
  }
}

static void render_scale_dropdown(int x, int y) {
  /* Two columns instead of eight vertical rows: at 300/400 percent the
   * logical desktop can be shorter than the original 192px dropdown.
   * All low zoom choices must remain hit-testable to escape that state. */
  const int columns = 2;
  const int rows = (NXS_SCALE_COUNT + columns - 1) / columns;
  const int cell_w = NXS_SELECT_W / columns;
  fb_rrect(x, y, NXS_SELECT_W, rows * NXS_MENU_ROW_H, 5, g_col_btn);
  for (int i = 0; i < NXS_SCALE_COUNT; ++i) {
    char label[32];
    int row = i / columns, column = i % columns;
    snprintf(label, sizeof(label), "%d%%", NXS_SCALE_OPTIONS[i]);
    render_menu_row(x + column * cell_w, y + row * NXS_MENU_ROW_H,
                    cell_w, label, ACT_SCALE_SELECT, NXS_SCALE_OPTIONS[i],
                    g_zoom_pct == NXS_SCALE_OPTIONS[i]);
  }
}

/* Low-resolution recovery view. At high UI zoom the logical desktop may
 * be only 160x120: full sidebar and 320px selectors cannot be hit. Always
 * expose every supported zoom choice directly, in a bounded 3x3 grid. */
static void render_compact_zoom(void) {
  int active = nxs_active_scale();
  char title[48];
  int text = snprintf(title, sizeof(title), "UI zoom %d%%", active);
  if (text > 0) buf_draw_text(5, 4, title, g_col_accent);
  const int columns = 3;
  const int margin = 4;
  const int gap = 2;
  const int cell_w = (g_ww - 2 * margin - 2 * gap) / columns;
  const int row_height = 21;
  if (cell_w < 28) return;
  for (int i = 0; i < NXS_SCALE_COUNT; ++i) {
    char label[20];
    int written = snprintf(label, sizeof(label), "%d%%", NXS_SCALE_OPTIONS[i]);
    if (written <= 0) continue;
    int column = i % columns;
    int row = i / columns;
    ui_button(margin + column * (cell_w + gap),
              24 + row * row_height, cell_w, row_height - 1,
              label, ACT_SCALE_SELECT, NXS_SCALE_OPTIONS[i],
              active == NXS_SCALE_OPTIONS[i]);
  }
  if (g_wh >= 122) {
    struct os1_display_workspace_v1 workspace;
    if (OS1_display_workspace_query(&workspace) == 0) {
      char desc[64];
      int written = snprintf(desc, sizeof(desc), "%s %ux%u",
          workspace.mode == OS1_DISPLAY_WORKSPACE_AUTO ? "Auto" : "Manual",
          workspace.base_width, workspace.base_height);
      if (written > 0) buf_draw_text(5, 99, desc, g_col_text_dim);
    }
  }
}

/* ============================================================
 *                       Section renderers
 * ============================================================ */

static void render_sidebar(void) {
  fb_rrect(0, 0, SIDEBAR_W, g_wh, 0, g_col_sidebar_bg);
  int y = PAD;
  for (int i = 0; i < SEC_COUNT; i++) {
    int ry = y + i * ROW_H;
    if (i == g_section)
      fb_rrect(6, ry, SIDEBAR_W - 12, ROW_H - 6, 6, g_col_sidebar_sel);
    buf_draw_text(18, ry + (ROW_H - 6 - 16) / 2, SEC_LABELS[i],
                  i == g_section ? g_col_accent : g_col_text);
    add_btn(6, ry, SIDEBAR_W - 12, ROW_H - 6, ACT_SECTION, i);
  }
}

static void render_display(void) {
  int cx = CONTENT_X, y = PAD;
  char label[128];
  struct os1_display_workspace_v1 workspace;
  int status = (int)OS1_display_workspace_query(&workspace);
  ui_text(cx, &y, "Display", g_col_accent);
  if (status == 0) {
    (void)snprintf(label, sizeof(label), "Desktop: %ux%u logical",
                   workspace.logical_width, workspace.logical_height);
    ui_text(cx, &y, label, g_col_text);
  } else {
    ui_text(cx, &y, "Desktop unavailable", g_col_text_dim);
  }
  ui_text(cx, &y, "Physical output", g_col_text_dim);
  struct os1_display_mode_v1 physical;
  char mode_label[104] = "Hardware modes unavailable";
  if (nxs_current_mode(&physical) == 0)
    nxs_mode_label(&physical, mode_label, sizeof(mode_label));
  int mode_y = y;
  ui_button(cx, y, NXS_SELECT_W, NXS_SELECT_H, mode_label,
            ACT_MODE_MENU, 0, g_mode_menu_open);
  y += NXS_SELECT_H + 7;

  ui_text(cx, &y, "Logical desktop - base resolution", g_col_text_dim);
  int half = (NXS_SELECT_W - 4) / 2;
  ui_button(cx, y, half, NXS_SELECT_H, "Auto (hardware)",
            ACT_LOGICAL_AUTO, 0,
            status == 0 && workspace.mode == OS1_DISPLAY_WORKSPACE_AUTO);
  ui_button(cx + half + 4, y, half, NXS_SELECT_H, "Manual",
            ACT_LOGICAL_MANUAL, 0,
            status == 0 && workspace.mode == OS1_DISPLAY_WORKSPACE_MANUAL);
  y += NXS_SELECT_H + 4;
  int logical_y = y;
  if (status == 0) {
    (void)snprintf(label, sizeof(label), "%ux%u base (choose size)",
                   workspace.base_width, workspace.base_height);
    ui_button(cx, y, NXS_SELECT_W, NXS_SELECT_H, label,
              ACT_LOGICAL_MENU, 0, g_logical_menu_open);
  }
  y += NXS_SELECT_H + 7;

  ui_text(cx, &y, "Zoom / UI scale (independent)", g_col_text_dim);
  g_zoom_pct = status == 0 ? (int)workspace.zoom_percent : 100;
  (void)snprintf(label, sizeof(label), "%d%%", g_zoom_pct);
  int scale_y = y;
  ui_button(cx, y, NXS_SELECT_W, NXS_SELECT_H, label,
            ACT_SCALE_MENU, 0, g_scale_menu_open);
  if (g_mode_menu_open) render_mode_dropdown(cx, mode_y + NXS_SELECT_H + 2);
  if (g_logical_menu_open && status == 0)
    render_logical_dropdown(cx, logical_y + NXS_SELECT_H + 2, &workspace);
  if (g_scale_menu_open)
    render_scale_dropdown(cx, scale_y + NXS_SELECT_H + 2);
}

static void render_style(void) {
  int cx = CONTENT_X, y = PAD;
  char line[96];

  ui_text(cx, &y, "Compositor style", g_col_accent);
  y += 4;
  int bx = cx;
  for (int i = 0; i < NXS_NSTYLES; i++) {
    ui_button(bx, y, 84, 28, NXS_STYLES[i], ACT_STYLE, i, g_cur_style == i);
    bx += 90;
  }
  y += 40;
  snprintf(line, sizeof(line), "applied: %s",
           g_cur_style >= 0 ? NXS_STYLES[g_cur_style] : "not set this session");
  ui_text(cx, &y, line, g_col_text_dim);
  y += 16;

  ui_text(cx, &y, "Theme", g_col_accent);
  y += 4;
  bx = cx;
  for (int i = 0; i < NXS_NTHEMES; i++) {
    ui_button(bx, y, 84, 28, NXS_THEMES[i], ACT_THEME, i, g_cur_theme == i);
    bx += 90;
  }
  y += 40;
  snprintf(line, sizeof(line), "applied: %s",
           g_cur_theme >= 0 ? NXS_THEMES[g_cur_theme] : "not set this session");
  ui_text(cx, &y, line, g_col_text_dim);
  y += 16;

  ui_text(cx, &y, "Background", g_col_accent);
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
  ui_text(cx, &y, line, g_col_text_dim);
}

static void render_system(void) {
  int cx = CONTENT_X, y = PAD;
  char line[160], cwd[128];

  ui_text(cx, &y, "System", g_col_accent);
  y += 6;

  snprintf(line, sizeof(line), "%s %s", NXINFO_OS_NAME, NXINFO_OS_VERSION);
  ui_text(cx, &y, line, g_col_text);

  long up_ms = nxinfo_uptime_ms();
  snprintf(line, sizeof(line), "Uptime: %ld ms (%ld s)", up_ms, up_ms / 1000);
  ui_text(cx, &y, line, g_col_text);

  int procs = nxinfo_proc_count();
  if (procs < 0)
    snprintf(line, sizeof(line), "Active processes: <unavailable> (err %d)",
             procs);
  else
    snprintf(line, sizeof(line), "Active processes: %d", procs);
  ui_text(cx, &y, line, g_col_text);

  int w = 0, h = 0;
  nxinfo_display(&w, &h);
  snprintf(line, sizeof(line), "Resolution: %dx%d", w, h);
  ui_text(cx, &y, line, g_col_text);

  snprintf(line, sizeof(line), "PID: %d", get_pid());
  ui_text(cx, &y, line, g_col_text);

  if (getcwd(cwd, sizeof(cwd)) != 0)
    cwd[0] = '\0';
  snprintf(line, sizeof(line), "CWD: %s", cwd[0] ? cwd : "?");
  ui_text(cx, &y, line, g_col_text);
}

static void render_perm(void) {
  int cx = CONTENT_X, y = PAD;
  char line[160], m[96];

  ui_text(cx, &y, "Permissions", g_col_accent);
  y += 6;

  int level = 0;
  unsigned int mask = 0;
  OS1_identity(&level, &mask);
  nxperm_mask_str(mask, m, (int)sizeof(m));
  snprintf(line, sizeof(line), "Level: %s", nxperm_level_name(level));
  ui_text(cx, &y, line, g_col_text);
  snprintf(line, sizeof(line), "Can:     %s", m);
  ui_text(cx, &y, line, g_col_text);
  y += 10;

  ui_text(cx, &y, "Available levels:", g_col_accent);
  for (int lv = 0; lv < PLVL_COUNT; lv++) {
    nxperm_mask_str(nxperm_level_mask(lv), m, (int)sizeof(m));
    snprintf(line, sizeof(line), "%-8s %s", nxperm_level_name(lv), m);
    ui_text(cx, &y, line, g_col_text_dim);
  }
  y += 10;

  ui_text(cx, &y, "Services:", g_col_accent);
  for (int i = 0; i < NXPERM_NSERVICES; i++) {
    const struct nxperm_service *s = &NXPERM_SERVICES[i];
    snprintf(line, sizeof(line), "%-9s [%s] %s", s->name,
             (mask & s->cap) ? "yes" : "no", s->desc);
    ui_text(cx, &y, line, (mask & s->cap) ? g_col_text : g_col_text_dim);
  }
}

static void render_proc(void) {
  int cx = CONTENT_X, y = PAD;
  ui_text(cx, &y, "Processes", g_col_accent);
  y += 6;

  struct ps_info procs[NXPROC_MAX];
  int count = nxproc_snapshot(procs, NXPROC_MAX);
  if (count < 0) {
    char line[64];
    snprintf(line, sizeof(line), "error reading process table (%d)", count);
    ui_text(cx, &y, line, g_col_text_dim);
    return;
  }

  char hdr[64];
  snprintf(hdr, sizeof(hdr), "%-5s %-14s %-9s %-4s", "PID", "NAME", "STATES",
           "PRIO");
  ui_text(cx, &y, hdr, g_col_text_dim);
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
    buf_draw_text(cx, y + 3, line, g_col_text);
    ui_button(g_ww - 74, y - 2, 58, 22, "Kill", ACT_KILL, procs[i].pid, 0);
    y += row_h;
  }
  if (count > max_rows) {
    char more[48];
    snprintf(more, sizeof(more), "... and %d more (resize the window)",
             count - max_rows);
    ui_text(cx, &y, more, g_col_text_dim);
  }
}

static void render_mem(void) {
  int cx = CONTENT_X, y = PAD;
  ui_text(cx, &y, "Memory", g_col_accent);
  y += 6;

  struct os1_sysstats s;
  long r = nxmemstat_snapshot(&s);
  if (r < 0) {
    char line[80];
    snprintf(line, sizeof(line),
             "OS1_sys_stats denied (needs ROOT) or failed (%ld)", r);
    ui_text(cx, &y, line, g_col_text_dim);
    return;
  }

  char line[128];
  unsigned long free_mb = (unsigned long)((s.pmm_free_pages * 4UL) / 1024UL);
  unsigned long tot_mb = (unsigned long)((s.pmm_total_pages * 4UL) / 1024UL);
  snprintf(line, sizeof(line), "PMM:   free %lu / %lu MB", free_mb, tot_mb);
  ui_text(cx, &y, line, g_col_text);

  snprintf(line, sizeof(line), "kheap: in-use %lu KB  hi %lu KB  live %lu",
           (unsigned long)(s.km_bytes_in_use / 1024UL),
           (unsigned long)(s.km_high_water_bytes / 1024UL),
           (unsigned long)s.km_live_allocs);
  ui_text(cx, &y, line, g_col_text);

  snprintf(line, sizeof(line), "sched: cpu %lu  runnable %lu  zombie %lu",
           (unsigned long)s.sched_ncpu, (unsigned long)s.sched_runnable,
           (unsigned long)s.sched_zombie_count);
  ui_text(cx, &y, line, g_col_text);

  snprintf(line, sizeof(line), "ctxsw: %lu totali",
           (unsigned long)s.sched_ctx_switches);
  ui_text(cx, &y, line, g_col_text);

  snprintf(line, sizeof(line),
           "objs:  FILE %lu  PROC %lu  REGKEY %lu  WINDOW %lu",
           (unsigned long)s.obj_live_by_type[OBJ_TYPE_FILE],
           (unsigned long)s.obj_live_by_type[OBJ_TYPE_PROCESS],
           (unsigned long)s.obj_live_by_type[OBJ_TYPE_REGKEY],
           (unsigned long)s.obj_live_by_type[OBJ_TYPE_WINDOW]);
  ui_text(cx, &y, line, g_col_text);
}

static void render_notify(void) {
  int cx = CONTENT_X, y = PAD;
  ui_text(cx, &y, "Recent notifications", g_col_accent);
  y += 6;

  struct nxs_nrec recs[NXS_NOTIFY_MAX];
  int n = nxs_notify_fetch(recs, NXS_NOTIFY_MAX);
  if (n == 0) {
    ui_text(cx, &y, "no notifications", g_col_text_dim);
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
                                      : g_col_text;
    ui_text(cx, &y, line, col);
  }
}

/* ============================================================
 *                       Redraw + input
 * ============================================================ */

static void redraw(void) {
  fb_rrect(0, 0, g_ww, g_wh, 0, g_col_bg);
  g_btn_n = 0;
  if (g_ww < CONTENT_X + NXS_SELECT_W + PAD || g_wh < 220) {
    /* The compact recovery UI is useful even if the active sidebar page was
     * changed just before entering a high-zoom desktop. */
    render_compact_zoom();
    return;
  }
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
    g_mode_menu_open = 0;
    g_scale_menu_open = 0;
    g_logical_menu_open = 0;
    break;
  case ACT_MODE_MENU:
    g_mode_menu_open = !g_mode_menu_open;
    g_scale_menu_open = 0;
    g_logical_menu_open = 0;
    break;
  case ACT_MODE_SELECT: {
    if (arg < 0 || (unsigned long)arg >= OS1_DISPLAY_MODE_MAX)
      break;
    struct os1_display_mode_v1 mode;
    int queried = nxs_mode_query((uint32_t)arg, &mode);
    int status = queried == 0
                     ? (int)OS1_display_mode_set((uint32_t)arg, mode.generation)
                     : queried;
    if (status != 0)
      OS1_report_error("nxsettings.display_mode", status);
    g_mode_menu_open = 0;
    break;
  }
  case ACT_MODE_PAGE_PREV:
    if (g_mode_page > 0U)
      --g_mode_page;
    break;
  case ACT_MODE_PAGE_NEXT: {
    struct os1_display_mode_v1 head;
    if (nxs_mode_query(0U, &head) == 0) {
      uint32_t count = head.count > OS1_DISPLAY_MODE_MAX
                           ? OS1_DISPLAY_MODE_MAX
                           : head.count;
      uint32_t pages =
          (count + NXS_MODE_PAGE_SIZE - 1U) / NXS_MODE_PAGE_SIZE;
      if (pages > 0U && g_mode_page + 1U < pages)
        ++g_mode_page;
    }
    break;
  }
  case ACT_LOGICAL_AUTO:
  case ACT_LOGICAL_MANUAL: {
    struct os1_display_workspace_v1 state;
    int queried = (int)OS1_display_workspace_query(&state);
    if (queried != 0) {
      OS1_report_error("nxsettings.workspace_read", queried);
      break;
    }
    uint32_t mode = action == ACT_LOGICAL_AUTO ? OS1_DISPLAY_WORKSPACE_AUTO
                                              : OS1_DISPLAY_WORKSPACE_MANUAL;
    uint32_t width = mode == OS1_DISPLAY_WORKSPACE_MANUAL ?
                         state.base_width : 0U;
    uint32_t height = mode == OS1_DISPLAY_WORKSPACE_MANUAL ?
                          state.base_height : 0U;
    int changed = (int)OS1_display_workspace_set(
        mode, width, height, state.zoom_percent);
    if (changed != 0) OS1_report_error("nxsettings.workspace_mode", changed);
    g_logical_menu_open = 0;
    break;
  }
  case ACT_LOGICAL_MENU:
    g_logical_menu_open = !g_logical_menu_open;
    g_mode_menu_open = 0;
    g_scale_menu_open = 0;
    break;
  case ACT_LOGICAL_SELECT: {
    if (arg < 0 || arg >= (long)OS1_DISPLAY_MODE_MAX) break;
    struct os1_display_mode_v1 mode;
    struct os1_display_workspace_v1 state;
    int result = nxs_mode_query((uint32_t)arg, &mode);
    if (result == 0) result = (int)OS1_display_workspace_query(&state);
    if (result == 0)
      result = (int)OS1_display_workspace_set(OS1_DISPLAY_WORKSPACE_MANUAL,
                                             mode.width, mode.height,
                                             state.zoom_percent);
    if (result != 0) OS1_report_error("nxsettings.workspace_manual", result);
    g_logical_menu_open = 0;
    break;
  }
  case ACT_LOGICAL_PREV:
    if (g_logical_page > 0U) --g_logical_page;
    break;
  case ACT_LOGICAL_NEXT: {
    struct os1_display_mode_v1 head;
    if (nxs_mode_query(0U, &head) == 0 &&
        g_logical_page + 1U <
        (head.count + NXS_MODE_PAGE_SIZE - 1U) / NXS_MODE_PAGE_SIZE)
      ++g_logical_page;
    break;
  }
  case ACT_SCALE_MENU:
    g_scale_menu_open = !g_scale_menu_open;
    g_mode_menu_open = 0;
    g_logical_menu_open = 0;
    break;
  case ACT_SCALE_SELECT: {
    int percent = (int)arg;
    int status = OS1_display_set_zoom(percent);
    if (status == 0)
      g_zoom_pct = percent;
    else
      OS1_report_error("nxsettings.ui_scale", status);
    g_scale_menu_open = 0;
    break;
  }
  case ACT_STYLE:
    if (arg >= 0 && arg < NXS_NSTYLES) {
      g_cur_style = (int)arg;
      nxres_set_style(NXS_STYLES[arg]);
    }
    break;
  case ACT_THEME:
    if (arg >= 0 && arg < NXS_NTHEMES) {
      g_cur_theme = (int)arg;
      nxres_set_theme(NXS_THEMES[arg]);
      /* Refresh the settings palette after the theme change. */
      load_theme_colors(
          g_cur_theme == 0 ? 1 : 0); /* Registry order: 0=light, 1=dark. */
    }
    break;
  case ACT_BACKGROUND:
    if (arg >= 0 && arg < NXS_NBG) {
      g_cur_bg = (int)arg;
      nxres_set_background(NXS_BG[arg]);
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
  g_mode_menu_open = 0;
  g_scale_menu_open = 0;
  g_logical_menu_open = 0;
}

/* Size policy is expressed in logical pixels and constrained by the current
 * desktop. Normal desktop keeps its full UI; zoomed desktop favors recovery. */
static void nxs_target_size(int sw, int sh, int *ww, int *wh) {
  int w = (sw * 7) / 10, h = (sh * 7) / 10;
  if (w > 760) w = 760;
  if (h > 520) h = 520;
  if (w < MIN_WIN_W) w = MIN_WIN_W;
  if (h < MIN_WIN_H) h = MIN_WIN_H;
  if (sw > 8 && w > sw - 8) w = sw - 8;
  if (sh > 28 && h > sh - 28) h = sh - 28;
  *ww = w;
  *wh = h;
}

static int nxs_replace_framebuffer(int ww, int wh) {
  if (ww <= 0 || wh <= 0 || ww > NXS_GFX_MAX_DIM ||
      wh > NXS_GFX_MAX_DIM || (size_t)ww > SIZE_MAX / sizeof(uint32_t) /
                                        (size_t)wh)
    return -EINVAL;
  if (ww == g_ww && wh == g_wh) return 0;
  uint32_t *replacement = (uint32_t *)malloc((size_t)ww * (size_t)wh *
                                              sizeof(uint32_t));
  if (replacement == NULL) return -ENOMEM;
  uint32_t *old = g_fb;
  g_fb = replacement;
  g_ww = ww;
  g_wh = wh;
  free(old);
  return 0;
}

static void reinit_window(int ww, int wh) {
  int replaced = nxs_replace_framebuffer(ww, wh);
  if (replaced != 0) OS1_report_error("nxsettings.resize_buffer", replaced);
}

/* A zoom change modifies the logical desktop, not the GPU scanout. Refit the
 * already-open settings window so all recovery controls remain clickable. */
static void nxs_fit_display(void) {
  int sw = 0, sh = 0;
  if (OS1_display_get_size(&sw, &sh) != 0 || sw <= 0 || sh <= 0 ||
      (sw == g_display_w && sh == g_display_h)) return;
  g_display_w = sw;
  g_display_h = sh;
  int ww = 0, wh = 0;
  nxs_target_size(sw, sh, &ww, &wh);
  if (ww <= 0 || wh <= 0) return;
  if (ww != g_ww || wh != g_wh) {
    uint32_t *replacement = (uint32_t *)malloc(
        (size_t)ww * (size_t)wh * sizeof(uint32_t));
    if (replacement == NULL) {
      OS1_report_error("nxsettings.fit_alloc", -ENOMEM);
      return;
    }
    int resized = OS1_window_resize(g_win, ww, wh);
    if (resized != 0) {
      free(replacement);
      OS1_report_error("nxsettings.fit_resize", resized);
      return;
    }
    free(g_fb);
    g_fb = replacement;
    g_ww = ww;
    g_wh = wh;
  }
  /* Positioning is controlled by nxmoused's window-manager capability.
   * The compositor clamps this owner window on display geometry changes;
   * settings must not request manager authority just to expose zoom recovery. */
}

static int main_gui(void) {
  g_font = font_load("/fonts/Rewir-Light.off");

  int sw = 0, sh = 0;
  int display_status = OS1_display_get_size(&sw, &sh);
  if (display_status != 0) {
    OS1_report_error("nxsettings.display_size", display_status);
    return 1;
  }

  int ww = 0, wh = 0;
  nxs_target_size(sw, sh, &ww, &wh);
  if (ww <= 0 || wh <= 0) return 1;
  g_display_w = sw;
  g_display_h = sh;
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

  /* Self-register this pid as the live "srv.settings_pid" endpoint
   * (nxres.h's nxres_look_ping_targets) so an EXTERNAL style/theme/bg change
   * (`nxres` from a shell, or any other caller of SYS_SET_STYLE) reaches
   * this already-open window instead of only applying the next time
   * nxsettings is launched.  nxsettings is not init-supervised, so this is
   * a plain self-registration (no respawn hazard to guard against — see
   * init.c's register_service_pid for the singleton-service equivalent). */
  {
    char pidbuf[16];
    snprintf(pidbuf, sizeof(pidbuf), "%d", get_pid());
    OS1_registry_set("srv.settings_pid", pidbuf);
  }

  /* Load the current theme through the shared nxres_theme_is_light() getter.
   * The registry is updated by the common look/theme path after a successful
   * change. */
  int theme_light = nxres_theme_is_light();
  load_theme_colors(theme_light);
  /* Mirror the current theme in the local selection state. */
  g_cur_theme = theme_light ? 0 : 1;
  /* Mirror style/background too so the highlighted selection represents the
   * state that is actually active when the window opens. */
  {
    char buf[32] = {0};
    if (nxres_get_style(buf, sizeof(buf)) == 0)
      g_cur_style = nxres_name_to_id(buf, NXS_STYLES, NXS_NSTYLES);
    if (nxres_get_background(buf, sizeof(buf)) == 0)
      g_cur_bg = nxres_name_to_id(buf, NXS_BG, NXS_NBG);
  }

  for (;;) {
    nxs_fit_display();
    redraw();
    window_blit(g_win, 0, 0, g_ww, g_wh, g_fb);
    /* SCHED-STACK-ISO: init pumps the compositor via flush(); blit marks
     * damage — no nested compositor_render() here. */

    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_MOUSE && ev.mouse.button == MOUSE_BTN_LEFT &&
          ev.mouse.state == KEY_PRESSED) {
        handle_click(ev.mouse.x, ev.mouse.y);
      } else if (ev.type == INPUT_TYPE_RESIZE && ev.resize.w > 0 &&
                 ev.resize.h > 0) {
        reinit_window(ev.resize.w, ev.resize.h);
      } else if (ev.type == INPUT_TYPE_LOOK_CHANGED) {
        /* External style/theme/bg change (nxres_broadcast_look, nxres.h),
         * surfaced through this SAME input_poll_event() loop — not a second
         * try_recv() (that stole this window's mouse/keyboard events in an
         * earlier version of this feature; see nxres.h's header comment). */
        theme_light = nxres_theme_is_light();
        load_theme_colors(theme_light);
        g_cur_theme = theme_light ? 0 : 1;
        char buf[32] = {0};
        if (nxres_get_style(buf, sizeof(buf)) == 0)
          g_cur_style = nxres_name_to_id(buf, NXS_STYLES, NXS_NSTYLES);
        if (nxres_get_background(buf, sizeof(buf)) == 0)
          g_cur_bg = nxres_name_to_id(buf, NXS_BG, NXS_NBG);
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