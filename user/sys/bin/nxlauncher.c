/*
 * user/sys/bin/nxlauncher.c
 * NEXS launcher — a normal, resizable compositor window with a tile grid
 * sized to its OWN dimensions, not to the desktop.
 *
 * Unlike a classic pinned-at-bottom launcher, this one opens as a regular
 * window (with the native titlebar) in the centre of the desktop at half the
 * screen size.  The user drags/resizes it freely via the titlebar border;
 * the grid (cols x rows) is recomputed from the window's current w/h on every
 * resize event, so the layout adapts naturally.
 *
 * Tile geometry matches nxui's dock (TILE=40, TILE_RADIUS=6, TILE_GAP=12) so
 * the launcher and the dock read as one design family.
 *
 * App discovery (no hardcoded app list, no hardcoded folder list):
 *   The root view contains EXACTLY TWO folders:
 *     - "System" → contents of /sys/bin  (filtered)
 *     - "User"   → contents of /bin      (filtered)
 *   Each folder is scanned live from the filesystem on startup.  Every entry
 *   whose name ends in one of the filtered extensions (".wad", ".txt",
 *   ".png", ".cfg", ".dat", ".md", ".json") is SKIPPED; "." and ".." are
 *   ALWAYS skipped so the user never has to think about them — instead, a
 *   DRAWN back-arrow button at the top of the folder view returns to the
 *   root (the compositor gives us a ".." replacement we own).
 *
 *   The optional /sys/bin/nxlauncher.cfg can append extra entries or pin
 *   labels, but the root view itself never changes shape — there is no way
 *   to expose a top-level app that is not under one of the two folders.
 *
 * Window events handled:
 *   - INPUT_TYPE_MOUSE (left press): hit-test against the per-page slot
 *     tables; tile click → spawn_args(path); folder tile → enter that
 *     folder; back arrow → leave folder; arrow buttons → page left/right;
 *     dot click → jump to page.  AFTER a successful app spawn, the launcher
 *     auto-minimises itself (OS1_window_minimize) so the spawned app gets
 *     the focus and the launcher stays available in the dock — the user
 *     restores it with another dock click.
 *   - INPUT_TYPE_RESIZE (event-driven): the compositor reports the new
 *     logical size.  We realloc the ARGB buffer, recompute the layout, and
 *     force a redraw.
 *
 * Resize is also polled via OS1_display_info for the rare host-driven
 * resolution change.
 */
#include "nxexec.h"
#include "nxres.h" /* nxres_theme_is_light(), IPC_LOOK_PING_MAGIC (posix_types.h) — see palette below */
#include <font_lib.h>
#include <image.h>
#include <input.h>
#include <os1.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------- First-open size ------------------------ */
/* Half the desktop, centred.  The native titlebar lets the user drag and
 * resize freely afterwards. */
#define DEFAULT_W_RATIO 2
#define DEFAULT_H_RATIO 2

/* ------------------------- Tile geometry --------------------------- */
/* Same as nxui (TILE=40, TILE_RADIUS=6, TILE_GAP=12 → nxui.c:61-63). */
#define TILE 40
#define TILE_GAP_X 12
#define TILE_GAP_Y 12
#define TILE_RADIUS 6

#define LABEL_GAP 4
#define LABEL_MAX_W 56 /* tighter than the old 96 because tiles are smaller */

#define WIN_PAD 8 /* padding between the window edge and the grid */

/* ------------------------- Page indicator ------------------------- */
#define DOT_DIA 7
#define DOT_GAP 7
#define DOT_BOTTOM 10

/* ------------------------- Side arrows --------------------------- */
#define ARROW_W 22
#define ARROW_H 30
#define ARROW_GAP 8

/* ------------------------- Back arrow (folder view) -------------- */
#define BACK_W 64
#define BACK_H 26

/* ------------------------- App table cap ------------------------- */
#define MAX_APPS 128
#define NAME_MAX 32
#define PATH_MAX 64
#define CFG_MAX 4096

/* ------------------------- Categories ---------------------------- */
enum cat {
  CAT_USER = 0,
  CAT_SYSTEM,
  CAT_UTILITY,
  CAT_GAMES,
  CAT_OFFICE,
  CAT_OTHER,
  CAT_FOLDER,
};
#define CAT_COUNT 7

struct app_def {
  char path[PATH_MAX];
  char label[NAME_MAX];
  enum cat category;
};

/* ------------------------- Window state --------------------------- */
static uint32_t *g_fb;          /* ARGB buffer (g_ww x g_wh)                */
static int g_ww, g_wh;          /* current window size                      */
static int g_dsw, g_dsh;        /* last-seen desktop (for first-open only)  */
static int g_win = -1;          /* our window id                            */
static struct font_ctx *g_font; /* /fonts/Rewir-Light.off, may be NULL      */

/* ------------------------- App table ----------------------------- */
static struct app_def g_apps[MAX_APPS];
static int g_n_apps;

/* ------------------------- Folder view stack --------------------- */
#define VIEW_PAGES 0
#define VIEW_FOLDER 1
static int g_view;
static int g_view_top[MAX_APPS];

/* ------------------------- Paging state -------------------------- */
static int g_page;
static int g_pages;
static int g_pressed;

/* Per-page slot table (rebuilt each redraw, used for hit-testing). */
struct slot {
  int x, y, w, h;
  int kind; /* 0=app, 1=folder                                      */
  int idx;  /* index into g_apps[]                                   */
};
#define MAX_SLOTS_PER_PAGE 64
static struct slot g_slots[MAX_SLOTS_PER_PAGE];
static int g_slot_n;

/* ------------------------- Hit-test scratch ---------------------- */
static int g_arrow_l_x, g_arrow_l_y;
static int g_arrow_r_x, g_arrow_r_y;
static int g_dots_y, g_dots_x0;
static int g_back_x, g_back_y;

/* ------------------------- Layout scratch ------------------------ */
static int grid_origin_x, grid_origin_y;
static int grid_w, grid_h;
static int cols, rows;

/* ------------------------- Palette ------------------------------- */
static const uint32_t COL_TILE[CAT_COUNT] = {
    0xFF42A5F5u, /* CAT_USER     blue 400  */
    0xFF66BB6Au, /* CAT_SYSTEM   green 400 */
    0xFF8D6E63u, /* CAT_UTILITY  brown 400 */
    0xFFE53935u, /* CAT_GAMES    red 600   */
    0xFF1E88E5u, /* CAT_OFFICE   blue 600  */
    0xFF757575u, /* CAT_OTHER    grey 600  */
    0xFF26A69Au, /* CAT_FOLDER   teal 400  */
};
/* Window chrome (bg/label/dots/arrows) tracks theme.color (nxres.h); the
 * per-category tile colours above stay fixed across both themes — same
 * reasoning as nxui's launcher-tile green / nxbar's badge red. */
static uint32_t g_col_bg;
static uint32_t g_col_label;
static uint32_t g_col_label_shadow;
static uint32_t g_col_dot_active;
static uint32_t g_col_dot_inactive;
static uint32_t g_col_arrow;
static uint32_t g_col_arrow_pressed;
static uint32_t g_col_back;

static void nxlauncher_load_palette(int light) {
  if (light) {
    g_col_bg = 0xE8F5F5F7u;
    g_col_label = 0xFF1C1C1Eu;
    g_col_label_shadow = 0x80FFFFFFu;
    g_col_dot_active = 0xFF1C1C1Eu;
    g_col_dot_inactive = 0x661C1C1Eu;
    g_col_arrow = 0xCCD1D1D6u;
    g_col_arrow_pressed = 0xCCAEAEB2u;
    g_col_back = 0xCCD1D1D6u;
  } else {
    g_col_bg = 0xE8202028u; /* window background */
    g_col_label = 0xFFFFFFFFu;
    g_col_label_shadow = 0x80000000u;
    g_col_dot_active = 0xFFFFFFFFu;
    g_col_dot_inactive = 0x66FFFFFFu;
    g_col_arrow = 0xCC2A2A33u;
    g_col_arrow_pressed = 0xCC6B6B73u;
    g_col_back = 0xCC2A2A33u;
  }
}

/* ============================================================
 *                        Pixel helpers
 * ============================================================ */

static void fb_fill(uint32_t c) {
  int total = g_ww * g_wh;
  for (int i = 0; i < total; i++)
    g_fb[i] = c;
}

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

static void fb_circle(int cx, int cy, int r, uint32_t c) {
  int r2 = r * r;
  for (int j = -r; j <= r; j++) {
    for (int i = -r; i <= r; i++) {
      if (i * i + j * j <= r2) {
        int px = cx + i, py = cy + j;
        if (px >= 0 && px < g_ww && py >= 0 && py < g_wh)
          g_fb[py * g_ww + px] = c;
      }
    }
  }
}

/* ============================================================
 *                      Font rendering on g_fb
 * ============================================================ */

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

  // Disegno diretto pixel per pixel
  for (int gy = 0; gy < gi->height; gy++) {
    for (int gx = 0; gx < gi->width; gx++) {
      uint8_t alpha = bitmap[gy * gi->width + gx];

      // Se il pixel del font è visibile, lo disegniamo
      // Se vuoi gestire l'anti-aliasing, qui puoi fare un alpha blending
      if (alpha > 64) {
        int px = start_x + gx;
        int py = start_y + gy;
        if (px >= 0 && px < g_ww && py >= 0 && py < g_wh) {
          g_fb[py * g_ww + px] = color;
        }
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

static void buf_truncate_to_width(char *s, int max_w) {
  if (!g_font || !s || max_w <= 0)
    return;
  if (buf_text_width(s) <= max_w)
    return;
  int dots_w = buf_text_width("...");
  int target = max_w - dots_w;
  if (target < 0)
    target = 0;
  int len = (int)strlen(s);
  while (len > 0 && buf_text_width(s) > target)
    s[--len] = '\0';
  if (len + 3 < (int)sizeof(((struct app_def *)0)->label)) {
    s[len++] = '.';
    s[len++] = '.';
    s[len++] = '.';
    s[len] = '\0';
  }
}

/* ============================================================
 *                    App table management
 * ============================================================ */

static int apps_add(const char *path, const char *label, enum cat cat) {
  if (g_n_apps >= MAX_APPS)
    return 0;
  if (!path || !*path)
    return 0;
  struct app_def *a = &g_apps[g_n_apps];
  int n;
  n = 0;
  while (path[n] && n < PATH_MAX - 1) {
    a->path[n] = path[n];
    n++;
  }
  a->path[n] = '\0';
  if (!label || !*label) {
    const char *slash = strrchr(path, '/');
    label = slash ? slash + 1 : path;
  }
  n = 0;
  while (label[n] && n < NAME_MAX - 1) {
    a->label[n] = label[n];
    n++;
  }
  a->label[n] = '\0';
  a->category = cat;
  g_n_apps++;
  return 1;
}

struct folder {
  int first_child;
  int child_count;
};
#define MAX_FOLDERS 16
static struct folder g_folders[MAX_FOLDERS];
static int g_n_folders;

static int apps_add_folder(const char *name) {
  if (g_n_folders >= MAX_FOLDERS)
    return -1;
  if (g_n_apps >= MAX_APPS)
    return -1;
  struct app_def *a = &g_apps[g_n_apps];
  a->path[0] = '\0';
  int n = 0;
  while (name[n] && n < NAME_MAX - 1) {
    a->label[n] = name[n];
    n++;
  }
  a->label[n] = '\0';
  a->category = CAT_FOLDER;
  int id = g_n_apps++;
  g_folders[g_n_folders].first_child = g_n_apps;
  g_folders[g_n_folders].child_count = 0;
  g_n_folders++;
  return id;
}

static void apps_end_folder(void) {
  if (g_n_folders == 0)
    return;
  g_folders[g_n_folders - 1].child_count =
      g_n_apps - g_folders[g_n_folders - 1].first_child;
}

static int apps_folder_first_child(int folder_idx) {
  for (int i = 0; i < g_n_folders; i++) {
    if (g_folders[i].first_child > folder_idx)
      break;
    if (g_folders[i].first_child + g_folders[i].child_count > folder_idx)
      return g_folders[i].first_child;
  }
  return -1;
}

static int apps_folder_count(int folder_idx) {
  for (int i = 0; i < g_n_folders; i++) {
    if (g_folders[i].first_child > folder_idx)
      break;
    if (g_folders[i].first_child + g_folders[i].child_count > folder_idx)
      return g_folders[i].child_count;
  }
  return 0;
}

/* ============================================================
 *                       App discovery
 * ============================================================ */

static enum cat parse_cat(const char *s) {
  if (!s || !*s)
    return CAT_USER;
  if (!strcasecmp(s, "system"))
    return CAT_SYSTEM;
  if (!strcasecmp(s, "user"))
    return CAT_USER;
  if (!strcasecmp(s, "utility"))
    return CAT_UTILITY;
  if (!strcasecmp(s, "games"))
    return CAT_GAMES;
  if (!strcasecmp(s, "office"))
    return CAT_OFFICE;
  if (!strcasecmp(s, "other"))
    return CAT_OTHER;
  return CAT_USER;
}

static void load_cfg(void) {
  int sz = file_read("/sys/bin/nxlauncher.cfg", (char *)0, 0, 0);
  if (sz <= 0 || sz > CFG_MAX)
    return;
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf)
    return;
  int n = file_read("/sys/bin/nxlauncher.cfg", buf, sz, 0);
  if (n <= 0) {
    free(buf);
    return;
  }
  buf[n] = '\0';

  int in_folder = 0;
  char *p = buf;
  while (p < buf + n) {
    char *eol = p;
    while (eol < buf + n && *eol != '\n')
      eol++;
    if (eol < buf + n)
      *eol = '\0';

    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0' || *p == '#') {
      p = eol + 1;
      continue;
    }

    if (strncmp(p, "folder:", 7) == 0) {
      if (in_folder)
        apps_end_folder();
      if (apps_add_folder(p + 7) < 0) {
        free(buf);
        return;
      }
      in_folder = 1;
    } else if (strcmp(p, "end") == 0) {
      if (in_folder) {
        apps_end_folder();
        in_folder = 0;
      }
    } else {
      char *path = p;
      char *label = (char *)0;
      char *cat = (char *)0;
      for (char *q = p; *q; q++) {
        if (*q == ':') {
          *q = '\0';
          if (!label)
            label = q + 1;
          else if (!cat) {
            cat = q + 1;
            break;
          }
        }
      }
      apps_add(path, label, parse_cat(cat));
    }

    p = eol + 1;
  }
  if (in_folder)
    apps_end_folder();
  free(buf);
}

/* Filter: skip file names ending in any of these extensions.  .wad is the
 * one the user asked for; the others are defensive (the /bin directory may
 * hold all sorts of artefacts — config, docs, images — that we never want
 * to offer as a launchable app). */
// Lista delle estensioni (devono avere il punto)
static const char *const filtered_extensions[] = {
    ".wad", ".txt", ".cfg", ".dat", ".md", ".json", ".old", ".dsg", NULL};

// Lista dei nomi di file completi (esatti)
/* Hidden from the tile grid: the boot/supervisor services, the launcher and
 * dock themselves, and nxexec (the execution service — it is the thing that
 * LAUNCHES tiles, not a tile; clicking it with no argument does nothing). */
static const char *const filtered_files[] = {"init",
                                             "nxntfy_srv",
                                             "nxui",
                                             "nxbar"
                                             "nxlauncher",
                                             "nxexec",
                                             "background",
                                             ".",
                                             "..",
                                             NULL};

static int has_filtered_ext(const char *name) {
  if (os1_image_path_has_known_ext(name))
    return 1;

  // 1. Controllo estensioni
  const char *dot = strrchr(name, '.');
  if (dot) {
    for (int i = 0; filtered_extensions[i]; i++) {
      if (strcasecmp(dot, filtered_extensions[i]) == 0)
        return 1;
    }
  }

  // 2. Controllo nomi file completi
  for (int i = 0; filtered_files[i]; i++) {
    if (strcasecmp(name, filtered_files[i]) == 0)
      return 1;
  }

  return 0;
}

static void scan_bin(void) {
  char buf[1024];

  /* System applications (/sys/bin) */
  int n = list_dir("/sys/bin", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    char *save = (char *)0;
    for (char *tok = strtok_r(buf, " \t", &save); tok;
         tok = strtok_r((char *)0, " \t", &save)) {
      if (!*tok)
        continue;
      if (has_filtered_ext(tok))
        continue;

      char path[PATH_MAX];
      int k = 0;
      const char *prefix = "/sys/bin/";
      while (prefix[k] && k < PATH_MAX - 1)
        path[k] = prefix[k], k++;

      int j = 0;
      while (tok[j] && k < PATH_MAX - 1)
        path[k++] = tok[j++];

      path[k] = '\0';
      apps_add(path, (const char *)0, CAT_SYSTEM);
    }
  }

  /* User applications (/bin) */
  n = list_dir("/bin", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    char *save = (char *)0;
    for (char *tok = strtok_r(buf, " \t", &save); tok;
         tok = strtok_r((char *)0, " \t", &save)) {
      if (!*tok)
        continue;
      if (has_filtered_ext(tok))
        continue;

      char path[PATH_MAX];
      int k = 0;
      const char *prefix = "/bin/";
      while (prefix[k] && k < PATH_MAX - 1)
        path[k] = prefix[k], k++;

      int j = 0;
      while (tok[j] && k < PATH_MAX - 1)
        path[k++] = tok[j++];

      path[k] = '\0';
      apps_add(path, (const char *)0, CAT_USER);
    }
  }
}

/* ============================================================
 *                         Layout
 * ============================================================ */

static void recompute_layout(void) {
  int usable_w = g_ww - 2 * WIN_PAD;
  int usable_h = g_wh - 2 * WIN_PAD;

  int dot_row_h = DOT_DIA + DOT_BOTTOM + 4;
  int top_extra = 0;
  if (g_view == VIEW_FOLDER)
    top_extra = BACK_H + 10;

  grid_origin_x = WIN_PAD;
  grid_origin_y = WIN_PAD + top_extra;
  grid_w = usable_w;
  grid_h = usable_h - top_extra - dot_row_h;
  if (grid_h < 0)
    grid_h = 0;

  int tile_pitch_x = TILE + TILE_GAP_X;
  int tile_pitch_y = TILE + LABEL_GAP + LABEL_MAX_W + TILE_GAP_Y;
  cols = (grid_w + TILE_GAP_X) / tile_pitch_x;
  if (cols < 1)
    cols = 1;
  rows = (grid_h + TILE_GAP_Y) / tile_pitch_y;
  if (rows < 1)
    rows = 1;
}

static int current_view_first(void) {
  if (g_view == VIEW_PAGES)
    return 0;
  if (g_view == VIEW_FOLDER)
    return (g_view_top[0] >= 0) ? g_view_top[0] : 0;
  return 0;
}

static int current_view_count(void) {
  if (g_view == VIEW_PAGES)
    return g_n_apps;
  if (g_view == VIEW_FOLDER && g_view_top[0] >= 0)
    return apps_folder_count(g_view_top[0] - 1);
  return g_n_apps;
}

static void populate_slots(void) {
  int first = current_view_first();
  int count = current_view_count();
  if (first < 0)
    first = 0;
  if (count < 0)
    count = 0;

  int per_page = cols * rows;
  if (per_page < 1)
    per_page = 1;

  int effective_cols = cols;
  if (count > per_page && cols > 1)
    effective_cols = cols - 1;
  per_page = effective_cols * rows;
  if (per_page < 1)
    per_page = 1;

  int pages = (count + per_page - 1) / per_page;
  if (pages < 1)
    pages = 1;
  if (g_page >= pages)
    g_page = pages - 1;
  if (g_page < 0)
    g_page = 0;
  g_pages = pages;

  int page_start = first + g_page * per_page;
  int page_count = count - g_page * per_page;
  if (page_count < 0)
    page_count = 0;
  if (page_count > per_page)
    page_count = per_page;

  int grid_pixel_w = effective_cols * TILE + (effective_cols - 1) * TILE_GAP_X;
  int start_x = (g_ww - grid_pixel_w) / 2;
  int arrow_strip = 0;
  if (count > per_page)
    arrow_strip = ARROW_W + ARROW_GAP;
  int tile_x0 = start_x + arrow_strip;

  int row_pixel_h = TILE + LABEL_GAP + LABEL_MAX_W + TILE_GAP_Y;
  int grid_pixel_h = rows * row_pixel_h - TILE_GAP_Y;
  int start_y = grid_origin_y + ((grid_h - grid_pixel_h) / 2);
  if (start_y < grid_origin_y)
    start_y = grid_origin_y;

  g_slot_n = 0;
  for (int i = 0; i < page_count; i++) {
    int app_idx = page_start + i;
    int c = i % effective_cols;
    int r = i / effective_cols;
    int tx = tile_x0 + c * (TILE + TILE_GAP_X);
    int ty = start_y + r * row_pixel_h;
    if (g_slot_n < MAX_SLOTS_PER_PAGE) {
      g_slots[g_slot_n].x = tx;
      g_slots[g_slot_n].y = ty;
      g_slots[g_slot_n].w = TILE;
      g_slots[g_slot_n].h = TILE + LABEL_GAP + LABEL_MAX_W;
      g_slots[g_slot_n].kind = (g_apps[app_idx].category == CAT_FOLDER) ? 1 : 0;
      g_slots[g_slot_n].idx = app_idx;
      g_slot_n++;
    }
  }

  if (count > per_page) {
    int mid_y = start_y + (rows / 2) * row_pixel_h + (TILE - ARROW_H) / 2;
    g_arrow_l_x = start_x + ARROW_W / 2;
    g_arrow_l_y = mid_y + ARROW_H / 2;
    g_arrow_r_x = g_ww - start_x - ARROW_W / 2;
    g_arrow_r_y = mid_y + ARROW_H / 2;
  } else {
    g_arrow_l_x = g_arrow_r_x = -1;
  }

  g_dots_y = grid_origin_y + grid_h + DOT_BOTTOM + DOT_DIA / 2;
  if (count > per_page) {
    int dots_w = g_pages * DOT_DIA + (g_pages - 1) * DOT_GAP;
    g_dots_x0 = (g_ww - dots_w) / 2 + DOT_DIA / 2;
  } else {
    g_dots_x0 = -1;
  }

  if (g_view == VIEW_FOLDER) {
    g_back_x = (g_ww - BACK_W) / 2;
    g_back_y = WIN_PAD;
  } else {
    g_back_x = g_back_y = -1;
  }
}

/* ============================================================
 *                         Redraw
 * ============================================================ */

static void draw_label(int x, int y, const char *label) {
  char tmp[NAME_MAX + 4];
  int n = 0;
  while (label[n] && n < (int)sizeof(tmp) - 4) {
    tmp[n] = label[n];
    n++;
  }
  tmp[n] = '\0';
  buf_truncate_to_width(tmp, LABEL_MAX_W);
  int tw = buf_text_width(tmp);
  int lx = x + (TILE - tw) / 2;
  if (lx < 0)
    lx = 0;
  int ly = y + TILE + LABEL_GAP;
  if (g_font)
    buf_draw_text(lx + 1, ly + 1, tmp, g_col_label_shadow);
  buf_draw_text(lx, ly, tmp, g_col_label);
}

static void draw_folder_tile(int x, int y) {
  fb_rrect(x, y, TILE, TILE, TILE_RADIUS, COL_TILE[CAT_FOLDER]);
  int tab_w = 18, tab_h = 6;
  fb_rrect(x + 5, y + 6, tab_w, tab_h, 2, 0xCC000000u);
  int mid_y = y + TILE / 2;
  for (int i = 7; i < TILE - 7; i++)
    if (x + i >= 0 && x + i < g_ww && mid_y >= 0 && mid_y < g_wh)
      g_fb[mid_y * g_ww + (x + i)] = 0x33000000u;
}

static void redraw(void) {
  fb_fill(g_col_bg);

  if (g_view == VIEW_FOLDER && g_back_x >= 0) {
    fb_rrect(g_back_x, g_back_y, BACK_W, BACK_H, 6, g_col_back);
    if (g_font) {
      const char *t = "< Back";
      int tw = buf_text_width(t);
      buf_draw_text(g_back_x + (BACK_W - tw) / 2, g_back_y + (BACK_H - 16) / 2,
                    t, g_col_label);
    }
  }

  for (int i = 0; i < g_slot_n; i++) {
    int idx = g_slots[i].idx;
    const struct app_def *a = &g_apps[idx];
    if (g_slots[i].kind == 1) {
      draw_folder_tile(g_slots[i].x, g_slots[i].y);
    } else {
      fb_rrect(g_slots[i].x, g_slots[i].y, TILE, TILE, TILE_RADIUS,
               COL_TILE[a->category]);
    }
    draw_label(g_slots[i].x, g_slots[i].y, a->label);
  }

  if (g_arrow_l_x >= 0) {
    uint32_t lc = (g_pressed == 1) ? g_col_arrow_pressed : g_col_arrow;
    uint32_t rc = (g_pressed == 2) ? g_col_arrow_pressed : g_col_arrow;
    int ax_l = g_arrow_l_x - ARROW_W / 2;
    int ax_r = g_arrow_r_x - ARROW_W / 2;
    int ay = g_arrow_l_y - ARROW_H / 2;
    fb_rrect(ax_l, ay, ARROW_W, ARROW_H, 5, lc);
    fb_rrect(ax_r, ay, ARROW_W, ARROW_H, 5, rc);
    if (g_pressed == 1)
      ax_l += 1;
    if (g_pressed == 2)
      ax_r += 1;
    fb_rrect(ax_l + ARROW_W / 2 - 4, ay + ARROW_H / 2 - 3, 2, 6, 1, g_col_label);
    fb_rrect(ax_l + ARROW_W / 2 - 2, ay + ARROW_H / 2 - 1, 2, 2, 1, g_col_label);
    fb_rrect(ax_l + ARROW_W / 2 - 4, ay + ARROW_H / 2 + 1, 2, 2, 1, g_col_label);
    fb_rrect(ax_r + ARROW_W / 2 + 2, ay + ARROW_H / 2 - 3, 2, 6, 1, g_col_label);
    fb_rrect(ax_r + ARROW_W / 2, ay + ARROW_H / 2 - 1, 2, 2, 1, g_col_label);
    fb_rrect(ax_r + ARROW_W / 2, ay + ARROW_H / 2 + 1, 2, 2, 1, g_col_label);
  }

  if (g_dots_x0 >= 0) {
    for (int i = 0; i < g_pages; i++) {
      int cx = g_dots_x0 + i * (DOT_DIA + DOT_GAP);
      uint32_t c = (i == g_page) ? g_col_dot_active : g_col_dot_inactive;
      fb_circle(cx, g_dots_y, DOT_DIA / 2, c);
    }
  }
  compositor_render();
}

/* ============================================================
 *                       Click handling
 * ============================================================ */

static int hit_arrow(int x, int y) {
  if (g_arrow_l_x < 0)
    return 0;
  int ax_l = g_arrow_l_x - ARROW_W / 2;
  int ax_r = g_arrow_r_x - ARROW_W / 2;
  int ay = g_arrow_l_y - ARROW_H / 2;
  if (x >= ax_l && x < ax_l + ARROW_W && y >= ay && y < ay + ARROW_H)
    return 1;
  if (x >= ax_r && x < ax_r + ARROW_W && y >= ay && y < ay + ARROW_H)
    return 2;
  return 0;
}

static int hit_dot(int x, int y) {
  if (g_dots_x0 < 0)
    return -1;
  for (int i = 0; i < g_pages; i++) {
    int cx = g_dots_x0 + i * (DOT_DIA + DOT_GAP);
    int dx = x - cx;
    int dy = y - g_dots_y;
    if (dx * dx + dy * dy <= (DOT_DIA / 2) * (DOT_DIA / 2))
      return i;
  }
  return -1;
}

static int hit_slot(int x, int y) {
  for (int i = 0; i < g_slot_n; i++) {
    if (x >= g_slots[i].x && x < g_slots[i].x + g_slots[i].w &&
        y >= g_slots[i].y && y < g_slots[i].y + TILE)
      return i;
  }
  return -1;
}

static int hit_back(int x, int y) {
  if (g_back_x < 0)
    return 0;
  return (x >= g_back_x && x < g_back_x + BACK_W && y >= g_back_y &&
          y < g_back_y + BACK_H);
}

static void enter_folder(int folder_idx) {
  int first = apps_folder_first_child(folder_idx);
  if (first < 0)
    return;
  g_view = VIEW_FOLDER;
  g_view_top[0] = first;
  g_page = 0;
}

static void leave_folder(void) {
  g_view = VIEW_PAGES;
  g_view_top[0] = -1;
  g_page = 0;
}

static void handle_click(int mx, int my) {
  if (hit_back(mx, my)) {
    leave_folder();
    return;
  }
  int a = hit_arrow(mx, my);
  if (a == 1) {
    g_pressed = 1;
    g_page = (g_page - 1 + g_pages) % g_pages;
    return;
  }
  if (a == 2) {
    g_pressed = 2;
    g_page = (g_page + 1) % g_pages;
    return;
  }
  int d = hit_dot(mx, my);
  if (d >= 0 && d != g_page) {
    g_page = d;
    return;
  }
  int s = hit_slot(mx, my);
  if (s >= 0) {
    int idx = g_slots[s].idx;
    if (g_apps[idx].category == CAT_FOLDER) {
      enter_folder(idx);
    } else if (g_apps[idx].path[0]) {
      /* HOSTED launch (#193, nxexec model): every tile runs under a fresh
       * host shell spawned DETACHED (the launcher is never anyone's ctty).
       * The host decides at runtime: windowed app -> host vanishes unseen;
       * terminal program -> a shell window appears hosting its output
       * ("i processi terminale ottengono una shell come padre"). */
      int pid = nxexec_spawn_hosted(g_apps[idx].path);
      if (pid <= 0) {
        /* g_win is already up and actively self-rendering at this point in
         * the session (unlike main()'s startup path) — printf() here would
         * draw text straight into g_fb via window_text_write, the same
         * buffer redraw()/window_blit() use (verified: both ultimately hit
         * compositor.c's win->buffer) — so this goes through the existing
         * notification popup (OS1_notify_warn, lib.c) instead. */
        OS1_notify_warn("nxlauncher", g_apps[idx].path);
      } else {
        /* Auto-background: with the app launched, the launcher's job is done
         * for this turn.  Send ourselves to background so the spawned app
         * gets the focus.  The user can bring the launcher back from the
         * dock (its green tile is always at position 0). */
        OS1_window_minimize(g_win);
      }
    }
  }
}

/* ============================================================
 *                  Window (re)initialisation
 * ============================================================ */

static void reinit_window(int ww, int wh) {
  if (ww <= 0)
    ww = 400;
  if (wh <= 0)
    wh = 300;

  if (g_win >= 0)
    OS1_window_resize(g_win, ww, wh);

  uint32_t *nb = (uint32_t *)malloc((size_t)ww * wh * 4);
  if (nb) {
    free(g_fb);
    g_fb = nb;
  }
  g_ww = ww;
  g_wh = wh;
  recompute_layout();
  populate_slots();
  compositor_render();
}

int main(void) {
  g_font = font_load("/fonts/Rewir-Light.off");
  nxlauncher_load_palette(nxres_theme_is_light());

  scan_bin();
  load_cfg();

  g_view = VIEW_PAGES;
  g_view_top[0] = -1;
  g_page = 0;
  g_pages = 1;

  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;
  g_dsw = sw;
  g_dsh = sh;

  int ww = sw / DEFAULT_W_RATIO;
  int wh = sh / DEFAULT_H_RATIO;
  int wx = (sw - ww) / 2;
  int wy = (sh - wh) / 2;
  if (ww < 320)
    ww = 320;
  if (wh < 240)
    wh = 240;

  g_fb = (uint32_t *)malloc((size_t)ww * wh * 4);
  if (!g_fb)
    return 1;

  g_win = create_window(wx, wy, ww, wh, "nxlauncher");
  OS1_window_minimize(g_win);
  if (g_win < 0)
    return 1;
  /* No set_window_flags() — we are a NORMAL window, not top_most.  The
   * compositor draws the native titlebar; the user can drag/resize freely. */

  reinit_window(ww, wh);
  for (;;) {
    long d = OS1_display_info();
    int cw = (int)((d >> 16) & 0xFFFF), ch = (int)(d & 0xFFFF);
    if (cw > 0 && ch > 0 && (cw != g_dsw || ch != g_dsh)) {
      g_dsw = cw;
      g_dsh = ch;
    }

    populate_slots();
    redraw();
    window_blit(g_win, 0, 0, g_ww, g_wh, g_fb);

    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_MOUSE && ev.mouse.button == MOUSE_BTN_LEFT &&
          ev.mouse.state == KEY_PRESSED) {
        handle_click(ev.mouse.x, ev.mouse.y);
      } else if (ev.type == INPUT_TYPE_MOUSE &&
                 ev.mouse.button == MOUSE_BTN_LEFT &&
                 ev.mouse.state == KEY_RELEASED && g_pressed) {
        g_pressed = 0;
      } else if (ev.type == INPUT_TYPE_RESIZE) {
        int nw = ev.resize.w;
        int nh = ev.resize.h;
        if (nw > 0 && nh > 0)
          reinit_window(nw, nh);
      } else if (ev.type == INPUT_TYPE_LOOK_CHANGED) {
        /* External style/theme/bg change (nxres_broadcast_look, nxres.h),
         * surfaced through this SAME input_poll_event() loop — see nxres.h's
         * header comment for why a second try_recv() loop is wrong here. */
        nxlauncher_load_palette(nxres_theme_is_light());
      }
    }

    g_pressed = 0;
    OS1_sleep(50);
  }
  return 0;
}
