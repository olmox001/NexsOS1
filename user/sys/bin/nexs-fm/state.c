/*
 * NeXs File Manager - State Management
 * Handles global application state, history, selection, and filters
 */
#include "nexs-fm.h"

fm_state_t fm_state = {0};

void fm_state_init(void) {
    fm_state.window_id = -1;
    fm_state.running = 1;
    fm_state.sort_mode = SORT_NAME;
    fm_state.sort_reverse = 0;
    fm_state.view_mode = VIEW_LIST;
    fm_state.show_hidden = 0;
    fm_state.show_sidebar = 1;
    fm_state.show_statusbar = 1;
    fm_state.menu_open = 0;
    fm_state.search_active = 0;
    fm_state.file_count = 0;
    fm_state.selected_count = 0;
    fm_state.scroll_offset = 0;
    fm_state.highlighted_item = 0;
    fm_state.clipboard.is_valid = 0;
    fm_state.menu_count = 0;
    fm_state.button_count = 0;
    fm_state.total_size = 0;
    fm_state.selected_size = 0;

    /*
     * BUG FIX (crash primario): os1.h dichiara getcwd come:
     *   int getcwd(char *buf, size_t size)  → ritorna 0 (ok) o -1 (errore)
     * Il codice originale non controllava il valore di ritorno.
     * Se il processo viene lanciato dal kernel senza CWD impostata,
     * getcwd fallisce, home_path rimane "" e list_dir("") crasha il sistema.
     * Soluzione: fallback a "/" se getcwd fallisce.
     */
    if (getcwd(fm_state.home_path, FM_PATH_MAX) != 0) {
        /* getcwd fallito: usa root come home di emergenza */
        fm_state.home_path[0] = '/';
        fm_state.home_path[1] = '\0';
    }
    /* Ulteriore guardia: se home_path è ancora vuoto per qualsiasi motivo */
    if (fm_state.home_path[0] == '\0') {
        fm_state.home_path[0] = '/';
        fm_state.home_path[1] = '\0';
    }

    strcpy(fm_state.current_path, fm_state.home_path);

    /* BUG FIX (history): inserisci il path iniziale come entry [0]
       così fm_navigate_back() dopo la prima navigazione torna al punto
       di partenza invece di saltare a una stringa vuota. */
    strcpy(fm_state.history[0], fm_state.home_path);
    fm_state.history_count = 1;
    fm_state.history_pos = 0;
}

void fm_state_add_to_history(const char *path) {
    /* Se stiamo navigando da una posizione intermedia, tronca il futuro */
    if (fm_state.history_pos < fm_state.history_count - 1) {
        fm_state.history_count = fm_state.history_pos + 1;
    }

    /* Non aggiungere lo stesso path due volte consecutive */
    if (fm_state.history_count > 0 &&
        strcmp(fm_state.history[fm_state.history_pos], path) == 0) {
        return;
    }

    /* Se il buffer è pieno, scorri di uno */
    if (fm_state.history_count >= FM_HISTORY_MAX) {
        int i;
        for (i = 0; i < fm_state.history_count - 1; i++) {
            strcpy(fm_state.history[i], fm_state.history[i + 1]);
        }
        fm_state.history_count--;
        if (fm_state.history_pos > 0) fm_state.history_pos--;
    }

    strcpy(fm_state.history[fm_state.history_count], path);
    fm_state.history_count++;
    fm_state.history_pos = fm_state.history_count - 1;
}

int fm_state_can_undo(void) {
    return fm_state.history_pos > 0;
}

int fm_state_can_redo(void) {
    return fm_state.history_pos < fm_state.history_count - 1;
}

void fm_state_sort_files(fm_sort_mode_t mode) {
    if (fm_state.sort_mode == mode) {
        fm_state.sort_reverse = !fm_state.sort_reverse;
    } else {
        fm_state.sort_mode = mode;
        fm_state.sort_reverse = 0;
    }
}

void fm_state_filter_files(void) {
    int visible_count = 0;

    for (int i = 0; i < fm_state.file_count; i++) {
        fm_file_t *f = &fm_state.files[i];

        if (!fm_state.show_hidden && f->is_hidden) {
            continue;
        }

        if (fm_state.search_active && strlen(fm_state.search_query) > 0) {
            if (strstr(f->name, fm_state.search_query) == NULL) {
                continue;
            }
        }

        visible_count++;
    }
    (void)visible_count; /* nexs-fm WIP: computed but not yet used; suppressed to
                          * keep the -Werror build green — wire it where intended. */
}

void fm_state_select_all(void) {
    fm_state.selected_count = 0;
    fm_state.selected_size = 0;

    for (int i = 0; i < fm_state.file_count; i++) {
        fm_state.files[i].is_selected = 1;
        fm_state.selected_count++;
        if (!fm_state.files[i].is_dir) {
            fm_state.selected_size += fm_state.files[i].size;
        }
    }
}

void fm_state_deselect_all(void) {
    for (int i = 0; i < fm_state.file_count; i++) {
        fm_state.files[i].is_selected = 0;
    }
    fm_state.selected_count = 0;
    fm_state.selected_size = 0;
}

void fm_state_toggle_select(int index) {
    if (index < 0 || index >= fm_state.file_count) {
        return;
    }

    fm_file_t *f = &fm_state.files[index];
    f->is_selected = !f->is_selected;

    if (f->is_selected) {
        fm_state.selected_count++;
        if (!f->is_dir) {
            fm_state.selected_size += f->size;
        }
    } else {
        fm_state.selected_count--;
        if (!f->is_dir) {
            fm_state.selected_size -= f->size;
        }
    }
}

void fm_state_update_sidebar(void) {
    fm_state.total_size = 0;
    for (int i = 0; i < fm_state.file_count; i++) {
        if (!fm_state.files[i].is_dir) {
            fm_state.total_size += fm_state.files[i].size;
        }
    }
}
