/*
 * user/sys/bin/nxfilem/ui.c
 * Layout, palette, hit-testing and the redraw entry point.
 *
 * Same button-hit-table technique as nxsettings.c: a per-frame table that
 * IS both the draw call and the hit-test source, rebuilt every redraw so
 * drawing and hit-testing can never drift apart. Redraw itself is gated by
 * a state signature (FNV-1a over everything visible) exactly like
 * nxui.c's redraw(force)/g_sig — nothing is blitted when nothing the user
 * could see has actually changed, so this app never busy-spins even though
 * the event loop polls at a fixed ~30Hz.
 */
#include "nxfilem.h"

/* ============================== Palette ============================== */
static uint32_t g_col_bg, g_col_menubar, g_col_toolbar, g_col_sidebar;
static uint32_t g_col_row_alt, g_col_row_hover, g_col_row_sel;
static uint32_t g_col_text, g_col_text_dim, g_col_accent, g_col_border;
static uint32_t g_col_btn, g_col_btn_active, g_col_danger;
static int g_light;

void fm_ui_load_theme(int is_light) {
  g_light = is_light;
  if (is_light) {
    g_col_bg = 0xFFF5F5F7u;
    g_col_menubar = 0xFFE5E5EAu;
    g_col_toolbar = 0xFFECECEEu;
    g_col_sidebar = 0xFFE5E5EAu;
    g_col_row_alt = 0xFFEDEDF0u;
    g_col_row_hover = 0xFFDCDCE0u;
    g_col_row_sel = 0xFFB8D4FEu;
    g_col_text = 0xFF1C1C1Eu;
    g_col_text_dim = 0xFF6E6E73u;
    g_col_accent = 0xFF007AFFu;
    g_col_border = 0xFFD0D0D5u;
    g_col_btn = 0xFFE0E0E0u;
    g_col_btn_active = 0xFF007AFFu;
    g_col_danger = 0xFFD93A3Au;
  } else {
    g_col_bg = 0xFF1E1E24u;
    g_col_menubar = 0xFF17171Cu;
    g_col_toolbar = 0xFF232329u;
    g_col_sidebar = 0xFF17171Cu;
    g_col_row_alt = 0xFF232329u;
    g_col_row_hover = 0xFF2C2C34u;
    g_col_row_sel = 0xFF3F7FD1u;
    g_col_text = 0xFFEAEAEEu;
    g_col_text_dim = 0xFF9A9AA4u;
    g_col_accent = 0xFF4FA3FFu;
    g_col_border = 0xFF33333Du;
    g_col_btn = 0xFF2C2C34u;
    g_col_btn_active = 0xFF3F7FD1u;
    g_col_danger = 0xFFE55A5Au;
  }
}

/* ============================== Actions ============================== */
enum {
  ACT_NONE = 0,
  ACT_TOP_MENU,
  ACT_NAV_BACK,
  ACT_NAV_FORWARD,
  ACT_NAV_UP,
  ACT_NAV_HOME,
  ACT_REFRESH,
  ACT_TOGGLE_HIDDEN,
  ACT_SORT_NAME,
  ACT_SORT_SIZE,
  ACT_SORT_DATE,
  ACT_SORT_TYPE,
  ACT_SELECT_ALL,
  ACT_DESELECT_ALL,
  ACT_OPEN,
  ACT_OPEN_KILO,
  ACT_RENAME,
  ACT_COPY,
  ACT_CUT,
  ACT_PASTE,
  ACT_DELETE,
  ACT_NEW_FOLDER,
  ACT_QUIT,
  ACT_ABOUT,
};

struct fm_btn {
  int x, y, w, h;
  int action;
  long arg;
};
static struct fm_btn g_btns[FM_MAX_BTNS];
static int g_btn_n;

static void add_btn(int x, int y, int w, int h, int action, long arg) {
  if (g_btn_n >= FM_MAX_BTNS)
    return;
  g_btns[g_btn_n].x = x;
  g_btns[g_btn_n].y = y;
  g_btns[g_btn_n].w = w;
  g_btns[g_btn_n].h = h;
  g_btns[g_btn_n].action = action;
  g_btns[g_btn_n].arg = arg;
  g_btn_n++;
}

/* ============================== Layout ============================== */
#define TOOLBAR_BTN_W 28
#define TOOLBAR_BTN_H 26
#define HEADER_H 22
#define SIZE_COL_W 90

static int g_content_x, g_content_y, g_content_w, g_content_h;
static int g_rows_y0, g_visible_rows;

static void compute_layout(void) {
  g_content_x = FM_SIDEBAR_W;
  g_content_y = FM_MENUBAR_H + FM_TOOLBAR_H;
  g_content_w = fm_fb_w - FM_SIDEBAR_W;
  g_content_h = fm_fb_h - g_content_y - FM_STATUSBAR_H;
  if (g_content_w < 0)
    g_content_w = 0;
  if (g_content_h < 0)
    g_content_h = 0;
  g_rows_y0 = g_content_y + HEADER_H;
  g_visible_rows = (g_content_h - HEADER_H) / FM_ROW_H;
  if (g_visible_rows < 0)
    g_visible_rows = 0;
}

/* ============================== Rename ============================== */
void fm_ui_begin_rename(int index) {
  if (index < 0 || index >= fm_state.file_count)
    return;
  fm_state.rename.active = 1;
  fm_state.rename.index = index;
  strncpy(fm_state.rename.buf, fm_state.files[index].name,
          sizeof(fm_state.rename.buf) - 1);
  fm_state.rename.buf[sizeof(fm_state.rename.buf) - 1] = '\0';
  fm_state.rename.len = (int)strlen(fm_state.rename.buf);
}

void fm_ui_commit_rename(void) {
  if (!fm_state.rename.active)
    return;

  int idx = fm_state.rename.index;

  char old_path[FM_PATH_MAX];
  char new_name[FM_NAME_MAX];

  old_path[0] = '\0';
  new_name[0] = '\0';

  if (idx >= 0 && idx < fm_state.file_count && fm_state.rename.len > 0) {

    strncpy(old_path, fm_state.files[idx].full_path, sizeof(old_path) - 1);

    old_path[sizeof(old_path) - 1] = '\0';

    strncpy(new_name, fm_state.rename.buf, sizeof(new_name) - 1);

    new_name[sizeof(new_name) - 1] = '\0';

    fm_rename_file(old_path, new_name);
  }

  fm_state.rename.active = 0;
  fm_state.rename.index = -1;
  fm_state.rename.len = 0;
  fm_state.rename.buf[0] = '\0';

  fm_refresh_directory();
}

void fm_ui_cancel_rename(void) { fm_state.rename.active = 0; }

int fm_ui_visible_rows(void) {
  compute_layout();
  return g_visible_rows;
}

/* ========================= Section renderers ========================= */

static const char *const MENU_LABELS[] = {"File", "Edit", "View", "Help"};
#define MENU_COUNT 4

struct menu_entry {
  const char *label;
  int action;
};
static const struct menu_entry MENU_FILE[] = {
    {"Open", ACT_OPEN},
    {"Open in Kilo", ACT_OPEN_KILO},
    {"New Folder", ACT_NEW_FOLDER},
    {"Rename", ACT_RENAME},
    {"Delete", ACT_DELETE},
    {"Refresh", ACT_REFRESH},
    {"Quit", ACT_QUIT},
};
static const struct menu_entry MENU_EDIT[] = {
    {"Copy", ACT_COPY},
    {"Cut", ACT_CUT},
    {"Paste", ACT_PASTE},
    {"Select All", ACT_SELECT_ALL},
    {"Deselect All", ACT_DESELECT_ALL},
};
static const struct menu_entry MENU_VIEW[] = {
    {"Toggle Hidden Files", ACT_TOGGLE_HIDDEN},
    {"Sort by Name", ACT_SORT_NAME},
    {"Sort by Size", ACT_SORT_SIZE},
    {"Sort by Date", ACT_SORT_DATE},
    {"Sort by Type", ACT_SORT_TYPE},
};
static const struct menu_entry MENU_HELP[] = {
    {"About", ACT_ABOUT},
};
static const struct menu_entry *const MENU_ITEMS[MENU_COUNT] = {
    MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_HELP};
static const int MENU_ITEM_COUNTS[MENU_COUNT] = {
    (int)(sizeof(MENU_FILE) / sizeof(MENU_FILE[0])),
    (int)(sizeof(MENU_EDIT) / sizeof(MENU_EDIT[0])),
    (int)(sizeof(MENU_VIEW) / sizeof(MENU_VIEW[0])),
    (int)(sizeof(MENU_HELP) / sizeof(MENU_HELP[0])),
};
static const int MENU_X[MENU_COUNT] = {8, 56, 104, 152};
#define MENU_ITEM_H 22
#define MENU_W 168

static const struct menu_entry CTX_ITEMS[] = {
    {"Open", ACT_OPEN}, {"Rename", ACT_RENAME}, {"Copy", ACT_COPY},
    {"Cut", ACT_CUT},   {"Paste", ACT_PASTE},   {"Delete", ACT_DELETE},
};
#define CTX_COUNT (int)(sizeof(CTX_ITEMS) / sizeof(CTX_ITEMS[0]))

static void render_dropdown(int x, int y, const struct menu_entry *items,
                            int count) {
  int h = count * MENU_ITEM_H;
  if (x + MENU_W > fm_fb_w)
    x = fm_fb_w - MENU_W;
  if (y + h > fm_fb_h)
    y = fm_fb_h - h;
  fm_fill_rect(x, y, MENU_W, h, g_col_toolbar);
  fm_fill_rect(x, y, MENU_W, 1, g_col_border);
  fm_fill_rect(x, y + h - 1, MENU_W, 1, g_col_border);
  fm_fill_rect(x, y, 1, h, g_col_border);
  fm_fill_rect(x + MENU_W - 1, y, 1, h, g_col_border);
  for (int i = 0; i < count; i++) {
    int iy = y + i * MENU_ITEM_H;
    fm_draw_text(x + 10, iy + 4, items[i].label, g_col_text);
    add_btn(x, iy, MENU_W, MENU_ITEM_H, items[i].action, 0);
  }
}

static void render_menubar(void) {
  fm_fill_rect(0, 0, fm_fb_w, FM_MENUBAR_H, g_col_menubar);
  for (int i = 0; i < MENU_COUNT; i++) {
    int x = MENU_X[i];
    uint32_t color = g_col_text;
    if (fm_state.menu_open == i) {
      fm_fill_rect(x - 6, 0, 44, FM_MENUBAR_H, g_col_toolbar);
      color = g_col_accent;
    }
    fm_draw_text(x, 6, MENU_LABELS[i], color);
    add_btn(x - 6, 0, 44, FM_MENUBAR_H, ACT_TOP_MENU, i);
  }
  fm_fill_rect(0, FM_MENUBAR_H - 1, fm_fb_w, 1, g_col_border);

  if (fm_state.menu_open >= 0 && fm_state.menu_open < MENU_COUNT)
    render_dropdown(MENU_X[fm_state.menu_open], FM_MENUBAR_H,
                    MENU_ITEMS[fm_state.menu_open],
                    MENU_ITEM_COUNTS[fm_state.menu_open]);
}

static void toolbar_btn(int x, int y, const char *label, int action,
                        int enabled) {
  fm_rrect(x, y, TOOLBAR_BTN_W, TOOLBAR_BTN_H, 5,
           enabled ? g_col_btn : g_col_toolbar);
  int tw = fm_text_width(label);
  fm_draw_text(x + (TOOLBAR_BTN_W - tw) / 2, y + 6, label,
               enabled ? g_col_text : g_col_text_dim);
  if (enabled)
    add_btn(x, y, TOOLBAR_BTN_W, TOOLBAR_BTN_H, action, 0);
}

static void render_toolbar(void) {
  int y = FM_MENUBAR_H;
  fm_fill_rect(0, y, fm_fb_w, FM_TOOLBAR_H, g_col_toolbar);
  int by = y + (FM_TOOLBAR_H - TOOLBAR_BTN_H) / 2;
  int bx = 6;

  toolbar_btn(bx, by, "<", ACT_NAV_BACK, fm_state_can_undo());
  bx += TOOLBAR_BTN_W + 4;
  toolbar_btn(bx, by, ">", ACT_NAV_FORWARD, fm_state_can_redo());
  bx += TOOLBAR_BTN_W + 4;
  toolbar_btn(bx, by, "^", ACT_NAV_UP, strcmp(fm_state.current_path, "/") != 0);
  bx += TOOLBAR_BTN_W + 4;
  toolbar_btn(bx, by, "H", ACT_NAV_HOME, 1);
  bx += TOOLBAR_BTN_W + 4;
  toolbar_btn(bx, by, "R", ACT_REFRESH, 1);
  bx += TOOLBAR_BTN_W + 10;

  int right_w = 96;
  int path_x = bx;
  int path_w = fm_fb_w - bx - right_w - 8;
  if (path_w < 20)
    path_w = 20;
  fm_draw_text(path_x, by + 6, fm_state.current_path, g_col_text_dim);
  (void)path_w; /* text is left-aligned and simply clipped by fm_draw_text's
                 * own bounds check; no ellipsis pass needed for now */

  int hidden_x = fm_fb_w - right_w;
  fm_rrect(hidden_x, by, right_w - 8, TOOLBAR_BTN_H, 5,
           fm_state.show_hidden ? g_col_btn_active : g_col_btn);
  fm_draw_text(hidden_x + 8, by + 6, "Hidden", g_col_text);
  add_btn(hidden_x, by, right_w - 8, TOOLBAR_BTN_H, ACT_TOGGLE_HIDDEN, 0);

  fm_fill_rect(0, y + FM_TOOLBAR_H - 1, fm_fb_w, 1, g_col_border);
}

static void render_sidebar(void) {
  fm_fill_rect(0, g_content_y, FM_SIDEBAR_W, g_content_h, g_col_sidebar);

  int y = g_content_y + FM_PAD;
  fm_rrect(FM_PAD, y, FM_SIDEBAR_W - 2 * FM_PAD, 24, 5, g_col_btn);
  fm_draw_text(FM_PAD + 8, y + 5, "Home", g_col_text);
  add_btn(FM_PAD, y, FM_SIDEBAR_W - 2 * FM_PAD, 24, ACT_NAV_HOME, 0);
  y += 34;

  fm_draw_text(FM_PAD, y, "Location", g_col_accent);
  y += 18;
  char line[64];
  const char *p = fm_state.current_path;
  int plen = (int)strlen(p);
  int max_chars = (FM_SIDEBAR_W - 2 * FM_PAD) / 7;
  if (plen > max_chars && max_chars > 3)
    snprintf(line, sizeof(line), "...%s", p + (plen - (max_chars - 3)));
  else
    snprintf(line, sizeof(line), "%s", p);
  fm_draw_text(FM_PAD, y, line, g_col_text_dim);
  y += 26;

  snprintf(line, sizeof(line), "%d items", fm_state.file_count);
  fm_draw_text(FM_PAD, y, line, g_col_text);
  y += 18;
  snprintf(line, sizeof(line), "%lld KB total", fm_state.total_size / 1024LL);
  fm_draw_text(FM_PAD, y, line, g_col_text);
  y += 18;
  if (fm_state.selected_count > 0) {
    snprintf(line, sizeof(line), "%d selected", fm_state.selected_count);
    fm_draw_text(FM_PAD, y, line, g_col_accent);
  }

  fm_fill_rect(FM_SIDEBAR_W - 1, g_content_y, 1, g_content_h, g_col_border);
}

static void format_size(long size, char *out, size_t sz) {
  if (size < 1024)
    snprintf(out, sz, "%ld B", size);
  else if (size < 1024 * 1024)
    snprintf(out, sz, "%ld KB", size / 1024);
  else
    snprintf(out, sz, "%ld MB", size / (1024 * 1024));
}

static void render_content_header(void) {
  fm_fill_rect(g_content_x, g_content_y, g_content_w, HEADER_H, g_col_toolbar);
  int name_x = g_content_x + FM_PAD + FM_ICON_SIZE + 8;
  uint32_t nc =
      fm_state.sort_mode == FM_SORT_NAME ? g_col_accent : g_col_text_dim;
  uint32_t sc =
      fm_state.sort_mode == FM_SORT_SIZE ? g_col_accent : g_col_text_dim;
  fm_draw_text(name_x, g_content_y + 4, "Name", nc);
  fm_draw_text(g_content_x + g_content_w - SIZE_COL_W, g_content_y + 4, "Size",
               sc);
  add_btn(g_content_x, g_content_y, g_content_w - SIZE_COL_W, HEADER_H,
          ACT_SORT_NAME, 0);
  add_btn(g_content_x + g_content_w - SIZE_COL_W, g_content_y, SIZE_COL_W,
          HEADER_H, ACT_SORT_SIZE, 0);
  fm_fill_rect(g_content_x, g_content_y + HEADER_H - 1, g_content_w, 1,
               g_col_border);
}

static void render_rename_box(int row_y, int name_x, int name_w) {
  fm_fill_rect(name_x - 2, row_y + 2, name_w, FM_ROW_H - 4, g_col_bg);
  fm_fill_rect(name_x - 2, row_y + 2, name_w, 1, g_col_accent);
  fm_fill_rect(name_x - 2, row_y + FM_ROW_H - 3, name_w, 1, g_col_accent);
  fm_draw_text(name_x, row_y + (FM_ROW_H - 14) / 2, fm_state.rename.buf,
               g_col_text);
  int cx = name_x + fm_text_width(fm_state.rename.buf) + 1;
  fm_fill_rect(cx, row_y + 4, 1, FM_ROW_H - 8, g_col_text);
}

static void render_content(void) {
  fm_fill_rect(g_content_x, g_content_y + HEADER_H, g_content_w,
               g_content_h - HEADER_H, g_col_bg);
  render_content_header();

  if (fm_state.file_count == 0) {
    fm_draw_text(g_content_x + FM_PAD, g_rows_y0 + FM_PAD, "No files",
                 g_col_text_dim);
    return;
  }

  int end = fm_state.scroll_offset + g_visible_rows;
  if (end > fm_state.file_count)
    end = fm_state.file_count;

  for (int i = fm_state.scroll_offset; i < end; i++) {
    int row_y = g_rows_y0 + (i - fm_state.scroll_offset) * FM_ROW_H;
    fm_file_t *file = &fm_state.files[i];

    uint32_t row_bg = (i % 2) ? g_col_row_alt : g_col_bg;
    if (file->is_selected)
      row_bg = g_col_row_sel;
    if (i == fm_state.highlighted_item)
      row_bg = g_col_row_hover;
    fm_fill_rect(g_content_x, row_y, g_content_w, FM_ROW_H, row_bg);

    int icon_y = row_y + (FM_ROW_H - FM_ICON_SIZE) / 2;
    fm_draw_icon(g_content_x + FM_PAD, icon_y, FM_ICON_SIZE, file->icon_id,
                 g_light, file->is_dir ? g_col_accent : g_col_text_dim);

    int name_x = g_content_x + FM_PAD + FM_ICON_SIZE + 8;
    int name_w = g_content_w - (FM_PAD + FM_ICON_SIZE + 8) - SIZE_COL_W - 4;
    if (fm_state.rename.active && fm_state.rename.index == i) {
      render_rename_box(row_y, name_x, name_w);
    } else {
      fm_draw_text(name_x, row_y + (FM_ROW_H - 14) / 2, file->name, g_col_text);
    }

    char size_str[32];
    if (file->is_dir)
      snprintf(size_str, sizeof(size_str), "%s", "--");
    else
      format_size(file->size, size_str, sizeof(size_str));
    fm_draw_text(g_content_x + g_content_w - SIZE_COL_W,
                 row_y + (FM_ROW_H - 14) / 2, size_str, g_col_text_dim);
  }

  if (fm_state.file_count > g_visible_rows) {
    int track_h = g_content_h - HEADER_H;
    int thumb_h = (g_visible_rows * track_h) / fm_state.file_count;
    if (thumb_h < 16)
      thumb_h = 16;
    int span = fm_state.file_count - g_visible_rows;
    int thumb_y =
        g_rows_y0 +
        (span > 0 ? (fm_state.scroll_offset * (track_h - thumb_h)) / span : 0);
    fm_fill_rect(g_content_x + g_content_w - 4, g_rows_y0, 4, track_h,
                 g_col_toolbar);
    fm_fill_rect(g_content_x + g_content_w - 4, thumb_y, 4, thumb_h,
                 g_col_border);
  }
}

static void render_statusbar(void) {
  int y = fm_fb_h - FM_STATUSBAR_H;
  fm_fill_rect(0, y, fm_fb_w, FM_STATUSBAR_H, g_col_toolbar);
  fm_fill_rect(0, y, fm_fb_w, 1, g_col_border);
  if (fm_state.status_message[0])
    fm_draw_text(FM_PAD, y + 5, fm_state.status_message, g_col_text);
  else {
    char line[96];
    snprintf(line, sizeof(line), "%d items, %d selected", fm_state.file_count,
             fm_state.selected_count);
    fm_draw_text(FM_PAD, y + 5, line, g_col_text_dim);
  }
}

static void render_context_menu(void) {
  if (!fm_state.context_menu_active)
    return;
  render_dropdown(fm_state.context_menu_x, fm_state.context_menu_y, CTX_ITEMS,
                  CTX_COUNT);
}

/* ============================== Dispatch ============================== */

static void dispatch(int action, long arg) {
  int idx = fm_state.highlighted_item;
  fm_state.menu_open = -1;
  fm_state.context_menu_active = 0;

  switch (action) {
  case ACT_TOP_MENU:
    fm_state.menu_open = (fm_state.menu_open == (int)arg) ? -1 : (int)arg;
    break;
  case ACT_NAV_BACK:
    fm_navigate_back();
    break;
  case ACT_NAV_FORWARD:
    fm_navigate_forward();
    break;
  case ACT_NAV_UP:
    fm_navigate_up();
    break;
  case ACT_NAV_HOME:
    fm_navigate_home();
    break;
  case ACT_REFRESH:
    fm_refresh_directory();
    break;
  case ACT_TOGGLE_HIDDEN:
    fm_state.show_hidden = !fm_state.show_hidden;
    fm_refresh_directory();
    break;
  case ACT_SORT_NAME:
    fm_state_sort_files(FM_SORT_NAME);
    break;
  case ACT_SORT_SIZE:
    fm_state_sort_files(FM_SORT_SIZE);
    break;
  case ACT_SORT_DATE:
    fm_state_sort_files(FM_SORT_DATE);
    break;
  case ACT_SORT_TYPE:
    fm_state_sort_files(FM_SORT_TYPE);
    break;
  case ACT_SELECT_ALL:
    fm_state_select_all();
    break;
  case ACT_DESELECT_ALL:
    fm_state_deselect_all();
    break;
  case ACT_OPEN:
    if (idx >= 0 && idx < fm_state.file_count)
      fm_open_file(&fm_state.files[idx]);
    break;
  case ACT_OPEN_KILO:
    if (idx >= 0 && idx < fm_state.file_count)
      fm_open_with_kilo(&fm_state.files[idx]);
    break;
  case ACT_RENAME:
    fm_ui_begin_rename(idx);
    break;
  case ACT_COPY:
    fm_clipboard_copy();
    break;
  case ACT_CUT:
    fm_clipboard_cut();
    break;
  case ACT_PASTE:
    fm_clipboard_paste();
    break;
  case ACT_DELETE:
    if (idx >= 0 && idx < fm_state.file_count) {
      fm_delete_file(fm_state.files[idx].full_path);
      fm_refresh_directory();
    }
    break;
  case ACT_NEW_FOLDER:
    fm_create_folder(NULL);
    break;
  case ACT_QUIT:
    fm_state.running = 0;
    break;
  case ACT_ABOUT:
    fm_set_status_message("NEXS File Manager");
    break;
  default:
    break;
  }
}

/* fm_ui_handle_click - content-list rows are handled directly by geometry
 * (selection + double-click open need state the generic button table
 * doesn't carry); everything else (menus, toolbar, sidebar, context menu)
 * goes through the g_btns table built by the last redraw. */
void fm_ui_handle_click(int x, int y, int button) {
  if (fm_state.rename.active) {
    fm_ui_commit_rename();
    return;
  }

  if (button == MOUSE_BTN_LEFT && x >= g_content_x && y >= g_rows_y0 &&
      x < g_content_x + g_content_w && !fm_state.context_menu_active &&
      fm_state.menu_open < 0) {
    int row = fm_state.scroll_offset + (y - g_rows_y0) / FM_ROW_H;
    if (row >= 0 && row < fm_state.file_count) {
      fm_state.highlighted_item = row;

      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long long now_ms =
          (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
      if (fm_state.last_click_item == row &&
          (now_ms - fm_state.last_click_time_ms) < 400 &&
          fm_state.last_click_time_ms > 0) {
        fm_open_file(&fm_state.files[row]);
        fm_state.last_click_time_ms = 0;
      } else {
        fm_state.last_click_time_ms = now_ms;
        fm_state.last_click_item = row;
      }
    }
    return;
  }

  if (button == MOUSE_BTN_RIGHT && x >= g_content_x && y >= g_rows_y0 &&
      x < g_content_x + g_content_w) {
    int row = fm_state.scroll_offset + (y - g_rows_y0) / FM_ROW_H;
    if (row >= 0 && row < fm_state.file_count) {
      fm_state.highlighted_item = row;
      fm_state.context_menu_active = 1;
      fm_state.context_menu_x = x;
      fm_state.context_menu_y = y;
    }
    return;
  }

  if (button != MOUSE_BTN_LEFT) {
    fm_state.context_menu_active = 0;
    fm_state.menu_open = -1;
    return;
  }

  for (int i = g_btn_n - 1; i >= 0; i--) {
    struct fm_btn *b = &g_btns[i];
    if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
      dispatch(b->action, b->arg);
      return;
    }
  }

  fm_state.context_menu_active = 0;
  fm_state.menu_open = -1;
}

/* ============================== Signature ============================== */

static unsigned fnv1a(unsigned h, const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}
static unsigned fnv1a_str(unsigned h, const char *s) {
  return fnv1a(h, s, strlen(s));
}

static unsigned compute_signature(void) {
  unsigned h = 2166136261u;
  h = fnv1a_str(h, fm_state.current_path);
  h = fnv1a_str(h, fm_state.status_message);
  h = fnv1a(h, &fm_state.file_count, sizeof(fm_state.file_count));
  h = fnv1a(h, &fm_state.highlighted_item, sizeof(fm_state.highlighted_item));
  h = fnv1a(h, &fm_state.scroll_offset, sizeof(fm_state.scroll_offset));
  h = fnv1a(h, &fm_state.selected_count, sizeof(fm_state.selected_count));
  h = fnv1a(h, &fm_state.sort_mode, sizeof(fm_state.sort_mode));
  h = fnv1a(h, &fm_state.sort_reverse, sizeof(fm_state.sort_reverse));
  h = fnv1a(h, &fm_state.show_hidden, sizeof(fm_state.show_hidden));
  h = fnv1a(h, &fm_state.menu_open, sizeof(fm_state.menu_open));
  h = fnv1a(h, &fm_state.context_menu_active,
            sizeof(fm_state.context_menu_active));
  h = fnv1a(h, &fm_state.context_menu_x, sizeof(fm_state.context_menu_x));
  h = fnv1a(h, &fm_state.context_menu_y, sizeof(fm_state.context_menu_y));
  h = fnv1a(h, &fm_state.clipboard.is_valid,
            sizeof(fm_state.clipboard.is_valid));
  h = fnv1a(h, &fm_state.clipboard.is_cut, sizeof(fm_state.clipboard.is_cut));
  h = fnv1a(h, &fm_state.history_pos, sizeof(fm_state.history_pos));
  h = fnv1a(h, &fm_state.history_count, sizeof(fm_state.history_count));
  h = fnv1a(h, &fm_state.rename, sizeof(fm_state.rename));
  h = fnv1a(h, &g_light, sizeof(g_light));
  h = fnv1a(h, &fm_fb_w, sizeof(fm_fb_w));
  h = fnv1a(h, &fm_fb_h, sizeof(fm_fb_h));
  for (int i = 0; i < fm_state.file_count; i++)
    h = fnv1a(h, &fm_state.files[i].is_selected,
              sizeof(fm_state.files[i].is_selected));
  return h;
}

/* ============================== Public API ============================== */

int fm_ui_init(void) {
  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;

  int ww = (sw * 7) / 10;
  int wh = (sh * 7) / 10;
  if (ww > 900)
    ww = 900;
  if (wh > 640)
    wh = 640;
  if (ww < FM_MIN_WIN_W)
    ww = FM_MIN_WIN_W;
  if (wh < FM_MIN_WIN_H)
    wh = FM_MIN_WIN_H;
  int wx = (sw - ww) / 2, wy = (sh - wh) / 2;

  int win = create_window(wx, wy, ww, wh, "nxfilem");
  if (win < 0)
    return -1;
  fm_state.window_id = win;
  fm_gfx_init(win, ww, wh);

  /* Self-register this pid as the live "srv.filem_pid" endpoint
   * (nxres.h's nxres_look_ping_targets) so an external style/theme/bg
   * change (nxres from a shell, or any other caller of SYS_SET_STYLE)
   * reaches this already-open window as INPUT_TYPE_LOOK_CHANGED instead of
   * only taking effect the next time nxfilem is launched.  Same pattern
   * nxsettings uses for "srv.settings_pid" (user-launched, not a singleton
   * — no respawn hazard to guard against; see init.c's register_service_pid
   * for the singleton-service equivalent).  Last-opened instance wins if
   * more than one is ever open at once. */
  {
    char pidbuf[16];
    snprintf(pidbuf, sizeof(pidbuf), "%d", get_pid());
    OS1_registry_set("srv.filem_pid", pidbuf);
  }

  fm_ui_load_theme(nxres_theme_is_light());
  return 0;
}

void fm_ui_reinit_window(int ww, int wh) {
  if (ww < FM_MIN_WIN_W)
    ww = FM_MIN_WIN_W;
  if (wh < FM_MIN_WIN_H)
    wh = FM_MIN_WIN_H;
  /* Keep the compositor's own window record in sync with the clamped size
   * (nxlauncher.c's reinit_window() does the same) -- without this the
   * compositor still thinks the window is the OLD size (stale w/h in
   * OS1_window_enum(), stale hit-test/clip rect) even though we already
   * reallocated our buffer to the new one. */
  if (fm_state.window_id >= 0)
    OS1_window_resize(fm_state.window_id, ww, wh);
  fm_gfx_realloc(ww, wh);
}

static unsigned g_sig = 0xFFFFFFFFu;

void fm_ui_redraw(int force) {
  compute_layout();

  unsigned sig = compute_signature();
  if (!force && sig == g_sig)
    return;
  g_sig = sig;

  g_btn_n = 0;
  fm_fill_rect(0, 0, fm_fb_w, fm_fb_h, g_col_bg);
  render_toolbar(); /* drawn before the menubar so the dropdown (menubar)
                     * paints on top of it, not the other way round */
  render_sidebar();
  render_content();
  render_statusbar();
  render_menubar();
  render_context_menu();

  fm_blit();
}
