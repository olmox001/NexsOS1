/*
 * NeXs File Manager - Event Handling
 * Keyboard and mouse input processing
 */
#include "nexs-fm.h"

void fm_handle_mouse_click(int x, int y) {
    /* Toolbar buttons */
    if (y >= FM_MENU_HEIGHT && y < FM_MENU_HEIGHT + FM_TOOLBAR_HEIGHT) {
        int btn_w = 40;
        int spacing = 48;
        
        if (x >= 8 && x < 8 + btn_w && fm_state_can_undo()) {
            fm_navigate_back();
            return;
        } else if (x >= 8 + spacing && x < 8 + spacing + btn_w && fm_state_can_redo()) {
            fm_navigate_forward();
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
    
    /* Sidebar clicks */
    if (x >= 0 && x < FM_SIDEBAR_WIDTH && y >= FM_CONTENT_Y && y < FM_WIN_H - FM_STATUSBAR_HEIGHT) {
        int rel_y = y - FM_CONTENT_Y - 12;
        if (rel_y > 0 && rel_y < 28) {
            /* Quick Access header - no action */
        } else if (rel_y > 28 && rel_y < 56) {
            fm_navigate_home();
        } else if (rel_y > 56 && rel_y < 84) {
            fm_navigate_to("/");
        }
        return;
    }
    
    /* Content area clicks */
    if (x >= FM_CONTENT_X && x < FM_WIN_W && y >= FM_CONTENT_Y + 36) {
        int item_y = y - (FM_CONTENT_Y + 36);
        int index = fm_state.scroll_offset + (item_y / FM_ITEM_HEIGHT);
        
        if (index >= 0 && index < fm_state.file_count) {
            fm_state.highlighted_item = index;
            
            long current_time = get_time();
            if (current_time - fm_state.last_click_time < 300 && 
                fm_state.last_click_item == index) {
                /* Double click */
                fm_file_t *file = &fm_state.files[index];
                if (file->is_dir) {
                    fm_navigate_to(file->full_path);
                } else {
                    spawn(file->full_path);
                }
                fm_state.last_click_time = 0;
            } else {
                fm_state.last_click_time = current_time;
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
            if (file->is_dir) {
                fm_navigate_to(file->full_path);
            } else {
                spawn(file->full_path);
            }
        }
    }
}

void fm_handle_mouse_right_click(int x, int y) {
    if (x >= FM_CONTENT_X && x < FM_WIN_W && y >= FM_CONTENT_Y + 36) {
        int item_y = y - (FM_CONTENT_Y + 36);
        int index = fm_state.scroll_offset + (item_y / FM_ITEM_HEIGHT);
        
        if (index >= 0 && index < fm_state.file_count) {
            fm_state.highlighted_item = index;
            fm_draw_context_menu(x, y);
        }
    }
}

void fm_handle_mouse(input_event_t *event) {
    if (event->type != INPUT_TYPE_MOUSE) return;
    
    int x = event->mouse.x;
    int y = event->mouse.y;
    int button = event->mouse.button;
    
    if (event->mouse.state == KEY_PRESSED) {
        if (button == 1) {
            fm_handle_mouse_click(x, y);
        } else if (button == 3) {
            fm_handle_mouse_right_click(x, y);
        }
    }
}

void fm_handle_keyboard(input_event_t *event) {
    if (event->type != INPUT_TYPE_KEYBOARD) return;
    if (event->keyboard.state != KEY_PRESSED) return;

    unsigned char key = event->keyboard.key;

    /* Navigation - frecce su/giù via codici escape (0x41=Up, 0x42=Down) */
    /* BUG FIX: originale usava 'A' e 'B' come tasti freccia, ma 'A' confligge
       con "Deselect all" e 'B' è un carattere valido.
       I tasti freccia ANSI mandano sequenze ESC [ A / ESC [ B; qui assumiamo
       che il sistema li converta in codici < 0x20 o in valori dedicati.
       Usiamo KEY_UP / KEY_DOWN se definiti dall'OS, altrimenti 0x41/0x42
       che però non devono più collidere con tasti alfabetici. */
    if (key == KEY_UP || key == 'w') {
        if (fm_state.highlighted_item > 0) {
            fm_state.highlighted_item--;
            if (fm_state.highlighted_item < fm_state.scroll_offset) {
                fm_state.scroll_offset = fm_state.highlighted_item;
            }
        }
    } else if (key == KEY_DOWN || key == 's') {
        if (fm_state.highlighted_item < fm_state.file_count - 1) {
            fm_state.highlighted_item++;
            int visible = FM_CONTENT_H / FM_ITEM_HEIGHT - 1;
            if (fm_state.highlighted_item >= fm_state.scroll_offset + visible) {
                fm_state.scroll_offset = fm_state.highlighted_item - visible + 1;
            }
        }
    }
    /* Selection */
    else if (key == ' ') {
        fm_state_toggle_select(fm_state.highlighted_item);
    }
    /* Navigation commands */
    else if (key == 'u') {
        fm_navigate_up();
    } else if (key == 'h') {
        fm_navigate_home();
    } else if (key == 'r') {
        fm_refresh_directory();
    }
    /* File operations */
    else if (key == 'c') {
        fm_clipboard_copy();
    } else if (key == 'x') {
        fm_clipboard_cut();
    } else if (key == 'v') {
        fm_clipboard_paste();
    } else if (key == 'd') {
        if (fm_state.highlighted_item >= 0 && fm_state.highlighted_item < fm_state.file_count) {
            fm_delete_file(fm_state.files[fm_state.highlighted_item].full_path);
            fm_refresh_directory();
        }
    }
    /* Selection commands - BUG FIX: 'a' select all, 'A' (shift+a) deselect all.
       Erano entrambi presenti ma 'A' veniva catturato dal ramo freccia su. */
    else if (key == 'a') {
        fm_state_select_all();
    } else if (key == 'A') {
        fm_state_deselect_all();
    }
    /* Open file */
    else if (key == '\r' || key == 'o') {
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
    /* Quit */
    else if (key == 'q') {
        fm_state.running = 0;
    }
}
