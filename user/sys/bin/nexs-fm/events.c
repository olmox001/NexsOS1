/*
 * NeXs File Manager - Event Handling
 * Keyboard and mouse input processing
 */
#include "nexs-fm.h"

/* Soglia double-click in millisecondi (clock_gettime CLOCK_MONOTONIC). */
#define FM_DBLCLICK_MS 400

/*
 * fm_handle_mouse_click - process a left-button press inside the window.
 *
 * Coord sono LOCALI alla finestra (il driver fornisce x/y di finestra
 * perché il click arriva dalla IPC del compositor, vedi
 * kernel/graphics/compositor.c::compositor_handle_click). Quindi tutti
 * i confronti con FM_*_OFFSET sono diretti.
 */
void fm_handle_mouse_click(int x, int y) {
    /* Toolbar buttons */
    if (y >= FM_MENU_HEIGHT && y < FM_MENU_HEIGHT + FM_TOOLBAR_HEIGHT) {
        int btn_w = 40;
        int spacing = 48;
        int btn_y = FM_MENU_HEIGHT + 8;
        int btn_h = 40;

        if (y >= btn_y && y < btn_y + btn_h) {
            if (x >= 8 && x < 8 + btn_w) {
                if (fm_state_can_undo()) fm_navigate_back();
                return;
            } else if (x >= 8 + spacing && x < 8 + spacing + btn_w) {
                if (fm_state_can_redo()) fm_navigate_forward();
                return;
            } else if (x >= 8 + spacing * 2 && x < 8 + spacing * 2 + btn_w) {
                fm_navigate_up();
                return;
            } else if (x >= 8 + spacing * 3 && x < 8 + spacing * 3 + btn_w) {
                fm_navigate_home();
                return;
            } else if (x >= 8 + spacing * 4 && x < 8 + spacing * 4 + btn_w) {
                fm_refresh_directory();
                return;
            }
        }
    }

    /* Sidebar clicks (solo se abilitata) */
    if (fm_state.show_sidebar &&
        x >= 0 && x < FM_SIDEBAR_WIDTH &&
        y >= FM_CONTENT_Y && y < FM_WIN_H - FM_STATUSBAR_HEIGHT) {
        /* Layout (vedi ui.c::fm_draw_sidebar):
         *   FM_CONTENT_Y + 12  : header "Navigation"
         *   +28                : "Home"
         *   +28                : "Root"                                   */
        int rel_y = y - FM_CONTENT_Y;
        if (rel_y >= 12 + 28 && rel_y < 12 + 56) {
            fm_navigate_home();
            return;
        } else if (rel_y >= 12 + 56 && rel_y < 12 + 84) {
            fm_navigate_to("/");
            return;
        }
    }

    /* Menu bar (navigazione File/Edit/View/Tools/Help) */
    if (y >= 0 && y < FM_MENU_HEIGHT) {
        /* Per ora nessuna azione: i menu sono solo renderizzati, non aperti. */
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
            fm_state.needs_redraw = 1;

            /* Double-click detection con clock_gettime MONOTONIC. */
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long now_ms = (long long)now.tv_sec * 1000LL
                             + now.tv_nsec / 1000000LL;
            long long last_ms = (long long)fm_state.last_click_time;

            if ((now_ms - last_ms) < FM_DBLCLICK_MS &&
                fm_state.last_click_item == index && last_ms > 0) {
                /* Double click */
                fm_file_t *file = &fm_state.files[index];
                if (file->is_dir) {
                    fm_navigate_to(file->full_path);
                } else {
                    spawn(file->full_path);
                }
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
            fm_state.needs_redraw = 1;
            if (file->is_dir) {
                fm_navigate_to(file->full_path);
            } else {
                spawn(file->full_path);
            }
        }
    }
}

void fm_handle_mouse_right_click(int x, int y) {
    /* Click destro sulla lista file: mostra menu contestuale.
     * NON resettare needs_redraw dopo il render del menu — altrimenti
     * qualsiasi evento successivo (mouse move, timer) non aggiorna lo
     * schermo e il menu resta "congelato" sopra lo sfondo vecchio. */
    if (x >= FM_CONTENT_X && x < FM_WIN_W &&
        y >= FM_CONTENT_Y + 36 && y < FM_WIN_H - FM_STATUSBAR_HEIGHT) {
        int item_y = y - (FM_CONTENT_Y + 36);
        int index = fm_state.scroll_offset + (item_y / FM_ITEM_HEIGHT);

        if (index >= 0 && index < fm_state.file_count) {
            fm_state.highlighted_item = index;
            fm_state.context_menu_active = 1;
            fm_state.context_menu_x = x;
            fm_state.context_menu_y = y;
            fm_state.needs_redraw = 1;
        }
    }
}

void fm_handle_mouse(input_event_t *event) {
    if (event->type != INPUT_TYPE_MOUSE) return;

    int x = event->mouse.x;
    int y = event->mouse.y;
    int button = event->mouse.button;
    int state = event->mouse.state;

    /* Se il menu contestuale è aperto, qualsiasi click lo chiude
     * e lascia che il gestore "normale" processi l'azione. */
    if (fm_state.context_menu_active && state == KEY_PRESSED) {
        fm_state.context_menu_active = 0;
        fm_state.needs_redraw = 1;
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
    if (event->type != INPUT_TYPE_KEYBOARD) return;
    if (event->keyboard.state != KEY_PRESSED) return;

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
                fm_state.needs_redraw = 1;
            }
        } else if (sc == INPUT_KEY_DOWN || key == 's') {
            if (fm_state.highlighted_item < fm_state.file_count - 1) {
                fm_state.highlighted_item++;
                int visible = FM_CONTENT_H / FM_ITEM_HEIGHT - 1;
                if (visible < 1) visible = 1;
                if (fm_state.highlighted_item >= fm_state.scroll_offset + visible) {
                    fm_state.scroll_offset = fm_state.highlighted_item - visible + 1;
                }
                fm_state.needs_redraw = 1;
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
        if (fm_state.highlighted_item >= 0 && fm_state.highlighted_item < fm_state.file_count) {
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
        if (fm_state.highlighted_item >= 0 && fm_state.highlighted_item < fm_state.file_count) {
            fm_file_t *file = &fm_state.files[fm_state.highlighted_item];
            if (file->is_dir) {
                fm_navigate_to(file->full_path);
            } else {
                spawn(file->full_path);
            }
        }
    }
    /* Sort */
    else if (key == '1') { fm_state_sort_files(SORT_NAME);  fm_refresh_directory(); }
    else if (key == '2') { fm_state_sort_files(SORT_SIZE);  fm_refresh_directory(); }
    else if (key == '3') { fm_state_sort_files(SORT_DATE);  fm_refresh_directory(); }
    else if (key == '4') { fm_state_sort_files(SORT_TYPE);  fm_refresh_directory(); }
    /* ESC chiude menu contestuale */
    else if (sc == INPUT_KEY_ESC || key == 27) {
        if (fm_state.context_menu_active) {
            fm_state.context_menu_active = 0;
            fm_state.needs_redraw = 1;
        }
    }
    /* Quit */
    else if (key == 'q' || key == 'Q') {
        fm_state.running = 0;
    }
}
