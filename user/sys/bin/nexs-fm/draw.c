/*
 * NeXs File Manager - Drawing Primitives
 * Low-level graphics and text rendering
 */
#include "nexs-fm.h"

void fm_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > FM_WIN_W) w = FM_WIN_W - x;
    if (y + h > FM_WIN_H) h = FM_WIN_H - y;
    if (w <= 0 || h <= 0) return;
    
    window_draw(fm_state.window_id, x, y, w, h, color);
}

void fm_draw_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness) {
    /* Top */
    fm_draw_rect(x, y, w, thickness, color);
    /* Bottom */
    fm_draw_rect(x, y + h - thickness, w, thickness, color);
    /* Left */
    fm_draw_rect(x, y, thickness, h, color);
    /* Right */
    fm_draw_rect(x + w - thickness, y, thickness, h, color);
}

void fm_draw_text(int x, int y, const char *text, uint32_t color) {
    graphics_draw_text(fm_state.window_id, x, y, text, color);
}

void fm_draw_centered_text(int x, int y, int w, int h, const char *text, uint32_t color) {
    int text_len = strlen(text);
    int text_w = text_len * 8;
    int text_x = x + (w - text_w) / 2;
    int text_y = y + (h - 14) / 2;
    if (text_x < x) text_x = x;
    if (text_y < y) text_y = y;
    fm_draw_text(text_x, text_y, text, color);
}

void fm_draw_icon(int x, int y, int id, uint32_t color) {
    /* Simple icon drawing based on ID */
    switch (id) {
        case 0: /* Folder icon */
            fm_draw_rect(x, y + 6, 16, 10, color);
            fm_draw_rect(x, y + 4, 8, 2, color);
            break;
        case 1: /* File icon */
            fm_draw_rect(x, y, 12, 14, color);
            fm_draw_rect(x + 2, y + 2, 8, 10, FM_COLOR_BG);
            break;
        case 2: /* Executable icon */
            fm_draw_rect(x + 2, y + 2, 12, 10, color);
            fm_draw_rect(x + 4, y + 4, 8, 6, FM_COLOR_BG);
            break;
        case 3: /* Image icon */
            fm_draw_rect(x, y, 16, 14, color);
            fm_draw_rect(x + 1, y + 1, 14, 12, FM_COLOR_BG);
            break;
        case 4: /* Archive icon */
            fm_draw_rect(x, y, 14, 14, color);
            break;
    }
}

void fm_draw_file_icon(int x, int y, const fm_file_t *file) {
    if (file->is_dir) {
        fm_draw_icon(x, y, 0, FM_COLOR_YELLOW);
    } else {
        const char *ext = strrchr(file->name, '.');
        if (ext) {
            if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0 || strcmp(ext, ".S") == 0) {
                fm_draw_icon(x, y, 2, FM_COLOR_LAVENDER);
            } else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".png") == 0 || strcmp(ext, ".bmp") == 0) {
                fm_draw_icon(x, y, 3, FM_COLOR_CYAN);
            } else if (strcmp(ext, ".tar") == 0 || strcmp(ext, ".zip") == 0 || strcmp(ext, ".gz") == 0) {
                fm_draw_icon(x, y, 4, FM_COLOR_ORANGE);
            } else if (file->name[0] != '.') {
                /* BUG FIX: l'originale usava (file->size & 0x111) == 0
                   0x111 è 273 in decimale, NON una bitmask per i permessi eseguibili.
                   I bit eseguibili Unix sono 0111 in ottale (owner+group+other exec)
                   che in esadecimale è 0x49. Tuttavia 'size' non contiene i permessi:
                   serve un campo dedicato 'mode' nella struct fm_file_t.
                   Per ora usiamo file->icon_id come flag impostato al caricamento,
                   con fallback all'icona file generica. */
                if (file->icon_id == 2) {
                    fm_draw_icon(x, y, 2, FM_COLOR_GREEN);
                } else {
                    fm_draw_icon(x, y, 1, FM_COLOR_FG);
                }
            } else {
                fm_draw_icon(x, y, 1, FM_COLOR_FG);
            }
        } else {
            /* Nessuna estensione: usa icon_id se impostato, altrimenti file generico */
            if (file->icon_id == 2) {
                fm_draw_icon(x, y, 2, FM_COLOR_GREEN);
            } else {
                fm_draw_icon(x, y, 1, FM_COLOR_FG);
            }
        }
    }
}

void fm_draw_scrollbar(int x, int y, int h, int total, int visible, int offset) {
    if (total <= visible) return;
    
    int scrollbar_w = 12;
    int track_h = h;
    int thumb_h = (visible * track_h) / total;
    if (thumb_h < 20) thumb_h = 20;
    
    int thumb_y = y + (offset * (track_h - thumb_h)) / (total - visible);
    
    fm_draw_rect(x - scrollbar_w, y, scrollbar_w, track_h, FM_COLOR_SURFACE);
    fm_draw_rect(x - scrollbar_w, thumb_y, scrollbar_w, thumb_h, FM_COLOR_SURFACE2);
}
