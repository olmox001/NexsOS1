/*
 * NeXs File Manager - Event Handling
 * Keyboard and mouse input processing
 */
#include "nxfilem.h"

/* Soglia double-click in millisecondi (clock_gettime CLOCK_MONOTONIC). */
#define FM_DBLCLICK_MS 400
static const int fm_top_menu_x[] = {8, 60, 110, 160, 220};
static const int fm_top_menu_width = 44;
static const int fm_top_menu_count = 5;
static const int fm_top_menu_item_h = 24;

static int fm_top_menu_index_at(int x, int y) {
  if (y < 0 || y >= FM_MENU_HEIGHT)
    return -1;
  for (int i = 0; i < fm_top_menu_count; i++) {
    if (x >= fm_top_menu_x[i] && x < fm_top_menu_x[i] + fm_top_menu_width)
      return i;
  }
  return -1;
}

static int fm_dropdown_item_at(int x, int y, int menu_id) {
  int menu_x = fm_top_menu_x[menu_id];
  int menu_y = FM_MENU_HEIGHT;
  int menu_w = 160;
  int menu_h;

  static const int menu_item_counts[] = {5, 5, 3, 1, 1};
  if (menu_id < 0 || menu_id >= fm_top_menu_count)
    return -1;

  if (menu_x + menu_w > FM_WIN_W)
    menu_x = FM_WIN_W - menu_w;

  menu_h = menu_item_counts[menu_id] * fm_top_menu_item_h;
  if (x < menu_x || x >= menu_x + menu_w || y < menu_y || y >= menu_y + menu_h)
    return -1;

  return (y - menu_y) / fm_top_menu_item_h;
}

static void fm_apply_top_menu_action(int menu_id, int item_id) {
  if (menu_id < 0 || item_id < 0)
    return;

  switch (menu_id) {
  case 0: /* File */
    switch (item_id) {
    case 0:
      if (fm_state.highlighted_item >= 0 && fm_state.highlighted_item < fm_state.file_count)
        fm_open_file(&fm_state.files[fm_state.highlighted_item]);
      break;
    case 1:
      if (fm_state.highlighted_item >= 0 && fm_state.highlighted_item < fm_state.file_count)
        fm_open_with_kilo(&fm_state.files[fm_state.highlighted_item]);
      break;
    case 2:
      fm_refresh_directory();
      break;
    case 3:
      if (fm_state.highlighted_item >= 0 && fm_state.highlighted_item < fm_state.file_count) {
        fm_delete_file(fm_state.files[fm_state.highlighted_item].full_path);
        fm_refresh_directory();
      }
      break;
    case 4:
      fm_state.running = 0;
      break;
    }
    break;
  case 1: /* Edit */
    switch (item_id) {
    case 0:
      fm_clipboard_copy();
      break;
    case 1:
      fm_clipboard_cut();
      break;
    case 2:
      fm_clipboard_paste();
      fm_refresh_directory();
      break;
    case 3:
      fm_state_select_all();
      break;
    case 4:
      fm_state_deselect_all();
      break;
    }
    break;
  case 2: /* View */
    switch (item_id) {
    case 0:
      fm_state.show_sidebar = !fm_state.show_sidebar;
      break;
    case 1:
      fm_state.show_statusbar = !fm_state.show_statusbar;
      break;
    case 2:
      fm_refresh_directory();
      break;
    }
    break;
  case 3: /* Tools */
    if (item_id == 0)
      fm_refresh_directory();
    break;
  case 4: /* Help */
    if (item_id == 0)
      fm_set_status_message("Help not implemented");
    break;
  }
}

static void fm_handle_top_menu_click(int x, int y) {
  int index = fm_top_menu_index_at(x, y);
  if (index < 0)
    return;

  if (fm_state.menu_open && fm_state.menu_id == index) {
    fm_state.menu_open = 0;
    fm_mark_dirty_menu();
    fm_mark_dirty_toolbar();
    fm_mark_dirty_sidebar();
    fm_mark_dirty_content();
  } else {
    fm_state.menu_open = 1;
    fm_state.menu_id = index;
    fm_mark_dirty_menu();
    fm_mark_dirty_toolbar();
    fm_mark_dirty_sidebar();
    fm_mark_dirty_content();
  }
}

/*
 * fm_handle_mouse_click - process a left-button press inside the window.
 *
 * Coord sono LOCALI alla finestra (il driver fornisce x/y di finestra
 * perché il click arriva dalla IPC del compositor, vedi
 * kernel/graphics/compositor.c::compositor_handle_click). Quindi tutti
 * i confronti con FM_*_OFFSET sono diretti.
 */
void fm_handle_mouse_click(int x, int y) {
  if (fm_state.menu_open) {
    int menu_x = fm_top_menu_x[fm_state.menu_id];
    int menu_w = 160;
    int menu_h;
    static const int menu_item_counts[] = {5, 5, 3, 1, 1};
    menu_h = menu_item_counts[fm_state.menu_id] * fm_top_menu_item_h;
    if (menu_x + menu_w > FM_WIN_W)
      menu_x = FM_WIN_W - menu_w;

    if (y >= FM_MENU_HEIGHT && y < FM_MENU_HEIGHT + menu_h &&
        x >= menu_x && x < menu_x + menu_w) {
      int item = fm_dropdown_item_at(x, y, fm_state.menu_id);
      if (item >= 0) {
        fm_apply_top_menu_action(fm_state.menu_id, item);
        fm_state.menu_open = 0;
        fm_mark_dirty_menu();
        fm_mark_dirty_toolbar();
        fm_mark_dirty_content();
        return;
      }
      /* Click inside the dropdown but outside any item closes it. */
      fm_state.menu_open = 0;
      fm_mark_dirty_menu();
      fm_mark_dirty_toolbar();
      fm_mark_dirty_content();
      return;
    }

    if (x < menu_x || x >= menu_x + menu_w || y < FM_MENU_HEIGHT ||
        y >= FM_MENU_HEIGHT + menu_h) {
      fm_state.menu_open = 0;
      fm_mark_dirty_menu();
      fm_mark_dirty_toolbar();
      fm_mark_dirty_content();
      /* Continue processing the click so toolbar/content can still respond. */
    }
  }

  /* Toolbar buttons */
  if (y >= FM_MENU_HEIGHT && y < FM_MENU_HEIGHT + FM_TOOLBAR_HEIGHT) {
    int btn_w = 40;
    int spacing = 48;
    int btn_y = FM_MENU_HEIGHT + 8;
    int btn_h = 40;

    if (y >= btn_y && y < btn_y + btn_h) {
      if (x >= 8 && x < 8 + btn_w) {
        if (fm_state.menu_open) {
          fm_state.menu_open = 0;
          fm_mark_dirty_menu();
          fm_mark_dirty_content();
          return;
        }
        if (fm_state_can_undo())
          fm_navigate_back();
        return;
      } else if (x >= 8 + spacing && x < 8 + spacing + btn_w) {
        if (fm_state.menu_open) {
          fm_state.menu_open = 0;
          fm_mark_dirty_menu();
          fm_mark_dirty_content();
          return;
        }
        if (fm_state_can_redo())
          fm_navigate_forward();
        return;
      } else if (x >= 8 + spacing * 2 && x < 8 + spacing * 2 + btn_w) {
        if (fm_state.menu_open) {
          fm_state.menu_open = 0;
          fm_mark_dirty_menu();
          fm_mark_dirty_content();
          return;
        }
        fm_navigate_up();
        return;
      } else if (x >= 8 + spacing * 3 && x < 8 + spacing * 3 + btn_w) {
        if (fm_state.menu_open) {
          fm_state.menu_open = 0;
          fm_mark_dirty_menu();
          fm_mark_dirty_content();
          return;
        }
        fm_navigate_home();
        return;
      } else if (x >= 8 + spacing * 4 && x < 8 + spacing * 4 + btn_w) {
        if (fm_state.menu_open) {
          fm_state.menu_open = 0;
          fm_mark_dirty_menu();
          fm_mark_dirty_content();
          return;
        }
        fm_refresh_directory();
        return;
      }
    }
  }

  /* Sidebar clicks (solo se abilitata) */
  if (fm_state.show_sidebar && x >= 0 && x < FM_SIDEBAR_WIDTH &&
      y >= FM_CONTENT_Y && y < FM_WIN_H - FM_STATUSBAR_HEIGHT) {
    /* Sidebar is now informative only; no clickable Home/Root targets. */
    return;
  }

  /* Menu bar (navigazione File/Edit/View/Tools/Help) */
  if (y >= 0 && y < FM_MENU_HEIGHT) {
    fm_handle_top_menu_click(x, y);
    return;
  }

  /* Content area clicks (path bar + lista file) */
  if (x >= FM_CONTENT_X && x < FM_WIN_W && y >= FM_CONTENT_Y) {
    /* Click sul path bar: nessuna azione (solo display) */
    if (y < FM_CONTENT_Y + 36) {
      return;
    }

    int item_y = y - (FM_CONTENT_Y + 36);
    int index = fm_state.scroll_offset + (item_y / FM_ITEM_HEIGHT);

    if (index >= 0 && index < fm_state.file_count) {
      fm_state.highlighted_item = index;
      fm_mark_dirty_content();

      /* Double-click detection con clock_gettime MONOTONIC. */
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long long now_ms =
          (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
      long long last_ms = (long long)fm_state.last_click_time;

      if ((now_ms - last_ms) < FM_DBLCLICK_MS &&
          fm_state.last_click_item == index && last_ms > 0) {
        /* Double click */
        fm_file_t *file = &fm_state.files[index];
        fm_open_file(file);
        fm_state.last_click_time = 0;
      } else {
        fm_state.last_click_time = now_ms;
        fm_state.last_click_item = index;
      }
    }
  }
}

void fm_handle_mouse_double_click(int x, int y) {
  if (x >= FM_CONTENT_X && x < FM_WIN_W && y >= FM_CONTENT_Y + 36) {
    int item_y = y - (FM_CONTENT_Y + 36);
    int index = fm_state.scroll_offset + (item_y / FM_ITEM_HEIGHT);

    if (index >= 0 && index < fm_state.file_count) {
      fm_file_t *file = &fm_state.files[index];
      fm_mark_dirty_content();
      fm_open_file(file);
    }
  }
}

void fm_handle_mouse_right_click(int x, int y) {
  /* Click destro sulla lista file: mostra menu contestuale.
   * NON resettare needs_redraw dopo il render del menu — altrimenti
   * qualsiasi evento successivo (mouse move, timer) non aggiorna lo
   * schermo e il menu resta "congelato" sopra lo sfondo vecchio. */
  if (x >= FM_CONTENT_X && x < FM_WIN_W && y >= FM_CONTENT_Y + 36 &&
      y < FM_WIN_H - FM_STATUSBAR_HEIGHT) {
    int item_y = y - (FM_CONTENT_Y + 36);
    int index = fm_state.scroll_offset + (item_y / FM_ITEM_HEIGHT);

    if (index >= 0 && index < fm_state.file_count) {
      fm_state.highlighted_item = index;
      fm_state.context_menu_active = 1;
      fm_state.context_menu_x = x;
      fm_state.context_menu_y = y;
      fm_mark_dirty_content();
      fm_mark_dirty_menu();
      fm_mark_dirty_context_menu();
    }
  }
}

static int fm_handle_context_menu_click(int x, int y) {
  if (!fm_state.context_menu_active)
    return 0;

  for (int i = 0; i < fm_state.menu_count; i++) {
    fm_menu_item_t *item = &fm_state.menus[i];
    if (x >= item->x && x < item->x + item->w && y >= item->y &&
        y < item->y + item->h) {
      if (!item->enabled)
        return 1;

      int index = fm_state.highlighted_item;
      if (index < 0 || index >= fm_state.file_count)
        return 1;

      switch (item->id) {
      case 0:
        fm_open_file(&fm_state.files[index]);
        break;
      case 1:
        fm_open_with_kilo(&fm_state.files[index]);
        break;
      case 2:
        fm_clipboard_copy();
        break;
      case 3:
        fm_clipboard_paste();
        break;
      case 4:
        /* Rename richiede un prompt di testo/UI non disponibile in questa
           versione del file manager. */
        break;
      case 5:
        fm_delete_file(fm_state.files[index].full_path);
        fm_refresh_directory();
        break;
      default:
        break;
      }
      return 1;
    }
  }
  return 0;
}

void fm_handle_mouse(input_event_t *event) {
  if (event->type != INPUT_TYPE_MOUSE)
    return;

  int x = event->mouse.x;
  int y = event->mouse.y;
  int button = event->mouse.button;
  int state = event->mouse.state;

  if (fm_state.context_menu_active && state == KEY_PRESSED) {
    if (button == MOUSE_BTN_LEFT) {
      fm_handle_context_menu_click(x, y);
      fm_state.context_menu_active = 0;
      fm_mark_dirty_content();
      fm_mark_dirty_menu();
      return;
    }
    if (button == MOUSE_BTN_RIGHT) {
      fm_state.context_menu_active = 0;
      fm_mark_dirty_content();
      fm_mark_dirty_menu();
      return;
    }
  }

  if (fm_state.menu_open && state == KEY_PRESSED && button == MOUSE_BTN_LEFT) {
    int menu_x = fm_top_menu_x[fm_state.menu_id];
    int menu_w = 160;
    int menu_h;
    static const int menu_item_counts[] = {5, 5, 3, 1, 1};
    menu_h = menu_item_counts[fm_state.menu_id] * fm_top_menu_item_h;
    if (menu_x + menu_w > FM_WIN_W)
      menu_x = FM_WIN_W - menu_w;
    if (x < menu_x || x >= menu_x + menu_w || y < FM_MENU_HEIGHT ||
        y >= FM_MENU_HEIGHT + menu_h) {
      fm_state.menu_open = 0;
      fm_mark_dirty_menu();
    }
  }

  /* `mouse.button` è il codice evdev BTN_* (0x110 sinistra, 0x111 destra)
   * passato dal compositor via IPC, NON un bitmask. */
  if (state == KEY_PRESSED) {
    if (button == MOUSE_BTN_LEFT) {
      fm_handle_mouse_click(x, y);
    } else if (button == MOUSE_BTN_RIGHT) {
      fm_handle_mouse_right_click(x, y);
    }
  }
}

void fm_handle_keyboard(input_event_t *event) {
  if (event->type != INPUT_TYPE_KEYBOARD)
    return;
  if (event->keyboard.state != KEY_PRESSED)
    return;

  /* Preferisci lo scancode evdev per le frecce (INPUT_KEY_UP/DOWN/...)
   * — il driver le emette così (vedi <input.h>). L'ASCII key è 0 per
   * questi tasti, quindi confrontare solo `key` non funziona. */
  unsigned char key = event->keyboard.key;
  uint16_t sc = event->keyboard.scancode;
  int arrow = (sc == INPUT_KEY_UP || sc == INPUT_KEY_DOWN ||
               sc == INPUT_KEY_LEFT || sc == INPUT_KEY_RIGHT);

  /* Frecce su/giù */
  if (arrow || key == 'w' || key == 's') {
    if (sc == INPUT_KEY_UP || key == 'w') {
      if (fm_state.highlighted_item > 0) {
        fm_state.highlighted_item--;
        if (fm_state.highlighted_item < fm_state.scroll_offset) {
          fm_state.scroll_offset = fm_state.highlighted_item;
        }
        fm_mark_dirty_content();
      }
    } else if (sc == INPUT_KEY_DOWN || key == 's') {
      if (fm_state.highlighted_item < fm_state.file_count - 1) {
        fm_state.highlighted_item++;
        int visible = FM_CONTENT_H / FM_ITEM_HEIGHT - 1;
        if (visible < 1)
          visible = 1;
        if (fm_state.highlighted_item >= fm_state.scroll_offset + visible) {
          fm_state.scroll_offset = fm_state.highlighted_item - visible + 1;
        }
        fm_mark_dirty_content();
      }
    }
    return;
  }

  /* Selezione con Space */
  if (key == ' ') {
    fm_state_toggle_select(fm_state.highlighted_item);
    return;
  }

  /* Comandi di navigazione */
  if (key == 'u' || sc == INPUT_KEY_BACKSPACE) {
    fm_navigate_up();
  } else if (key == 'h') {
    fm_navigate_home();
  } else if (key == 'r' || key == 'R') {
    fm_refresh_directory();
  }
  /* Operazioni file */
  else if (key == 'c' || key == 'C') {
    fm_clipboard_copy();
  } else if (key == 'x' || key == 'X') {
    fm_clipboard_cut();
  } else if (key == 'v' || key == 'V') {
    fm_clipboard_paste();
  } else if (key == 'd' || key == 'D') {
    if (fm_state.highlighted_item >= 0 &&
        fm_state.highlighted_item < fm_state.file_count) {
      fm_delete_file(fm_state.files[fm_state.highlighted_item].full_path);
      fm_refresh_directory();
    }
  }
  /* Selezione multipla: 'a' tutto, 'A' (shift+a) deseleziona. */
  else if (key == 'a') {
    fm_state_select_all();
  } else if (key == 'A') {
    fm_state_deselect_all();
  }
  /* Apri file / Enter */
  else if (key == '\r' || key == '\n' || key == 'o' || key == 'O' ||
           sc == INPUT_KEY_ENTER) {
    if (fm_state.highlighted_item >= 0 &&
        fm_state.highlighted_item < fm_state.file_count) {
      fm_file_t *file = &fm_state.files[fm_state.highlighted_item];
      fm_open_file(file);
    }
  }
  /* Sort */
  else if (key == '1') {
    fm_state_sort_files(SORT_NAME);
    fm_refresh_directory();
  } else if (key == '2') {
    fm_state_sort_files(SORT_SIZE);
    fm_refresh_directory();
  } else if (key == '3') {
    fm_state_sort_files(SORT_DATE);
    fm_refresh_directory();
  } else if (key == '4') {
    fm_state_sort_files(SORT_TYPE);
    fm_refresh_directory();
  }
  /* ESC chiude menu contestuale */
  else if (sc == INPUT_KEY_ESC || key == 27) {
    if (fm_state.context_menu_active) {
      fm_state.context_menu_active = 0;
      fm_mark_dirty_content();
      fm_mark_dirty_menu();
    }
  }
  /* Quit */
  else if (key == 'q' || key == 'Q') {
    fm_state.running = 0;
  }
}
