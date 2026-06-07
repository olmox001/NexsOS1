/*
 * NeXs File Manager - File Operations
 * Directory navigation, file manipulation, clipboard operations
 */
#include "nexs-fm.h"

void fm_refresh_directory(void) {
    static char buf[16384];
    int len = list_dir(fm_state.current_path, buf, sizeof(buf));

    fm_state.file_count = 0;
    fm_state.selected_count = 0;
    fm_state.total_size = 0;

    if (len < 0) {
        return;
    }

    char *token = buf;
    while (*token != '\0' && fm_state.file_count < FM_MAX_FILES) {
        char *newline = strchr(token, '\n');
        if (!newline) break;
        *newline = '\0';

        if (strlen(token) == 0) {
            token = newline + 1;
            continue;
        }

        fm_file_t *file = &fm_state.files[fm_state.file_count];

        int is_dir = 0;
        int name_len = strlen(token);

        if (name_len > 0 && token[name_len - 1] == '/') {
            is_dir = 1;
            token[name_len - 1] = '\0';
        }

        strncpy(file->name, token, FM_NAME_MAX - 1);
        file->name[FM_NAME_MAX - 1] = '\0';

        /* Costruisci full_path evitando doppio slash quando current_path è "/" */
        if (fm_state.current_path[0] == '/' && fm_state.current_path[1] == '\0') {
            snprintf(file->full_path, FM_PATH_MAX, "/%s", file->name);
        } else {
            snprintf(file->full_path, FM_PATH_MAX, "%s/%s",
                     fm_state.current_path, file->name);
        }

        file->is_dir = is_dir;
        file->is_hidden = (file->name[0] == '.');
        file->is_selected = 0;
        file->size = is_dir ? 0 : fm_get_file_size(file->full_path);
        file->mtime = fm_get_file_mtime(file->full_path);
        file->icon_id = 1; /* default: file generico */

        if (!file->is_dir) {
            fm_state.total_size += file->size;
        }

        fm_state.file_count++;
        token = newline + 1;
    }

    /* Sort files */
    if (fm_state.sort_mode == SORT_NAME) {
        fm_qsort(fm_state.files, fm_state.file_count, fm_sort_by_name);
    } else if (fm_state.sort_mode == SORT_SIZE) {
        fm_qsort(fm_state.files, fm_state.file_count, fm_sort_by_size);
    } else if (fm_state.sort_mode == SORT_DATE) {
        fm_qsort(fm_state.files, fm_state.file_count, fm_sort_by_date);
    } else if (fm_state.sort_mode == SORT_TYPE) {
        fm_qsort(fm_state.files, fm_state.file_count, fm_sort_by_type);
    }

    if (fm_state.sort_reverse) {
        for (int i = 0; i < fm_state.file_count / 2; i++) {
            fm_file_t temp = fm_state.files[i];
            fm_state.files[i] = fm_state.files[fm_state.file_count - 1 - i];
            fm_state.files[fm_state.file_count - 1 - i] = temp;
        }
    }

    fm_state.highlighted_item = 0;
    fm_state.scroll_offset = 0;
}

void fm_navigate_to(const char *path) {
    /*
     * BUG FIX: chdir() non veniva controllato qui né in back/forward.
     * Se chdir fallisce (path inesistente, permessi) la CWD reale non
     * cambia ma current_path veniva aggiornato ugualmente → divergenza
     * tra stato interno e CWD del processo → crash al prossimo list_dir.
     */
    if (chdir(path) != 0) {
        return;
    }

    if (getcwd(fm_state.current_path, FM_PATH_MAX) != 0) {
        /* getcwd fallito dopo un chdir riuscito: situazione anomala,
           usa il path richiesto come fallback */
        strncpy(fm_state.current_path, path, FM_PATH_MAX - 1);
        fm_state.current_path[FM_PATH_MAX - 1] = '\0';
    }

    fm_state_add_to_history(fm_state.current_path);
    fm_refresh_directory();
    fm_state_update_sidebar();
}

void fm_navigate_back(void) {
    if (fm_state_can_undo()) {
        fm_state.history_pos--;
        /* BUG FIX: chdir non controllato nell'originale */
        if (chdir(fm_state.history[fm_state.history_pos]) != 0) {
            fm_state.history_pos++; /* ripristina posizione se fallisce */
            return;
        }
        if (getcwd(fm_state.current_path, FM_PATH_MAX) != 0) {
            strncpy(fm_state.current_path,
                    fm_state.history[fm_state.history_pos],
                    FM_PATH_MAX - 1);
            fm_state.current_path[FM_PATH_MAX - 1] = '\0';
        }
        fm_refresh_directory();
        fm_state_update_sidebar();
    }
}

void fm_navigate_forward(void) {
    if (fm_state_can_redo()) {
        fm_state.history_pos++;
        /* BUG FIX: chdir non controllato nell'originale */
        if (chdir(fm_state.history[fm_state.history_pos]) != 0) {
            fm_state.history_pos--;
            return;
        }
        if (getcwd(fm_state.current_path, FM_PATH_MAX) != 0) {
            strncpy(fm_state.current_path,
                    fm_state.history[fm_state.history_pos],
                    FM_PATH_MAX - 1);
            fm_state.current_path[FM_PATH_MAX - 1] = '\0';
        }
        fm_refresh_directory();
        fm_state_update_sidebar();
    }
}

void fm_navigate_home(void) {
    fm_navigate_to(fm_state.home_path);
}

void fm_navigate_up(void) {
    if (strcmp(fm_state.current_path, "/") == 0) {
        return;
    }

    char parent[FM_PATH_MAX];
    strncpy(parent, fm_state.current_path, FM_PATH_MAX - 1);
    parent[FM_PATH_MAX - 1] = '\0';

    char *last_slash = strrchr(parent, '/');

    if (last_slash == NULL || last_slash == parent) {
        /* Path tipo "/foo": il padre è root */
        strcpy(parent, "/");
    } else {
        *last_slash = '\0';
        if (parent[0] == '\0') {
            strcpy(parent, "/");
        }
    }

    fm_navigate_to(parent);
}

void fm_copy_file(const char *src, const char *dst) {
    (void)src; (void)dst;
}

void fm_move_file(const char *src, const char *dst) {
    (void)src; (void)dst;
}

void fm_delete_file(const char *path) {
    (void)path;
}

void fm_create_folder(const char *path) {
    (void)path;
}

void fm_rename_file(const char *old, const char *new) {
    (void)old; (void)new;
}

void fm_clipboard_copy(void) {
    if (fm_state.highlighted_item >= 0 && fm_state.highlighted_item < fm_state.file_count) {
        fm_file_t *file = &fm_state.files[fm_state.highlighted_item];
        strncpy(fm_state.clipboard.path, file->full_path, FM_PATH_MAX - 1);
        fm_state.clipboard.path[FM_PATH_MAX - 1] = '\0';
        fm_state.clipboard.is_cut = 0;
        fm_state.clipboard.is_valid = 1;
    }
}

void fm_clipboard_cut(void) {
    if (fm_state.highlighted_item >= 0 && fm_state.highlighted_item < fm_state.file_count) {
        fm_file_t *file = &fm_state.files[fm_state.highlighted_item];
        strncpy(fm_state.clipboard.path, file->full_path, FM_PATH_MAX - 1);
        fm_state.clipboard.path[FM_PATH_MAX - 1] = '\0';
        fm_state.clipboard.is_cut = 1;
        fm_state.clipboard.is_valid = 1;
    }
}

void fm_clipboard_paste(void) {
    if (!fm_state.clipboard.is_valid) return;

    char *src = fm_state.clipboard.path;
    char *filename = strrchr(src, '/');
    if (!filename) filename = src;
    else filename++;

    char dst[FM_PATH_MAX];
    snprintf(dst, FM_PATH_MAX, "%s/%s", fm_state.current_path, filename);

    if (fm_state.clipboard.is_cut) {
        fm_move_file(src, dst);
        fm_state.clipboard.is_valid = 0;
    } else {
        fm_copy_file(src, dst);
    }

    fm_refresh_directory();
}

int fm_get_file_size(const char *path) {
    (void)path;
    return 0;
}

long fm_get_file_mtime(const char *path) {
    (void)path;
    return 0;
}
