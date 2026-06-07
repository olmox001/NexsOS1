/*
 * NeXs File Manager - UI Rendering
 * Menu, toolbar, sidebar, content area, and statusbar rendering
 */
#include "nexs-fm.h"

void fm_draw_menu(void) {
    /* Menu bar background */
    fm_draw_rect(0, 0, FM_WIN_W, FM_MENU_HEIGHT, FM_MENU_BG);
    
    /* File menu */
    fm_draw_text(8, 6, "File", FM_MENU_TEXT);
    
    /* Edit menu */
    fm_draw_text(60, 6, "Edit", FM_MENU_TEXT);
    
    /* View menu */
    fm_draw_text(110, 6, "View", FM_MENU_TEXT);
    
    /* Tools menu */
    fm_draw_text(160, 6, "Tools", FM_MENU_TEXT);
    
    /* Help menu */
    fm_draw_text(220, 6, "Help", FM_MENU_TEXT);
    
    /* Menu separator */
    fm_draw_rect(0, FM_MENU_HEIGHT - 1, FM_WIN_W, 1, FM_COLOR_SURFACE2);
}

void fm_draw_toolbar(void) {
    fm_draw_rect(0, FM_MENU_HEIGHT, FM_WIN_W, FM_TOOLBAR_HEIGHT, FM_COLOR_SURFACE);
    
    int btn_y = FM_MENU_HEIGHT + 8;
    int btn_x = 8;
    int btn_w = 40;
    int btn_h = 40;
    int spacing = 48;
    
    /* Back button */
    fm_draw_rect_outline(btn_x, btn_y, btn_w, btn_h, 
        fm_state.history_pos > 0 ? FM_COLOR_BLUE : FM_COLOR_SUBTEXT, 1);
    fm_draw_centered_text(btn_x, btn_y, btn_w, btn_h, "<", FM_COLOR_FG);
    
    /* Forward button */
    fm_draw_rect_outline(btn_x + spacing, btn_y, btn_w, btn_h,
        fm_state.history_pos < fm_state.history_count - 1 ? FM_COLOR_BLUE : FM_COLOR_SUBTEXT, 1);
    fm_draw_centered_text(btn_x + spacing, btn_y, btn_w, btn_h, ">", FM_COLOR_FG);
    
    /* Up button */
    fm_draw_rect_outline(btn_x + spacing * 2, btn_y, btn_w, btn_h, FM_COLOR_BLUE, 1);
    fm_draw_centered_text(btn_x + spacing * 2, btn_y, btn_w, btn_h, "^", FM_COLOR_FG);
    
    /* Home button */
    fm_draw_rect_outline(btn_x + spacing * 3, btn_y, btn_w, btn_h, FM_COLOR_BLUE, 1);
    fm_draw_centered_text(btn_x + spacing * 3, btn_y, btn_w, btn_h, "~", FM_COLOR_FG);
    
    /* Refresh button */
    fm_draw_rect_outline(btn_x + spacing * 4, btn_y, btn_w, btn_h, FM_COLOR_BLUE, 1);
    fm_draw_centered_text(btn_x + spacing * 4, btn_y, btn_w, btn_h, "R", FM_COLOR_FG);
    
    /* Separator */
    fm_draw_rect(0, FM_MENU_HEIGHT + FM_TOOLBAR_HEIGHT - 1, FM_WIN_W, 1, FM_COLOR_SURFACE2);
}

void fm_draw_sidebar(void) {
    if (!fm_state.show_sidebar) return;
    
    fm_draw_rect(0, FM_CONTENT_Y, FM_SIDEBAR_WIDTH, FM_CONTENT_H, FM_COLOR_SURFACE);
    
    int y = FM_CONTENT_Y + 12;
    
    /* Quick Access section */
    fm_draw_text(8, y, "Navigation", FM_COLOR_BLUE);
    y += 28;
    
    /* Home */
    if (strcmp(fm_state.current_path, fm_state.home_path) == 0) {
        fm_draw_text(16, y, "Home", FM_COLOR_CYAN);
    } else {
        fm_draw_text(16, y, "Home", FM_COLOR_FG);
    }
    y += 28;
    
    /* Root filesystem */
    if (strcmp(fm_state.current_path, "/") == 0) {
        fm_draw_text(16, y, "Root", FM_COLOR_CYAN);
    } else {
        fm_draw_text(16, y, "Root", FM_COLOR_FG);
    }
    y += 40;
    
    /* Directory Info */
    fm_draw_text(8, y, "Current Path", FM_COLOR_BLUE);
    y += 28;
    
    char path_short[FM_SIDEBAR_WIDTH - 20];
    strncpy(path_short, fm_state.current_path, FM_SIDEBAR_WIDTH - 22);
    path_short[FM_SIDEBAR_WIDTH - 22] = '\0';
    fm_draw_text(16, y, path_short, FM_COLOR_SUBTEXT);
    y += 28;
    
    char size_str[64];
    snprintf(size_str, sizeof(size_str), "Total: %ld KB", fm_state.total_size / 1024);
    fm_draw_text(16, y, size_str, FM_COLOR_FG);
    y += 20;
    
    if (fm_state.selected_count > 0) {
        snprintf(size_str, sizeof(size_str), "Sel: %d files", fm_state.selected_count);
        fm_draw_text(16, y, size_str, FM_COLOR_CYAN);
    }
    
    /* Sidebar separator */
    fm_draw_rect(FM_SIDEBAR_WIDTH - 1, FM_CONTENT_Y, 1, FM_CONTENT_H, FM_COLOR_SURFACE2);
}

void fm_draw_content(void) {
    fm_draw_rect(FM_CONTENT_X, FM_CONTENT_Y, FM_CONTENT_W, FM_CONTENT_H, FM_COLOR_BG);
    
    /* Path bar */
    fm_draw_rect(FM_CONTENT_X, FM_CONTENT_Y, FM_CONTENT_W, 36, FM_COLOR_SURFACE1);
    fm_draw_text(FM_CONTENT_X + 8, FM_CONTENT_Y + 10, fm_state.current_path, FM_COLOR_FG);
    
    int content_start_y = FM_CONTENT_Y + 36;
    int available_h = FM_CONTENT_H - 36;
    int items_per_page = available_h / FM_ITEM_HEIGHT;

    /* Larghezza scrollbar: deve stare DENTRO FM_CONTENT_W, non oltre FM_WIN_W */
    int scrollbar_w = 12;

    if (fm_state.file_count == 0) {
        fm_draw_text(FM_CONTENT_X + 20, content_start_y + 50, "No files", FM_COLOR_SUBTEXT);
        return;
    }
    
    for (int i = fm_state.scroll_offset; 
         i < fm_state.file_count && i - fm_state.scroll_offset < items_per_page; 
         i++) {
        
        int item_y = content_start_y + (i - fm_state.scroll_offset) * FM_ITEM_HEIGHT;
        fm_file_t *file = &fm_state.files[i];
        
        /* Item background */
        if (i == fm_state.highlighted_item) {
            fm_draw_rect(FM_CONTENT_X, item_y, FM_CONTENT_W, FM_ITEM_HEIGHT, FM_COLOR_SURFACE1);
        } else if (file->is_selected) {
            fm_draw_rect(FM_CONTENT_X, item_y, FM_CONTENT_W, FM_ITEM_HEIGHT, FM_COLOR_SURFACE2);
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
            snprintf(size_str, sizeof(size_str), "%ld MB", file->size / (1024 * 1024));
        }
        /* Sposta il testo dimensione a sinistra della scrollbar */
        fm_draw_text(FM_CONTENT_X + FM_CONTENT_W - 100 - scrollbar_w,
                     item_y + 10, size_str, FM_COLOR_SUBTEXT);
    }
    
    /* BUG FIX: l'originale passava FM_CONTENT_X + FM_CONTENT_W come x
       che equivale a FM_WIN_W (720), fuori dallo schermo.
       fm_draw_scrollbar usa (x - scrollbar_w) come origine, quindi
       dobbiamo passare FM_WIN_W in modo che il track inizi a FM_WIN_W - 12. */
    if (fm_state.file_count > items_per_page) {
        fm_draw_scrollbar(FM_WIN_W,
            content_start_y, available_h,
            fm_state.file_count, items_per_page, fm_state.scroll_offset);
    }
}

void fm_draw_statusbar(void) {
    fm_draw_rect(0, FM_WIN_H - FM_STATUSBAR_HEIGHT, FM_WIN_W, FM_STATUSBAR_HEIGHT, FM_COLOR_SURFACE);
    
    char status[128];
    snprintf(status, sizeof(status), "%d items | %d selected | Press ? for help", 
        fm_state.file_count, fm_state.selected_count);
    fm_draw_text(8, FM_WIN_H - FM_STATUSBAR_HEIGHT + 6, status, FM_COLOR_FG);
    
    /* Separator */
    fm_draw_rect(0, FM_WIN_H - FM_STATUSBAR_HEIGHT, FM_WIN_W, 1, FM_COLOR_SURFACE2);
}

void fm_draw_full_ui(void) {
    fm_draw_rect(0, 0, FM_WIN_W, FM_WIN_H, FM_COLOR_BG);
    
    fm_draw_menu();
    fm_draw_toolbar();
    fm_draw_sidebar();
    fm_draw_content();
    fm_draw_statusbar();
    
    compositor_render();
}

void fm_draw_context_menu(int x, int y) {
    int menu_w = 140;
    int menu_h = 150;
    
    if (x + menu_w > FM_WIN_W) x = FM_WIN_W - menu_w;
    if (y + menu_h > FM_WIN_H) y = FM_WIN_H - menu_h;
    
    fm_draw_rect(x, y, menu_w, menu_h, FM_COLOR_SURFACE);
    fm_draw_rect_outline(x, y, menu_w, menu_h, FM_COLOR_SURFACE2, 1);
    
    int item_h = 24;
    const char *labels[] = {"Open", "Copy", "Paste", "Rename", "Delete"};
    
    for (int i = 0; i < 5; i++) {
        fm_draw_text(x + 8, y + 6 + i * item_h, labels[i], FM_COLOR_FG);
    }
}
