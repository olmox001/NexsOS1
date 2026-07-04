/*
 * NeXs File Manager - File Operations
 * Directory navigation, file manipulation, clipboard operations
 */
#include "nxfilem.h"

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
    fm_mark_dirty_content();
    fm_mark_dirty_statusbar();
    return;
  }

  char *token = buf;
  while (*token != '\0' && fm_state.file_count < FM_MAX_FILES) {
    char *next = token;
    while (*next != '\0' && *next != ' ' && *next != '\n' && *next != '\r' && *next != '\t')
      next++;

    if (next == token) {
      token++;
      continue;
    }

    *next = '\0';
    if (strlen(token) == 0) {
      token = next + 1;
      continue;
    }

    fm_file_t *file = &fm_state.files[fm_state.file_count];

    strncpy(file->name, token, FM_NAME_MAX - 1);
    file->name[FM_NAME_MAX - 1] = '\0';

    /* Costruisci full_path evitando doppio slash quando current_path è "/" */
    if (fm_state.current_path[0] == '/' && fm_state.current_path[1] == '\0') {
      snprintf(file->full_path, FM_PATH_MAX, "/%s", file->name);
    } else {
      snprintf(file->full_path, FM_PATH_MAX, "%s/%s", fm_state.current_path,
               file->name);
    }

    /* Classificazione delle directory. La libc utente stat() può non
     * distinguere correttamente directory/file, quindi usiamo opendir() come
     * verifica primaria per le cartelle reali. */
    struct stat st;
    int is_dir = 0;
    DIR *d = opendir(file->full_path);
    if (d) {
      is_dir = 1;
      file->size = 0;
      file->mtime = 0;
      closedir(d);
    } else if (stat(file->full_path, &st) == 0) {
      is_dir = S_ISDIR(st.st_mode);
      file->size = (long)st.st_size;
      file->mtime = (long)st.st_mtime;
    } else {
      is_dir = 0;
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
    token = next + 1;
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
  fm_mark_dirty_content();
  fm_mark_dirty_sidebar();
  fm_mark_dirty_statusbar();
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
    strncpy(parent, "/", FM_PATH_MAX - 1);
    parent[FM_PATH_MAX - 1] = '\0';
  } else {
    *last_slash = '\0';
    if (parent[0] == '\0') {
      strncpy(parent, "/", FM_PATH_MAX - 1);
      parent[FM_PATH_MAX - 1] = '\0';
    }
  }

  fm_navigate_to(parent);
}

/* ===== LOGICA DI STUB PER OPERAZIONI FILE (OS1 COMPATIBLE) ===== */

void fm_copy_file(const char *src, const char *dst) {
  if (!src || !dst || strcmp(src, dst) == 0)
    return;

  /* Elimina il file di destinazione esistente per evitare residui di contenuto
     quando la copia è più corta del file originale. */
  OS1_fs_unlink(dst);

  char copy_buf[512];
  int src_offset = 0;
  int dst_offset = 0;
  int bytes_read;

  while ((bytes_read = file_read(src, copy_buf, sizeof(copy_buf), src_offset)) >
         0) {
    int bytes_written = file_write(dst, copy_buf, bytes_read, dst_offset);
    if (bytes_written != bytes_read) {
      return;
    }
    src_offset += bytes_read;
    dst_offset += bytes_written;
  }
}

void fm_move_file(const char *src, const char *dst) {
  if (!src || !dst || strcmp(src, dst) == 0)
    return;

  fm_copy_file(src, dst);
  fm_delete_file(src);
}

void fm_delete_file(const char *path) {
  if (!path)
    return;
  OS1_fs_unlink(path);
}

void fm_create_folder(const char *path) {
  (void)path;
  /* mkdir support is currently unavailable in this release of the linked
     OS1 libc, quindi la creazione della cartella non è supportata qui. */
}

void fm_rename_file(const char *old, const char *new) {
  if (!old || !new || strcmp(old, new) == 0)
    return;

  fm_copy_file(old, new);
  fm_delete_file(old);
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
    fm_mark_dirty_statusbar();
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
    fm_mark_dirty_statusbar();
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
  if (os1_image_path_has_known_ext(name)) {
    return 3; /* immagine: icona ciano */
  }
  if (strcmp(ext, ".tar") == 0 || strcmp(ext, ".zip") == 0 ||
      strcmp(ext, ".gz") == 0 || strcmp(ext, ".xz") == 0) {
    return 4; /* archivio: icona arancione */
  }
  return 1; /* file generico */
}

static int fm_path_has_extension(const char *path) {
  if (!path)
    return 0;

  const char *name = strrchr(path, '/');
  if (name)
    name++;
  else
    name = path;

  /* Ignora il punto iniziale se il file è nascosto. */
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name)
    return 0;
  return 1;
}

static void fm_prune_viewer_pids(void) {
  int write = 0;
  for (int i = 0; i < fm_state.viewer_pid_count; i++) {
    int pid = fm_state.viewer_pids[i];
    if (wait(pid) == -1) {
      fm_state.viewer_pids[write++] = pid;
    }
  }
  fm_state.viewer_pid_count = write;
}

static int fm_register_viewer_pid(int pid) {
  fm_prune_viewer_pids();

  if (fm_state.viewer_pid_count >= FM_VIEWER_PID_MAX) {
    int old_pid = fm_state.viewer_pids[0];
    kill_process(old_pid);
    wait(old_pid);
    for (int i = 1; i < fm_state.viewer_pid_count; i++) {
      fm_state.viewer_pids[i - 1] = fm_state.viewer_pids[i];
    }
    fm_state.viewer_pid_count--;
  }

  fm_state.viewer_pids[fm_state.viewer_pid_count++] = pid;
  return pid;
}

static int fm_is_executable_path(const char *path) {
  if (!path)
    return 0;

  if ((strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/sys/bin/", 9) == 0) &&
      !fm_path_has_extension(path)) {
    return 1;
  }

  return 0;
}

int fm_open_with_kilo(fm_file_t *file) {
  if (!file)
    return -1;
  if (file->is_dir) {
    fm_set_status_message("Cannot open directory in kilo");
    return -1;
  }

  char argv0[16];
  char argv1[FM_PATH_MAX];
  char *argv[2];
  char out_path[NXEXEC_PATH_MAX];
  int pid = 0;

  strncpy(argv0, "kilo", sizeof(argv0) - 1);
  argv0[sizeof(argv0) - 1] = '\0';
  strncpy(argv1, file->full_path, sizeof(argv1) - 1);
  argv1[sizeof(argv1) - 1] = '\0';
  argv[0] = argv0;
  argv[1] = argv1;

  pid = nxexec_spawn_search(2, argv, out_path, /*detached=*/1);
  if (pid <= 0) {
    fm_set_status_message("Failed to open with kilo");
    return pid;
  }
  fm_register_viewer_pid(pid);
  fm_set_status_message("Opened with kilo");
  return pid;
}

void fm_set_status_message(const char *msg) {
  if (!msg) {
    fm_state.status_message[0] = '\0';
  } else {
    strncpy(fm_state.status_message, msg, sizeof(fm_state.status_message) - 1);
    fm_state.status_message[sizeof(fm_state.status_message) - 1] = '\0';
  }
  fm_mark_dirty_statusbar();
}

int fm_open_file(fm_file_t *file) {
  if (!file)
    return -1;
  if (file->is_dir) {
    fm_navigate_to(file->full_path);
    return 0;
  }

  char argv0[16];
  char argv1[FM_PATH_MAX];
  char *argv[2];
  char out_path[NXEXEC_PATH_MAX];
  int pid = 0;

  if (os1_image_path_has_known_ext(file->full_path)) {
    strncpy(argv0, "nximage", sizeof(argv0) - 1);
    argv0[sizeof(argv0) - 1] = '\0';
    strncpy(argv1, file->full_path, sizeof(argv1) - 1);
    argv1[sizeof(argv1) - 1] = '\0';
    argv[0] = argv0;
    argv[1] = argv1;
    pid = nxexec_spawn_search(2, argv, out_path, /*detached=*/1);
    if (pid <= 0) {
      fm_set_status_message("Failed to open image with nximage");
      return pid;
    }
    fm_register_viewer_pid(pid);
    fm_set_status_message("Opened image in nximage");
    return pid;
  }

  if (fm_is_executable_path(file->full_path)) {
    argv[0] = file->full_path;
    pid = nxexec_spawn_search(1, argv, out_path, /*detached=*/1);
    if (pid <= 0) {
      fm_set_status_message("Failed to launch executable");
      return pid;
    }
    fm_set_status_message("Launched executable");
    return pid;
  }

  return fm_open_with_kilo(file);
}
