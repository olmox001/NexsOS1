/*
 * NeXs File Manager - File Operations
 * Directory navigation, file manipulation, clipboard operations
 */
#include "nexs-fm.h"

/*
 * fm_refresh_directory - ripopola fm_state.files[] con il contenuto di
 * current_path. Per ogni entry costruiamo il full_path e poi
 * classifichiamo il tipo via stat(S_ISDIR) — NON più dallo slash finale
 * nel nome (che è un hack e dipende dal formato di list_dir).
 */
void fm_refresh_directory(void) {
  static char buf[16384];
  int len = list_dir(fm_state.current_path, buf, sizeof(buf));

  fm_state.file_count = 0;
  fm_state.selected_count = 0;
  fm_state.total_size = 0;

  if (len < 0) {
    fm_state.needs_redraw = 1;
    return;
  }

  char *token = buf;
  while (*token != '\0' && fm_state.file_count < FM_MAX_FILES) {
    char *newline = strchr(token, '\n');
    if (!newline)
      break;
    *newline = '\0';

    if (strlen(token) == 0) {
      token = newline + 1;
      continue;
    }

    fm_file_t *file = &fm_state.files[fm_state.file_count];

    int name_len = strlen(token);

    /* list_dir storicamente appende '/' ai nomi delle directory;
     * lo tolleriamo qui per compatibilità ma il tipo vero lo decide
     * stat() sotto. */
    if (name_len > 0 && token[name_len - 1] == '/') {
      token[name_len - 1] = '\0';
      name_len--;
    }

    strncpy(file->name, token, FM_NAME_MAX - 1);
    file->name[FM_NAME_MAX - 1] = '\0';

    /* Costruisci full_path evitando doppio slash quando current_path è "/" */
    if (fm_state.current_path[0] == '/' && fm_state.current_path[1] == '\0') {
      snprintf(file->full_path, FM_PATH_MAX, "/%s", file->name);
    } else {
      snprintf(file->full_path, FM_PATH_MAX, "%s/%s", fm_state.current_path,
               file->name);
    }

    /* Classificazione via stat() (lib.c fornisce stat() su file_read
     * con size probing + S_IFREG). Riconosciamo directory dal
     * fallimento di stat su path normale, OPPURE se list_dir ha
     * aggiunto '/' al nome (per compat con vecchio formato). */
    struct stat st;
    int is_dir = 0;
    if (stat(file->full_path, &st) == 0) {
      is_dir = S_ISDIR(st.st_mode);
      file->size = (long)st.st_size;
      file->mtime = (long)st.st_mtime;
    } else {
      /* stat fallita (es. non file regolare). Tenta come directory:
       * se list_dir aveva suffissato '/' è quasi certamente una dir. */
      is_dir = 1;
      file->size = 0;
      file->mtime = 0;
    }

    file->is_dir = is_dir;
    file->is_hidden = (file->name[0] == '.');
    file->is_selected = 0;
    file->icon_id = is_dir ? 0 : fm_classify_icon(file->name);

    if (!is_dir) {
      fm_state.total_size += file->size;
    }

    fm_state.file_count++;
    token = newline + 1;
  }

  /* Ordinamento dei file tramite fm_qsort (allineato ai prototipi dell'header)
   */
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
  fm_state.needs_redraw = 1;
}

void fm_navigate_to(const char *path) {
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
    if (chdir(fm_state.history[fm_state.history_pos]) != 0) {
      fm_state.history_pos++; /* ripristina posizione se fallisce */
      return;
    }
    if (getcwd(fm_state.current_path, FM_PATH_MAX) != 0) {
      strncpy(fm_state.current_path, fm_state.history[fm_state.history_pos],
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
    if (chdir(fm_state.history[fm_state.history_pos]) != 0) {
      fm_state.history_pos--;
      return;
    }
    if (getcwd(fm_state.current_path, FM_PATH_MAX) != 0) {
      strncpy(fm_state.current_path, fm_state.history[fm_state.history_pos],
              FM_PATH_MAX - 1);
      fm_state.current_path[FM_PATH_MAX - 1] = '\0';
    }
    fm_refresh_directory();
    fm_state_update_sidebar();
  }
}

void fm_navigate_home(void) { fm_navigate_to(fm_state.home_path); }

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

/* ===== LOGICA DI STUB PER OPERAZIONI FILE (OS1 COMPATIBLE) ===== */

void fm_copy_file(const char *src, const char *dst) {
  /* Cast espliciti a void per evitare -Werror=unused-parameter */
  (void)src;
  (void)dst;

  char copy_buf[512];
  int offset = 0;
  int bytes_read;

  /* Utilizziamo la reale syscall file_read esposta dal kernel */
  while ((bytes_read = file_read(src, copy_buf, sizeof(copy_buf), offset)) >
         0) {
    offset += bytes_read;
  }
}

void fm_move_file(const char *src, const char *dst) {
  /* Chiamate interne sicure che non generano warning */
  fm_copy_file(src, dst);
  fm_delete_file(src);
}

void fm_delete_file(const char *path) { (void)path; }

void fm_create_folder(const char *path) { (void)path; }

void fm_rename_file(const char *old, const char *new) {
  (void)old;
  (void)new;
}

/* ===== APPUNTI E CLIPBOARD ===== */

void fm_clipboard_copy(void) {
  if (fm_state.highlighted_item >= 0 &&
      fm_state.highlighted_item < fm_state.file_count) {
    fm_file_t *file = &fm_state.files[fm_state.highlighted_item];
    strncpy(fm_state.clipboard.path, file->full_path, FM_PATH_MAX - 1);
    fm_state.clipboard.path[FM_PATH_MAX - 1] = '\0';
    fm_state.clipboard.is_cut = 0;
    fm_state.clipboard.is_valid = 1;
    fm_state.needs_redraw = 1;
  }
}

void fm_clipboard_cut(void) {
  if (fm_state.highlighted_item >= 0 &&
      fm_state.highlighted_item < fm_state.file_count) {
    fm_file_t *file = &fm_state.files[fm_state.highlighted_item];
    strncpy(fm_state.clipboard.path, file->full_path, FM_PATH_MAX - 1);
    fm_state.clipboard.path[FM_PATH_MAX - 1] = '\0';
    fm_state.clipboard.is_cut = 1;
    fm_state.clipboard.is_valid = 1;
    fm_state.needs_redraw = 1;
  }
}

void fm_clipboard_paste(void) {
  if (!fm_state.clipboard.is_valid)
    return;

  char *src = fm_state.clipboard.path;
  char *filename = strrchr(src, '/');
  if (!filename)
    filename = src;
  else
    filename++;

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
  struct stat st;
  if (stat(path, &st) == 0) {
    return (int)st.st_size;
  }
  return 0;
}

long fm_get_file_mtime(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return (long)st.st_mtime;
  }
  return 0;
}

/*
 * fm_classify_icon - mappa l'estensione di un file all'icona appropriata
 */
int fm_classify_icon(const char *name) {
  if (!name)
    return 1;
  const char *ext = strrchr(name, '.');
  if (!ext)
    return 1;

  if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0 ||
      strcmp(ext, ".S") == 0 || strcmp(ext, ".s") == 0) {
    return 2; /* eseguibile / sorgente: icona verde */
  }
  if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".png") == 0 ||
      strcmp(ext, ".bmp") == 0 || strcmp(ext, ".gif") == 0) {
    return 3; /* immagine: icona ciano */
  }
  if (strcmp(ext, ".tar") == 0 || strcmp(ext, ".zip") == 0 ||
      strcmp(ext, ".gz") == 0 || strcmp(ext, ".xz") == 0) {
    return 4; /* archivio: icona arancione */
  }
  return 1; /* file generico */
}