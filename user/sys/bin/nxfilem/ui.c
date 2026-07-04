/*
 * NeXs File Manager - UI Rendering
 * Menu, toolbar, sidebar, content area, and statusbar rendering
 */
#include "nxfilem.h"

static const char *top_menu_labels[] = {"File", "Edit", "View", "Tools", "Help"};
static const int top_menu_x[] = {8, 60, 110, 160, 220};
static const int top_menu_width = 44;
static const int top_menu_count = 5;

static void fm_draw_menu_dropdown(void) {
  if (!fm_state.menu_open || fm_state.menu_id < 0 || fm_state.menu_id >= top_menu_count)
    return;

  static const char *const menu_items[][7] = {
      {"Open", "Open in Kilo", "Refresh", "Delete", "Quit", NULL},
      {"Copy", "Cut", "Paste", "Select All", "Deselect All", NULL},
      {"Toggle Sidebar", "Toggle Statusbar", "Refresh", NULL},
      {"Refresh", NULL},
      {"About", NULL}};
  static const int menu_item_counts[] = {5, 5, 3, 1, 1};

  int x = top_menu_x[fm_state.menu_id];
  int y = FM_MENU_HEIGHT;
  int item_h = 24;
  int menu_w = 160;
  int count = menu_item_counts[fm_state.menu_id];
  int menu_h = count * item_h;

  if (x + menu_w > FM_WIN_W)
    x = FM_WIN_W - menu_w;
  if (y + menu_h > FM_WIN_H)
    y = FM_WIN_H - menu_h;

  fm_draw_rect(x, y, menu_w, menu_h, FM_COLOR_SURFACE);
  fm_draw_rect_outline(x, y, menu_w, menu_h, FM_COLOR_SURFACE2, 1);

  fm_state.menu_count = count;
  for (int i = 0; i < count; i++) {
    fm_menu_item_t *item = &fm_state.menus[i];
    const char *label = menu_items[fm_state.menu_id][i];
    strncpy(item->label, label, sizeof(item->label) - 1);
    item->label[sizeof(item->label) - 1] = '\0';
    item->x = x;
    item->y = y + i * item_h;
    item->w = menu_w;
    item->h = item_h;
    item->id = i;
    item->has_submenu = 0;
    item->enabled = 1;
    fm_draw_text(x + 8, item->y + 6, item->label,
                 item->enabled ? FM_COLOR_FG : FM_COLOR_SUBTEXT);
  }
}

void fm_draw_menu(void) {
  /* Menu bar background */
  fm_draw_rect(0, 0, FM_WIN_W, FM_MENU_HEIGHT, FM_MENU_BG);

  for (int i = 0; i < top_menu_count; i++) {
    uint32_t color = FM_MENU_TEXT;
    if (fm_state.menu_open && fm_state.menu_id == i) {
      int x = top_menu_x[i];
      fm_draw_rect(x - 4, 0, top_menu_width, FM_MENU_HEIGHT, FM_COLOR_SURFACE1);
      color = FM_COLOR_CYAN;
    }
    fm_draw_text(top_menu_x[i], 6, top_menu_labels[i], color);
  }

  /* Menu separator */
  fm_draw_rect(0, FM_MENU_HEIGHT - 1, FM_WIN_W, 1, FM_COLOR_SURFACE2);
}

void fm_draw_toolbar(void) {
  fm_draw_rect(0, FM_MENU_HEIGHT, FM_WIN_W, FM_TOOLBAR_HEIGHT,
               FM_COLOR_SURFACE);

  int btn_y = FM_MENU_HEIGHT + 8;
  int btn_x = 8;
  int btn_w = 40;
  int btn_h = 40;
  int spacing = 48;

  /* Back button */
  fm_draw_rect_outline(
      btn_x, btn_y, btn_w, btn_h,
      fm_state.history_pos > 0 ? FM_COLOR_BLUE : FM_COLOR_SUBTEXT, 1);
  fm_draw_centered_text(btn_x, btn_y, btn_w, btn_h, "<", FM_COLOR_FG);

  /* Forward button */
  fm_draw_rect_outline(btn_x + spacing, btn_y, btn_w, btn_h,
                       fm_state.history_pos < fm_state.history_count - 1
                           ? FM_COLOR_BLUE
                           : FM_COLOR_SUBTEXT,
                       1);
  fm_draw_centered_text(btn_x + spacing, btn_y, btn_w, btn_h, ">", FM_COLOR_FG);

  /* Up button */
  fm_draw_rect_outline(btn_x + spacing * 2, btn_y, btn_w, btn_h, FM_COLOR_BLUE,
                       1);
  fm_draw_centered_text(btn_x + spacing * 2, btn_y, btn_w, btn_h, "^",
                        FM_COLOR_FG);

  /* Refresh button */
  fm_draw_rect_outline(btn_x + spacing * 4, btn_y, btn_w, btn_h, FM_COLOR_BLUE,
                       1);
  fm_draw_centered_text(btn_x + spacing * 4, btn_y, btn_w, btn_h, "R",
                        FM_COLOR_FG);

  /* Separator */
  fm_draw_rect(0, FM_MENU_HEIGHT + FM_TOOLBAR_HEIGHT - 1, FM_WIN_W, 1,
               FM_COLOR_SURFACE2);
}

void fm_draw_sidebar(void) {
  if (!fm_state.show_sidebar)
    return;

  fm_draw_rect(0, FM_CONTENT_Y, FM_SIDEBAR_WIDTH, FM_CONTENT_H,
               FM_COLOR_SURFACE);

  int y = FM_CONTENT_Y + 12;

  /* Quick Access section */
  fm_draw_text(8, y, "Navigation", FM_COLOR_BLUE);
  y += 28;

  /* Directory Info */
  fm_draw_text(8, y, "Current Path", FM_COLOR_BLUE);
  y += 28;

  char path_short[FM_SIDEBAR_WIDTH - 20];
  strncpy(path_short, fm_state.current_path, FM_SIDEBAR_WIDTH - 22);
  path_short[FM_SIDEBAR_WIDTH - 22] = '\0';
  fm_draw_text(16, y, path_short, FM_COLOR_SUBTEXT);
  y += 28;

  char size_str[64];
  snprintf(size_str, sizeof(size_str), "Total: %ld KB",
           fm_state.total_size / 1024);
  fm_draw_text(16, y, size_str, FM_COLOR_FG);
  y += 20;

  if (fm_state.selected_count > 0) {
    snprintf(size_str, sizeof(size_str), "Sel: %d files",
             fm_state.selected_count);
    fm_draw_text(16, y, size_str, FM_COLOR_CYAN);
  }

  /* Sidebar separator */
  fm_draw_rect(FM_SIDEBAR_WIDTH - 1, FM_CONTENT_Y, 1, FM_CONTENT_H,
               FM_COLOR_SURFACE2);
}

void fm_draw_content(void) {
  fm_draw_rect(FM_CONTENT_X, FM_CONTENT_Y, FM_CONTENT_W, FM_CONTENT_H,
               FM_COLOR_BG);

  /* Path bar */
  fm_draw_rect(FM_CONTENT_X, FM_CONTENT_Y, FM_CONTENT_W, 36, FM_COLOR_SURFACE1);
  fm_draw_text(FM_CONTENT_X + 8, FM_CONTENT_Y + 10, fm_state.current_path,
               FM_COLOR_FG);

  int content_start_y = FM_CONTENT_Y + 36;
  int available_h = FM_CONTENT_H - 36;
  int items_per_page = available_h / FM_ITEM_HEIGHT;

  /* Larghezza scrollbar: deve stare DENTRO FM_CONTENT_W, non oltre FM_WIN_W */
  int scrollbar_w = 12;

  if (fm_state.file_count == 0) {
    fm_draw_text(FM_CONTENT_X + 20, content_start_y + 50, "No files",
                 FM_COLOR_SUBTEXT);
    return;
  }

  for (int i = fm_state.scroll_offset;
       i < fm_state.file_count && i - fm_state.scroll_offset < items_per_page;
       i++) {

    int item_y =
        content_start_y + (i - fm_state.scroll_offset) * FM_ITEM_HEIGHT;
    fm_file_t *file = &fm_state.files[i];

    /* Item background */
    if (i == fm_state.highlighted_item) {
      fm_draw_rect(FM_CONTENT_X, item_y, FM_CONTENT_W, FM_ITEM_HEIGHT,
                   FM_COLOR_SURFACE1);
    } else if (file->is_selected) {
      fm_draw_rect(FM_CONTENT_X, item_y, FM_CONTENT_W, FM_ITEM_HEIGHT,
                   FM_COLOR_SURFACE2);
    }

    /* Icon */
    fm_draw_file_icon(FM_CONTENT_X + 8, item_y + 6, file);

    /* Filename */
    fm_draw_text(FM_CONTENT_X + 40, item_y + 10, file->name, FM_COLOR_FG);

    /* File size */
    char size_str[32];
    if (file->is_dir) {
      snprintf(size_str, sizeof(size_str), "DIR");
    } else if (file->size < 1024) {
      snprintf(size_str, sizeof(size_str), "%ld B", file->size);
    } else if (file->size < 1024 * 1024) {
      snprintf(size_str, sizeof(size_str), "%ld KB", file->size / 1024);
    } else {
      snprintf(size_str, sizeof(size_str), "%ld MB",
               file->size / (1024 * 1024));
    }
    /* Sposta il testo dimensione a sinistra della scrollbar */
    fm_draw_text(FM_CONTENT_X + FM_CONTENT_W - 100 - scrollbar_w, item_y + 10,
                 size_str, FM_COLOR_SUBTEXT);
  }

  if (fm_state.file_count > items_per_page) {
    fm_draw_scrollbar(FM_CONTENT_X + FM_CONTENT_W, content_start_y,
                      available_h, fm_state.file_count, items_per_page,
                      fm_state.scroll_offset);
  }
}

void fm_draw_statusbar(void) {
  fm_draw_rect(0, FM_WIN_H - FM_STATUSBAR_HEIGHT, FM_WIN_W, FM_STATUSBAR_HEIGHT,
               FM_COLOR_SURFACE);

  char status[128];
  if (fm_state.status_message[0]) {
    snprintf(status, sizeof(status), "%s", fm_state.status_message);
  } else {
    snprintf(status, sizeof(status), "%d items | %d selected | Press ? for help",
             fm_state.file_count, fm_state.selected_count);
  }
  fm_draw_text(8, FM_WIN_H - FM_STATUSBAR_HEIGHT + 6, status, FM_COLOR_FG);

  /* Separator */
  fm_draw_rect(0, FM_WIN_H - FM_STATUSBAR_HEIGHT, FM_WIN_W, 1,
               FM_COLOR_SURFACE2);
}

static void fm_draw_full_ui(void) {
  fm_draw_rect(0, 0, FM_WIN_W, FM_WIN_H, FM_COLOR_BG);

  fm_draw_menu();
  fm_draw_toolbar();
  fm_draw_sidebar();
  fm_draw_content();
  fm_draw_statusbar();

  if (fm_state.menu_open) {
    fm_draw_menu_dropdown();
  }
  if (fm_state.context_menu_active) {
    fm_draw_context_menu(fm_state.context_menu_x, fm_state.context_menu_y);
  }
}

void fm_mark_dirty_all(void) {
  fm_state.needs_redraw = 1;
  fm_state.dirty_all = 1;
}

void fm_mark_dirty_menu(void) {
  fm_state.needs_redraw = 1;
  fm_state.dirty_menu = 1;
}

void fm_mark_dirty_toolbar(void) {
  fm_state.needs_redraw = 1;
  fm_state.dirty_toolbar = 1;
}

void fm_mark_dirty_sidebar(void) {
  fm_state.needs_redraw = 1;
  fm_state.dirty_sidebar = 1;
}

void fm_mark_dirty_content(void) {
  fm_state.needs_redraw = 1;
  fm_state.dirty_content = 1;
}

void fm_mark_dirty_statusbar(void) {
  fm_state.needs_redraw = 1;
  fm_state.dirty_statusbar = 1;
}

void fm_mark_dirty_context_menu(void) {
  fm_state.needs_redraw = 1;
  fm_state.dirty_context_menu = 1;
}

void fm_render_dirty_ui(void) {
  if (fm_state.dirty_all) {
    fm_draw_full_ui();
  } else {
    if (fm_state.dirty_menu) {
      fm_draw_menu();
      if (fm_state.menu_open) {
        fm_draw_menu_dropdown();
      }
    }
    if (fm_state.dirty_toolbar) {
      fm_draw_toolbar();
    }
    if (fm_state.dirty_sidebar) {
      fm_draw_sidebar();
    }
    if (fm_state.dirty_content) {
      fm_draw_content();
    }
    if (fm_state.dirty_statusbar) {
      fm_draw_statusbar();
    }
    if (fm_state.dirty_context_menu) {
      if (fm_state.context_menu_active) {
        fm_draw_context_menu(fm_state.context_menu_x, fm_state.context_menu_y);
      } else {
        /* If the context menu was dismissed, redraw the entire window so the
           stale context menu graphics are cleared. */
        fm_draw_rect(0, 0, FM_WIN_W, FM_WIN_H, FM_COLOR_BG);
        fm_draw_menu();
        fm_draw_toolbar();
        fm_draw_sidebar();
        fm_draw_content();
        fm_draw_statusbar();
      }
    }
  }
  compositor_render();
}

void fm_draw_context_menu(int x, int y) {
  int menu_w = 140;
  int item_h = 24;
  int menu_h = item_h * 5;

  if (x + menu_w > FM_WIN_W)
    x = FM_WIN_W - menu_w;
  if (y + menu_h > FM_WIN_H)
    y = FM_WIN_H - menu_h;

  fm_draw_rect(x, y, menu_w, menu_h, FM_COLOR_SURFACE);
  fm_draw_rect_outline(x, y, menu_w, menu_h, FM_COLOR_SURFACE2, 1);

  const char *labels[] = {"Open", "Open in Kilo", "Copy", "Paste", "Rename", "Delete"};

  fm_state.menu_count = 6;
  for (int i = 0; i < fm_state.menu_count; i++) {
    fm_menu_item_t *item = &fm_state.menus[i];
    strncpy(item->label, labels[i], sizeof(item->label) - 1);
    item->label[sizeof(item->label) - 1] = '\0';
    item->x = x;
    item->y = y + i * item_h;
    item->w = menu_w;
    item->h = item_h;
    item->id = i;
    item->has_submenu = 0;
    item->enabled = 1;

    if ((i == 2 && !fm_state.clipboard.is_valid) ||
        (i == 1 && fm_state.files[fm_state.highlighted_item].is_dir)) {
      item->enabled = 0;
    }

    uint32_t text_color = item->enabled ? FM_COLOR_FG : FM_COLOR_SUBTEXT;
    fm_draw_text(x + 8, item->y + 6, item->label, text_color);
  }
}
