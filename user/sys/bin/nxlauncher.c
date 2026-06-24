/*
 * user/sys/bin/nxlauncher.c
 * NEXS launcher — fullscreen tile grid, always-on-bottom in z-order, never
 * the keyboard focus after a click.
 *
 * Model (LAUNCHER-01):
 *   The launcher is a SINGLE compositor window that covers the full desktop.
 *   It is created with set_window_flags(win, 1) — top_most (no titlebar, no
 *   shadows, sits in front of the desktop background) but NOT passive (we
 *   need to receive mouse clicks to drive tile selection).  After every
 *   click the launcher hands keyboard focus back to g_last_focus (the
 *   process that owned the keyboard before we did), so it never holds the
 *   caret and never steals keystrokes from the active app — the same
 *   pattern /sys/bin/nxui already uses in dock_reinit (nxui.c:444-445).
 *
 * Layout:
 *   - The top NOTIFY_BAR_H px are RESERVED for the future notification bar
 *     and are kept transparent (the compositor paints the desktop bg there).
 *   - The bottom DOCK_H px are the dock owned by /sys/bin/nxui.  We do not
 *     draw tiles there — DOCK_H matches nxui.c:60.
 *   - In between sits a grid: cols × rows tiles per page, with a row of
 *     dots (Android page indicator) and a back-arrow (for folder views)
 *     anchored to the bottom of the grid.
 *
 * App discovery (LAUNCHER-02):
 *   1. Hardcoded system array g_sys[] (always shown).
 *   2. Auto-scan of /bin/ via list_dir() — anything ending in .elf (or with
 *      no suffix) is treated as a USER-category app.
 *   3. Optional /sys/bin/nxlauncher.cfg (path:label:cat per line, plus
 *      folder:Name blocks) merged in last so the user can pin favourites
 *      and override labels.  Missing file ⇒ level 3 is empty.
 *
 * Input:
 *   - input_poll_event delivers mouse + resize events directly to the
 *     launcher's window (it's top_most; mouse events go to whoever the
 *     compositor hit-tests as the topmost under the cursor, see
 *     kernel/graphics/compositor.c:1162).
 *   - Click coords are RELATIVE to the launcher's window, which is (0,0,sw,sh),
 *     so they equal absolute desktop coords.  Same trick nxui uses.
 *
 * Resize:
 *   - Polled (OS1_display_info) AND event-driven (INPUT_TYPE_RESIZE), same
 *     as nxui (nxui.c:400-403, 451-457).  On change: free+realloc the ARGB
 *     buffer, recompute the grid, force a redraw.
 *
 * Known limits (intentional, MVP):
 *   - No real icons: tiles are filled with a category colour.  The label
 *     below is the only identifier.  An icon decoder path (/icons/<basename>
 *     .png) is a natural follow-up — stb_image is already in lib.c.
 *   - No gesture swipe — paging is dot/frecce only (matches the "no gesture"
 *     requirement).
 *   - Folders: max depth 2.  A folder inside a folder collapses the
 *     parent and replaces the visible page.  Good enough for an MVP.
 *   - Resize reallocates the full ARGB buffer; a 1024x768 desktop is
 *     ~3 MB.  Acceptable; a future optimisation is a small dirty-rect
 *     blit.
 */
#include <font_lib.h>
#include <graphics.h>
#include <input.h>
#include <os1.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------- Screen zones ------------------------- */
/* Reserved at the top for the future notification bar. */
#define NOTIFY_BAR_H 32
/* Matches nxui's dock height (nxui.c:60).  Update both if the dock moves. */
#define DOCK_H 56

/* ------------------------- Tile geometry ------------------------- */
#define TILE 80
#define TILE_GAP_X 18
#define TILE_GAP_Y 22
#define TILE_RADIUS 14
#define LABEL_GAP 6
#define LABEL_MAX_W 96 /* truncate the label past this width */

/* ------------------------- Page indicator ------------------------ */
#define DOT_DIA 8
#define DOT_GAP 8
#define DOT_BOTTOM 16 /* distance from the bottom of the grid area */

/* ------------------------- Side arrows --------------------------- */
#define ARROW_W 28
#define ARROW_H 40
#define ARROW_GAP 12

/* ------------------------- Back arrow (folder view) -------------- */
#define BACK_W 72
#define BACK_H 32

/* ------------------------- App table cap ------------------------- */
#define MAX_APPS 128
#define NAME_MAX 32
#define PATH_MAX 64
#define CFG_MAX 4096 /* bytes we'll read from the config file   */

/* ------------------------- Categories ---------------------------- */
enum cat {
  CAT_SYSTEM = 0,
  CAT_USER,
  CAT_UTILITY,
  CAT_GAMES,
  CAT_OFFICE,
  CAT_OTHER,
  CAT_FOLDER, /* not a colour per se — marks a folder tile       */
};
#define CAT_COUNT 7

struct app_def {
  char path[PATH_MAX];
  char label[NAME_MAX];
  enum cat category;
};

/* Per-window framebuffer + layout state. */
static uint32_t *g_fb;          /* ARGB pixel buffer (g_sw × g_sh)        */
static int g_sw, g_sh;          /* current desktop size                   */
static int g_dsw, g_dsh;        /* last-seen desktop (for resize detect)  */
static int g_win = -1;          /* launcher's own window id               */
static struct font_ctx *g_font; /* /fonts/Rewir-Light.off, may be NULL    */
static unsigned g_sig;          /* signature: skip blit when unchanged    */

/* Focus bookkeeping — mirrors nxui's g_last_focus.  After every click the
 * launcher hands keyboard focus back to this pid so the next keystroke
 * reaches the app the user was actually working in. */
static int g_last_focus;

/* ------------------------- App table ----------------------------- */
static struct app_def g_apps[MAX_APPS];
static int g_n_apps;

/* ------------------------- Folder view stack --------------------- */
#define VIEW_PAGES 0  /* root: page through the top-level app list    */
#define VIEW_FOLDER 1 /* inside a folder: show the folder's children   */
static int g_view;    /* current view                              */
static int g_view_top[MAX_APPS]; /* parent-app index for nested folders (none
                                  * for now — depth collapses — but kept for
                                  * future use)                              */

/* ------------------------- Paging state -------------------------- */
static int g_page;
static int g_pages;
static int g_pressed; /* 0=none, 1=arrow-left, 2=arrow-right (visual)  */

/* Per-page slot tables (rebuilt each redraw, used for hit-testing).
 * Each slot is one tile on the current page.  We store (x,y,w,h,kind,idx)
 * where idx is the index into g_apps[].  kind is 0=app, 1=folder, 2=back. */
struct slot {
  int x, y, w, h;
  int kind; /* 0 = app, 1 = folder, 2 = back arrow                  */
  int idx;  /* index into g_apps[]                                  */
};
#define MAX_SLOTS_PER_PAGE 64
static struct slot g_slots[MAX_SLOTS_PER_PAGE];
static int g_slot_n;

/* ------------------------- Hit-test scratch ---------------------- */
static int g_arrow_l_x, g_arrow_l_y;
static int g_arrow_r_x, g_arrow_r_y;
static int g_dots_y;
static int g_dots_x0; /* leftmost dot x for centring            */
static int g_back_x, g_back_y;

/* ------------------------- Palette ------------------------------- */
/* All ARGB.  Tile fill is solid; dots and labels ride alpha. */
static const uint32_t COL_TILE[CAT_COUNT] = {
    0xFF3F51B5u, /* CAT_SYSTEM   indigo 500                          */
    0xFF43A047u, /* CAT_USER     green 600                           */
    0xFF8D6E63u, /* CAT_UTILITY  brown 400                           */
    0xFFE53935u, /* CAT_GAMES    red 600                             */
    0xFF1E88E5u, /* CAT_OFFICE   blue 600                            */
    0xFF757575u, /* CAT_OTHER    grey 600                            */
    0xFF26A69Au, /* CAT_FOLDER   teal 400                            */
};
#define COL_LABEL 0xFFFFFFFFu
#define COL_LABEL_SHADOW 0x80000000u
#define COL_DOT_ACTIVE 0xFFFFFFFFu
#define COL_DOT_INACTIVE 0x66FFFFFFu
#define COL_ARROW 0xCC1C1C24u
#define COL_ARROW_PRESSED 0xCC6B6B73u
#define COL_BACK 0xCC1C1C24u

/* ------------------------- Hardcoded system apps ----------------- */
/* The list is intentionally small: only things every NeXs user needs.
 * Order in this array = order on the first page.  The user can reorder
 * / add via /sys/bin/nxlauncher.cfg. */
static const struct {
  const char *path;
  const char *label;
  enum cat cat;
} g_sys[] = {
    {"/sys/bin/nxshell", "NXShell", CAT_SYSTEM},
    {"/sys/bin/nxproc", "Processes", CAT_SYSTEM},
    {"/sys/bin/nxres", "Display", CAT_SYSTEM},
    {"/sys/bin/fontman", "Fonts", CAT_SYSTEM},
    {"/sys/bin/top", "Top", CAT_SYSTEM},
    {"/sys/bin/nxperm", "Permissions", CAT_SYSTEM},
    {"/sys/bin/nxmemstat", "Memstat", CAT_SYSTEM},
    {"/sys/bin/nxinfo", "Info", CAT_SYSTEM},
};
#define G_SYS_N (int)(sizeof(g_sys) / sizeof(g_sys[0]))

/* ============================================================
 *                        Pixel helpers
 * ============================================================ */

static void fb_fill(uint32_t c) {
  int total = g_sw * g_sh;
  for (int i = 0; i < total; i++)
    g_fb[i] = c;
}

/* Filled rounded rectangle into the launcher buffer (corners clipped to a
 * quarter-circle of radius r).  Out-of-bounds pixels are silently skipped
 * (the caller passes screen-space coordinates). */
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
      if (px >= 0 && px < g_sw && py >= 0 && py < g_sh)
        g_fb[py * g_sw + px] = c;
    }
  }
}

/* Filled circle into the launcher buffer. */
static void fb_circle(int cx, int cy, int r, uint32_t c) {
  int r2 = r * r;
  for (int j = -r; j <= r; j++) {
    for (int i = -r; i <= r; i++) {
      if (i * i + j * j <= r2) {
        int px = cx + i, py = cy + j;
        if (px >= 0 && px < g_sw && py >= 0 && py < g_sh)
          g_fb[py * g_sw + px] = c;
      }
    }
  }
}

/* ============================================================
 *                      Font rendering on g_fb
 * ============================================================ */

/* Draw a single glyph bitmap (alpha bytes, threshold at 128) into the
 * launcher buffer at (x, y) with the given ARGB colour.  Identical math
 * to font_lib.c:draw_glyph but writes to a local buffer instead of
 * calling window_draw — needed so we keep the launcher's background
 * transparent everywhere we don't paint a tile. */
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
    int gx = 0;
    while (gx < gi->width) {
      while (gx < gi->width && bitmap[gy * gi->width + gx] <= 128)
        gx++;
      int span_start = gx;
      while (gx < gi->width && bitmap[gy * gi->width + gx] > 128)
        gx++;
      if (gx > span_start) {
        fb_rrect(start_x + span_start, start_y + gy, gx - span_start, 1, 0,
                 color);
      }
    }
  }
}

/* Draw a UTF-8 string into the launcher buffer.  No wrapping, no
 * measurement — use buf_text_width() first to position. */
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

/* Truncate s in place to fit within max_w pixels, appending an ellipsis
 * if anything was cut.  Writes at most max_w-ellipsis_w worth of chars. */
static void buf_truncate_to_width(char *s, int max_w) {
  if (!g_font || !s || max_w <= 0)
    return;
  /* If it already fits, do nothing. */
  if (buf_text_width(s) <= max_w)
    return;
  /* Walk from the end, removing characters until width fits with room
   * for the "…" ellipsis.  We accept some imprecision (the ellipsis
   * width is approximated as the width of "."). */
  int dots_w = buf_text_width("...");
  int target = max_w - dots_w;
  if (target < 0)
    target = 0;
  int len = (int)strlen(s);
  while (len > 0 && buf_text_width(s) > target) {
    s[--len] = '\0';
  }
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

/* Append an app to g_apps[].  Returns 1 on success, 0 if full. */
static int apps_add(const char *path, const char *label, enum cat cat) {
  if (g_n_apps >= MAX_APPS)
    return 0;
  if (!path || !*path)
    return 0;
  struct app_def *a = &g_apps[g_n_apps];
  int n;
  /* path */
  n = 0;
  while (path[n] && n < PATH_MAX - 1) {
    a->path[n] = path[n];
    n++;
  }
  a->path[n] = '\0';
  /* label (default = basename of path) */
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

/* Append a folder marker to g_apps[].  Folders live in the same array
 * but with category = CAT_FOLDER and path = "folder:<children-id-list>".
 * For our simple model children are recorded inline as a NUL-separated
 * list appended right after the folder entry — we keep this trivial by
 * just storing the children's paths on lines and parsing at view-time. */
struct folder {
  int first_child; /* index in g_apps[] of the first child             */
  int child_count;
};
#define MAX_FOLDERS 16
static struct folder g_folders[MAX_FOLDERS];
static int g_n_folders;
static int g_cur_folder_first; /* first child of the folder we're parsing */
static int g_cur_folder_count;

static int apps_add_folder(const char *name) {
  if (g_n_folders >= MAX_FOLDERS)
    return -1;
  if (g_n_apps >= MAX_APPS)
    return -1;
  struct app_def *a = &g_apps[g_n_apps];
  a->path[0] = '\0'; /* folders have no executable                     */
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
  int f = g_n_folders++;
  g_cur_folder_first = g_folders[f].first_child;
  g_cur_folder_count = 0;
  return id;
}

static void apps_end_folder(void) {
  if (g_n_folders == 0)
    return;
  g_folders[g_n_folders - 1].child_count =
      g_n_apps - g_folders[g_n_folders - 1].first_child;
  g_cur_folder_first = -1;
  g_cur_folder_count = 0;
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

/* Read /sys/bin/nxlauncher.cfg if present.  Format:
 *   path:label:cat
 *   folder:Name
 *     path:label:cat
 *     path:label:cat
 *   end
 * Lines starting with '#' or empty are ignored.  cat is one of
 * SYSTEM/USER/UTILITY/GAMES/OFFICE/OTHER (case-insensitive); absent =
 * UTILITY. */
static enum cat parse_cat(const char *s) {
  if (!s || !*s)
    return CAT_UTILITY;
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
  return CAT_UTILITY;
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

  /* Line-by-line parse.  Replace each '\n' with '\0' in place. */
  int in_folder = 0;
  char *p = buf;
  while (p < buf + n) {
    char *eol = p;
    while (eol < buf + n && *eol != '\n')
      eol++;
    if (eol < buf + n)
      *eol = '\0';

    /* Trim leading whitespace. */
    while (*p == ' ' || *p == '\t')
      p++;
    /* Skip blanks/comments. */
    if (*p == '\0' || *p == '#') {
      p = eol + 1;
      continue;
    }

    /* Folder begin: folder:Name */
    if (strncmp(p, "folder:", 7) == 0) {
      if (in_folder)
        apps_end_folder();
      const char *name = p + 7;
      if (apps_add_folder(name) < 0) {
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
      /* path:label:cat — split on ':' up to three fields. */
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

/* Auto-scan /bin/.  list_dir returns a space-separated string; we tokenise
 * with strtok_r.  Each non-empty token is added as a USER-category app
 * with a /bin/<name> path. */
static void scan_bin(void) {
  char buf[1024];
  int n = list_dir("/bin", buf, sizeof(buf) - 1);
  if (n <= 0)
    return;
  buf[n] = '\0';
  char *save = (char *)0;
  for (char *tok = strtok_r(buf, " \t", &save); tok;
       tok = strtok_r((char *)0, " \t", &save)) {
    if (!*tok)
      continue;
    /* Compose /bin/<name> */
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

/* ============================================================
 *                         Layout
 * ============================================================ */

static int grid_origin_y;  /* y of the top of the grid (below NOTIFY) */
static int grid_origin_x;  /* x of the leftmost column               */
static int grid_w, grid_h; /* size of the grid (excluding dots)      */
static int cols, rows;     /* tiles per page                         */

/* Compute the grid rectangle and per-page tile geometry.  Called whenever
 * the desktop size changes. */
static void recompute_layout(void) {
  grid_origin_y = NOTIFY_BAR_H;
  int usable_h = g_sh - NOTIFY_BAR_H - DOCK_H;
  /* Reserve space for the back arrow (folder view) and the dot row. */
  int dot_row_h = DOT_DIA + DOT_BOTTOM;
  int top_extra = 0;
  if (g_view == VIEW_FOLDER)
    top_extra = BACK_H + 12;
  grid_h = usable_h - dot_row_h - top_extra;
  if (grid_h < 0)
    grid_h = 0;

  int usable_w = g_sw;
  grid_w = usable_w;
  grid_origin_x = 0;

  /* How many tiles fit horizontally?  Account for the side arrows which
   * are shown whenever paging is needed. */
  int tile_pitch_x = TILE + TILE_GAP_X;
  int tile_pitch_y = TILE + LABEL_GAP + LABEL_MAX_W + TILE_GAP_Y;
  cols = (grid_w + TILE_GAP_X) / tile_pitch_x;
  if (cols < 1)
    cols = 1;
  rows = (grid_h + TILE_GAP_Y) / tile_pitch_y;
  if (rows < 1)
    rows = 1;

  /* If paging is going to be needed, leave room for the arrows.  Check
   * against the row of the actual visible page after we know n_visible. */
  /* We compute the per-page slot count AFTER we know how many apps are on
   * the current view (see populate_slots). */
}

/* Determine which apps are on the current page (in the current view) and
 * build g_slots[] for hit-testing.  Returns the page-app index range
 * [out_first, out_first + out_count). */
static int current_view_first(void) {
  if (g_view == VIEW_PAGES)
    return 0;
  if (g_view == VIEW_FOLDER) {
    /* g_view_top[] should hold the parent's first-child index for the
     * folder we're inside.  If we ever reach here with no parent (root
     * view fell through), bail back to pages. */
    if (g_view_top[0] < 0)
      return 0;
    return g_view_top[0];
  }
  return 0;
}

static int current_view_count(void) {
  if (g_view == VIEW_PAGES)
    return g_n_apps;
  if (g_view == VIEW_FOLDER && g_view_top[0] >= 0) {
    int folder = g_view_top[0] - 1; /* parent app index is one before */
    return apps_folder_count(folder);
  }
  return g_n_apps;
}

/* Rebuild g_slots[] from the apps on the current page of the current view.
 * Also recomputes g_pages, g_slot_n, and the arrow/dot hit rects. */
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

  /* Reserve arrow slots?  Only if paging is needed.  Adjust cols then. */
  int effective_cols = cols;
  int effective_rows = rows;
  if (count > per_page && cols > 1) {
    /* Need arrow buttons on each row — sacrifice one column. */
    effective_cols = cols - 1;
    if (effective_cols < 1)
      effective_cols = 1;
  }
  per_page = effective_cols * effective_rows;
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

  /* Tile the page.  Layout: centred horizontally, top-aligned within the
   * grid rect. */
  int page_start = first + g_page * per_page;
  int page_count = count - g_page * per_page;
  if (page_count < 0)
    page_count = 0;
  if (page_count > per_page)
    page_count = per_page;

  /* Compute horizontal centring offset for the effective tile columns. */
  int grid_pixel_w = effective_cols * TILE + (effective_cols - 1) * TILE_GAP_X;
  int start_x = (g_sw - grid_pixel_w) / 2;

  /* Reserve a left strip for arrows when paging is needed. */
  int arrow_strip = 0;
  if (count > per_page)
    arrow_strip = ARROW_W + ARROW_GAP;
  /* Tiles begin at start_x + arrow_strip. */
  int tile_x0 = start_x + arrow_strip;

  /* Vertical centring: top-aligned with grid_origin_y plus the back-row
   * reservation (folder view). */
  int back_extra = (g_view == VIEW_FOLDER) ? (BACK_H + 12) : 0;
  int row_pixel_h = TILE + LABEL_GAP + LABEL_MAX_W + TILE_GAP_Y;
  int grid_pixel_h = effective_rows * row_pixel_h - TILE_GAP_Y;
  int start_y =
      grid_origin_y + back_extra + ((grid_h - back_extra - grid_pixel_h) / 2);
  if (start_y < grid_origin_y + back_extra)
    start_y = grid_origin_y + back_extra;

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

  /* Arrow hit rects (full vertical strip, but only middle row). */
  if (count > per_page) {
    int mid_y =
        start_y + (effective_rows / 2) * row_pixel_h + (TILE - ARROW_H) / 2;
    g_arrow_l_x = (start_x + ARROW_W / 2);
    g_arrow_l_y = mid_y + ARROW_H / 2;
    g_arrow_r_x = (g_sw - start_x - ARROW_W / 2);
    g_arrow_r_y = mid_y + ARROW_H / 2;
  } else {
    g_arrow_l_x = g_arrow_r_x = -1;
  }

  /* Dots: centred below the grid. */
  g_dots_y = grid_origin_y + back_extra + grid_h - DOT_BOTTOM - DOT_DIA / 2;
  if (count > per_page) {
    int dots_w = g_pages * DOT_DIA + (g_pages - 1) * DOT_GAP;
    g_dots_x0 = (g_sw - dots_w) / 2 + DOT_DIA / 2;
  } else {
    g_dots_x0 = -1;
  }

  /* Back arrow (folder view). */
  if (g_view == VIEW_FOLDER) {
    g_back_x = (g_sw - BACK_W) / 2;
    g_back_y = grid_origin_y + 4;
  } else {
    g_back_x = g_back_y = -1;
  }
}

/* ============================================================
 *                         Redraw
 * ============================================================ */

/* Draw one tile centred at (cx, cy) with the given label below it.
 * The tile fills (cx - TILE/2 .. cx + TILE/2, cy - TILE/2 .. cy + TILE/2);
 * the label is centred horizontally below the tile. */
static void draw_tile(int x, int y, uint32_t fill, const char *label) {
  fb_rrect(x, y, TILE, TILE, TILE_RADIUS, fill);

  /* Subtle inner highlight: a thin lighter rect 4 px inside the tile,
   * with 30% alpha.  Skipped when the font is missing because we can't
   * reliably shade.  For MVP just leave the tile flat — keeps the code
   * simple and matches the iOS flat-tile aesthetic. */

  /* Label below the tile, centred, truncated to TILE + slack. */
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
  /* Soft drop-shadow for legibility on light backgrounds. */
  if (g_font)
    buf_draw_text(lx + 1, ly + 1, tmp, COL_LABEL_SHADOW);
  buf_draw_text(lx, ly, tmp, COL_LABEL);
}

static void draw_folder_tile(int x, int y) {
  /* Folder tile: same shape, with a tiny "tab" notch at the top to suggest
   * a folder (Material-style folder icon in spirit, not pixel-accurate). */
  fb_rrect(x, y, TILE, TILE, TILE_RADIUS, COL_TILE[CAT_FOLDER]);
  /* Draw a smaller darker rect inside the top-left to suggest the folder
   * tab.  28×10 px, rounded. */
  int tab_w = 36, tab_h = 10;
  fb_rrect(x + 8, y + 10, tab_w, tab_h, 3, 0xCC000000u);
  /* Centerline divider (cosmetic): horizontal line at mid-height. */
  int mid_y = y + TILE / 2;
  for (int i = 14; i < TILE - 14; i++)
    if (x + i >= 0 && x + i < g_sw && mid_y >= 0 && mid_y < g_sh)
      g_fb[mid_y * g_sw + (x + i)] = 0x33000000u;
  /* Label below, just like app tiles.  The label is the folder name. */
}

static void redraw(void) {
  /* Transparent first — the compositor paints the desktop under us. */
  fb_fill(0x00000000u);

  /* Back arrow when in folder view. */
  if (g_view == VIEW_FOLDER && g_back_x >= 0) {
    fb_rrect(g_back_x, g_back_y, BACK_W, BACK_H, 8, COL_BACK);
    if (g_font) {
      const char *t = "< Back";
      int tw = buf_text_width(t);
      buf_draw_text(g_back_x + (BACK_W - tw) / 2, g_back_y + (BACK_H - 16) / 2,
                    t, COL_LABEL);
    }
  }

  /* Tiles. */
  for (int i = 0; i < g_slot_n; i++) {
    int idx = g_slots[i].idx;
    const struct app_def *a = &g_apps[idx];
    if (g_slots[i].kind == 1) {
      draw_folder_tile(g_slots[i].x, g_slots[i].y);
      char tmp[NAME_MAX + 4];
      int n = 0;
      while (a->label[n] && n < (int)sizeof(tmp) - 4) {
        tmp[n] = a->label[n];
        n++;
      }
      tmp[n] = '\0';
      buf_truncate_to_width(tmp, LABEL_MAX_W);
      int tw = buf_text_width(tmp);
      int lx = g_slots[i].x + (TILE - tw) / 2;
      int ly = g_slots[i].y + TILE + LABEL_GAP;
      if (g_font)
        buf_draw_text(lx + 1, ly + 1, tmp, COL_LABEL_SHADOW);
      buf_draw_text(lx, ly, tmp, COL_LABEL);
    } else {
      uint32_t fill = COL_TILE[a->category];
      draw_tile(g_slots[i].x, g_slots[i].y, fill, a->label);
    }
  }

  /* Side arrows (only when paging is needed). */
  if (g_arrow_l_x >= 0) {
    uint32_t lc = (g_pressed == 1) ? COL_ARROW_PRESSED : COL_ARROW;
    uint32_t rc = (g_pressed == 2) ? COL_ARROW_PRESSED : COL_ARROW;
    int ax_l = g_arrow_l_x - ARROW_W / 2;
    int ax_r = g_arrow_r_x - ARROW_W / 2;
    int ay = g_arrow_l_y - ARROW_H / 2;
    fb_rrect(ax_l, ay, ARROW_W, ARROW_H, 6, lc);
    fb_rrect(ax_r, ay, ARROW_W, ARROW_H, 6, rc);
    /* Chevron glyph: simple "<" and ">" made of three line segments via
     * small filled rounded rects. */
    if (g_pressed == 1)
      ax_l += 1;
    if (g_pressed == 2)
      ax_r += 1;
    /* Left chevron */
    fb_rrect(ax_l + ARROW_W / 2 - 5, ay + ARROW_H / 2 - 4, 2, 8, 1, COL_LABEL);
    fb_rrect(ax_l + ARROW_W / 2 - 3, ay + ARROW_H / 2 - 2, 2, 4, 1, COL_LABEL);
    fb_rrect(ax_l + ARROW_W / 2 - 5, ay + ARROW_H / 2 - 4, 4, 2, 1, COL_LABEL);
    fb_rrect(ax_l + ARROW_W / 2 - 5, ay + ARROW_H / 2 + 2, 4, 2, 1, COL_LABEL);
    /* Right chevron */
    fb_rrect(ax_r + ARROW_W / 2 + 3, ay + ARROW_H / 2 - 4, 2, 8, 1, COL_LABEL);
    fb_rrect(ax_r + ARROW_W / 2 + 1, ay + ARROW_H / 2 - 2, 2, 4, 1, COL_LABEL);
    fb_rrect(ax_r + ARROW_W / 2 + 1, ay + ARROW_H / 2 - 4, 4, 2, 1, COL_LABEL);
    fb_rrect(ax_r + ARROW_W / 2 + 1, ay + ARROW_H / 2 + 2, 4, 2, 1, COL_LABEL);
  }

  /* Page dots (Android-style indicator). */
  if (g_dots_x0 >= 0) {
    for (int i = 0; i < g_pages; i++) {
      int cx = g_dots_x0 + i * (DOT_DIA + DOT_GAP);
      uint32_t c = (i == g_page) ? COL_DOT_ACTIVE : COL_DOT_INACTIVE;
      fb_circle(cx, g_dots_y, DOT_DIA / 2, c);
    }
  }
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
        y >= g_slots[i].y && y < g_slots[i].y + TILE) {
      return i;
    }
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
  g_view_top[0] = first; /* parent path is "folder index + 1"        */
  g_page = 0;
}

static void leave_folder(void) {
  g_view = VIEW_PAGES;
  g_view_top[0] = -1;
  g_page = 0;
}

static void handle_click(int mx, int my) {
  /* 1. Back arrow (folder view) — highest priority. */
  if (hit_back(mx, my)) {
    leave_folder();
    g_sig = 0;
    return;
  }

  /* 2. Side arrows — paging. */
  int a = hit_arrow(mx, my);
  if (a == 1) {
    g_pressed = 1;
    g_page = (g_page - 1 + g_pages) % g_pages;
    g_sig = 0;
    return;
  }
  if (a == 2) {
    g_pressed = 2;
    g_page = (g_page + 1) % g_pages;
    g_sig = 0;
    return;
  }

  /* 3. Dot — paging. */
  int d = hit_dot(mx, my);
  if (d >= 0 && d != g_page) {
    g_page = d;
    g_sig = 0;
    return;
  }

  /* 4. Tile. */
  int s = hit_slot(mx, my);
  if (s >= 0) {
    int idx = g_slots[s].idx;
    if (g_apps[idx].category == CAT_FOLDER) {
      enter_folder(idx);
      g_sig = 0;
    } else if (g_apps[idx].path[0]) {
      int pid = spawn_args(g_apps[idx].path, 0, (char *const *)0);
      if (pid <= 0)
        printf("[launcher] spawn failed: %s\n", g_apps[idx].path);
    }
    return;
  }

  /* 5. Empty space: do nothing (the launcher's hit area covers the whole
   *    desktop, so a "miss" here means the click was on a transparent
   *    pixel — but since top_most+!passive we always receive the click;
   *    this is the place to bounce focus back to g_last_focus). */
}

/* ============================================================
 *                         Main loop
 * ============================================================ */

static void reinit_window(int sw, int sh) {
  g_dsw = sw;
  g_dsh = sh;
  g_sw = sw;
  g_sh = sh;

  uint32_t *nb = (uint32_t *)malloc((size_t)sw * sh * 4);
  if (nb) {
    free(g_fb);
    g_fb = nb;
  }
  if (g_win >= 0)
    destroy_window(g_win);
  g_win = create_window(0, 0, sw, sh, "nxlauncher");
  if (g_win >= 0) {
    set_window_flags(g_win, 1); /* top_most — no titlebar, no shadows */
  }
  g_sig = 0;
  recompute_layout();
  populate_slots();
}

int main(void) {
  /* Load the font (may fail; we still work without text). */
  g_font = font_load("/fonts/Rewir-Light.off");

  /* Discover apps: hardcoded → /bin → cfg. */
  for (int i = 0; i < G_SYS_N; i++)
    apps_add(g_sys[i].path, g_sys[i].label, g_sys[i].cat);
  scan_bin();
  load_cfg();

  /* Default view: top-level pages. */
  g_view = VIEW_PAGES;
  g_view_top[0] = -1;
  g_page = 0;
  g_pages = 1;

  /* Window + buffer. */
  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;
  g_fb = (uint32_t *)malloc((size_t)sw * sh * 4);
  if (!g_fb)
    return 1;
  reinit_window(sw, sh);

  for (;;) {
    /* Polled resize. */
    long d = OS1_display_info();
    int cw = (int)((d >> 16) & 0xFFFF), ch = (int)(d & 0xFFFF);
    if (cw > 0 && ch > 0 && (cw != g_dsw || ch != g_dsh)) {
      reinit_window(cw, ch);
    } else {
      /* The app table can change at runtime (config reload is a future
       * feature); for now refresh only on view/page change. */
      populate_slots();
    }

    /* Remember the currently focused window so we can hand focus back
     * after handling our click.  We snapshot at the top of each tick so
     * the value reflects whatever the system thinks is focused right
     * now (the user may have switched apps between our polls). */
    int current_focus = g_last_focus;
    struct window_info wi[8];
    int wn = (int)OS1_window_enum(wi, 8);
    for (int i = 0; i < wn; i++) {
      if (wi[i].id != g_win && (wi[i].flags & WININFO_FOCUSED)) {
        current_focus = wi[i].pid;
        g_last_focus = wi[i].pid;
        break;
      }
    }

    redraw();

    /* Signature-driven skip: if nothing changed (no click landed, no
     * resize, no view switch) we still blit at least once per frame to
     * keep the alpha buffer in sync with the desktop.  We blit every
     * iteration; the signature is currently unused but the hook is kept
     * for a future optimisation. */
    window_blit(g_win, 0, 0, g_sw, g_sh, g_fb);

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
        long d2 = OS1_display_info();
        int rw = (int)((d2 >> 16) & 0xFFFF), rh = (int)(d2 & 0xFFFF);
        if (rw > 0 && rh > 0)
          reinit_window(rw, rh);
      }
    }

    /* Hand focus back to whatever app was focused before us.  Done
     * unconditionally so a stray click on empty desktop space still
     * returns focus to the user's app. */
    if (current_focus > 0)
      OS1_window_focus(current_focus);

    g_pressed = 0;
    OS1_sleep(50); /* 20 Hz — a launcher doesn't need more than that */
  }
  return 0;
}
