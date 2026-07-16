/*
 * user/sys/bin/nxfilem/fileops.c
 * Directory navigation, file manipulation, clipboard, and file-open dispatch.
 */
#include "nxfilem.h"

/*
 * fm_refresh_directory - repopulate fm_state.files[] with current_path's
 * contents. Directory-ness comes from stat(S_ISDIR) (list_dir's own output
 * carries no type). Hidden entries are filtered out here (not just at
 * render time) when show_hidden is off, so scroll math/selection indices
 * never have to account for entries the user cannot see.
 */
void fm_refresh_directory(void) {
  static char buf[16384];
  int len = list_dir(fm_state.current_path, buf, sizeof(buf));

  fm_state.file_count = 0;
  fm_state.selected_count = 0;
  fm_state.total_size = 0;

  if (len >= 0) {
    char *token = buf;
    while (*token != '\0' && fm_state.file_count < FM_MAX_FILES) {
      char *next = token;
      while (*next != '\0' && *next != ' ' && *next != '\n' && *next != '\r' &&
             *next != '\t')
        next++;

      if (next == token) {
        token++;
        continue;
      }
      *next = '\0';
      if (token[0] == '\0') {
        token = next + 1;
        continue;
      }

      int is_hidden = (token[0] == '.');
      if (is_hidden && !fm_state.show_hidden) {
        token = next + 1;
        continue;
      }

      fm_file_t *file = &fm_state.files[fm_state.file_count];
      strncpy(file->name, token, FM_NAME_MAX - 1);
      file->name[FM_NAME_MAX - 1] = '\0';

      if (fm_state.current_path[0] == '/' && fm_state.current_path[1] == '\0')
        snprintf(file->full_path, FM_PATH_MAX, "/%s", file->name);
      else
        snprintf(file->full_path, FM_PATH_MAX, "%s/%s", fm_state.current_path,
                 file->name);

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
        file->size = 0;
        file->mtime = 0;
      }

      file->is_dir = is_dir;
      file->is_hidden = is_hidden;
      file->is_selected = 0;
      file->icon_id = nxicon_classify_file(file->name, is_dir);

      if (!is_dir)
        fm_state.total_size += file->size;

      fm_state.file_count++;
      token = next + 1;
    }
  }

  switch (fm_state.sort_mode) {
  case FM_SORT_NAME:
    fm_qsort(fm_state.files, fm_state.file_count, fm_sort_by_name);
    break;
  case FM_SORT_SIZE:
    fm_qsort(fm_state.files, fm_state.file_count, fm_sort_by_size);
    break;
  case FM_SORT_DATE:
    fm_qsort(fm_state.files, fm_state.file_count, fm_sort_by_date);
    break;
  case FM_SORT_TYPE:
    fm_qsort(fm_state.files, fm_state.file_count, fm_sort_by_type);
    break;
  }
  if (fm_state.sort_reverse) {
    for (int i = 0; i < fm_state.file_count / 2; i++) {
      fm_file_t t = fm_state.files[i];
      fm_state.files[i] = fm_state.files[fm_state.file_count - 1 - i];
      fm_state.files[fm_state.file_count - 1 - i] = t;
    }
  }

  fm_state.highlighted_item = fm_state.file_count > 0 ? 0 : -1;
  fm_state.scroll_offset = 0;
}

void fm_navigate_to(const char *path) {
  if (chdir(path) != 0) {
    fm_set_status_message("Cannot open directory");
    return;
  }
  if (getcwd(fm_state.current_path, FM_PATH_MAX) != 0) {
    strncpy(fm_state.current_path, path, FM_PATH_MAX - 1);
    fm_state.current_path[FM_PATH_MAX - 1] = '\0';
  }
  fm_state_add_to_history(fm_state.current_path);
  fm_refresh_directory();
}

void fm_navigate_back(void) {
  if (!fm_state_can_undo())
    return;
  fm_state.history_pos--;
  if (chdir(fm_state.history[fm_state.history_pos]) != 0) {
    fm_state.history_pos++;
    return;
  }
  if (getcwd(fm_state.current_path, FM_PATH_MAX) != 0) {
    strncpy(fm_state.current_path, fm_state.history[fm_state.history_pos],
            FM_PATH_MAX - 1);
    fm_state.current_path[FM_PATH_MAX - 1] = '\0';
  }
  fm_refresh_directory();
}

void fm_navigate_forward(void) {
  if (!fm_state_can_redo())
    return;
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
}

void fm_navigate_home(void) { fm_navigate_to(fm_state.home_path); }

void fm_navigate_up(void) {
  if (strcmp(fm_state.current_path, "/") == 0)
    return;

  char parent[FM_PATH_MAX];
  strncpy(parent, fm_state.current_path, FM_PATH_MAX - 1);
  parent[FM_PATH_MAX - 1] = '\0';

  char *slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent)
    strncpy(parent, "/", FM_PATH_MAX - 1);
  else {
    *slash = '\0';
    if (parent[0] == '\0')
      strncpy(parent, "/", FM_PATH_MAX - 1);
  }
  fm_navigate_to(parent);
}

/* ===== File operations (OS1 has no rename/mkdir syscall: copy+unlink is the
 * only available primitive for a "move") ===== */

/* Cap for a single in-memory copy: generous for anything nxfilem realistically
 * moves (source, config, small assets) without risking a huge kmalloc/user
 * malloc for a stray large file. Mirrors image.h's OS1_IMAGE_MAX_FILE_BYTES
 * spirit (a deliberate, documented bound, not an arbitrary one). */
#define FM_COPY_MAX_BYTES (8 * 1024 * 1024)

/*
 * fm_copy_file - single read + single write, NOT a chunked read/write loop.
 *
 * file_read()/OS1_fs_read() opens a FILE capability handle per call
 * (OS1low_handle_create -> OS1_object_read -> OS1low_handle_close, see
 * lib.c) whenever size>0 and buf!=NULL — i.e. every call this function
 * makes. A chunked loop (the original implementation, 512 B at a time)
 * opened and closed one of those handles per chunk, which crashed the
 * kernel on anything but a tiny file — the handle-churn path is new and
 * not hardened for that call rate yet. nxshell's `mv`/`cp` avoid the loop
 * entirely (one file_read + one OS1_fs_write); this does the same, sized to
 * the file instead of nxshell's fixed 1024 B ceiling.
 */
void fm_copy_file(const char *src, const char *dst) {
  if (!src || !dst || strcmp(src, dst) == 0)
    return;

  int size = file_read(src, NULL, 0, 0); /* size probe, no handle opened */
  if (size < 0)
    return;
  if (size == 0) {
    OS1_fs_unlink(dst);
    file_write(dst, "", 0, 0); /* create/truncate an empty destination */
    return;
  }
  if (size > FM_COPY_MAX_BYTES)
    return;

  uint8_t *buf = (uint8_t *)malloc((size_t)size);
  if (!buf)
    return;

  int n = file_read(src, buf, size, 0);
  if (n <= 0) {
    free(buf);
    return;
  }

  OS1_fs_unlink(dst); /* avoid trailing garbage if dst previously existed */
  file_write(dst, buf, n, 0);
  free(buf);
}

void fm_move_file(const char *src, const char *dst) {
  if (!src || !dst || !src[0] || !dst[0])
    return;

  if (strcmp(src, dst) == 0)
    return;

  char src_copy[FM_PATH_MAX];
  char dst_copy[FM_PATH_MAX];

  strncpy(src_copy, src, sizeof(src_copy) - 1);
  src_copy[sizeof(src_copy) - 1] = '\0';

  strncpy(dst_copy, dst, sizeof(dst_copy) - 1);
  dst_copy[sizeof(dst_copy) - 1] = '\0';

  fm_copy_file(src_copy, dst_copy);

  /*
   * Cancella solo se la copia è realmente avvenuta.
   * Verifica minima: il file destinazione deve esistere.
   */
  struct stat st;
  if (stat(dst_copy, &st) == 0)
    fm_delete_file(src_copy);
}

void fm_delete_file(const char *path) {
  if (path)
    OS1_fs_unlink(path);
}

/* fm_create_folder - create a subdirectory in the current directory via the
 * mkdir() syscall wrapper (SYS_MKDIR), then refresh so the new folder shows. */
void fm_create_folder(const char *name) {
  if (!name || !name[0]) {
    fm_set_status_message("New folder: empty name");
    return;
  }

  char path[FM_PATH_MAX];
  if (fm_state.current_path[0] == '/' && fm_state.current_path[1] == '\0')
    snprintf(path, sizeof(path), "/%s", name);
  else
    snprintf(path, sizeof(path), "%s/%s", fm_state.current_path, name);

  if (mkdir(path, 0755) != 0) {
    fm_set_status_message("New folder: failed");
    return;
  }
  fm_set_status_message("Folder created");
  fm_refresh_directory();
}

void fm_rename_file(const char *old_path, const char *new_name) {
  if (!old_path || !new_name || !new_name[0])
    return;

  char new_path[FM_PATH_MAX];

  const char *slash = strrchr(old_path, '/');

  if (slash == NULL) {
    snprintf(new_path, sizeof(new_path), "%s", new_name);
  } else if (slash == old_path) {
    /* file nella root: /old -> /new */
    snprintf(new_path, sizeof(new_path), "/%s", new_name);
  } else {
    int dir_len = (int)(slash - old_path);

    snprintf(new_path, sizeof(new_path), "%.*s/%s", dir_len, old_path,
             new_name);
  }

  new_path[sizeof(new_path) - 1] = '\0';

  if (strcmp(old_path, new_path) == 0)
    return;

  fm_copy_file(old_path, new_path);

  struct stat st;
  if (stat(new_path, &st) == 0)
    fm_delete_file(old_path);
}

/* ===== Clipboard ===== */

void fm_clipboard_copy(void) {
  if (fm_state.highlighted_item < 0 ||
      fm_state.highlighted_item >= fm_state.file_count)
    return;
  fm_file_t *file = &fm_state.files[fm_state.highlighted_item];
  strncpy(fm_state.clipboard.path, file->full_path, FM_PATH_MAX - 1);
  fm_state.clipboard.is_cut = 0;
  fm_state.clipboard.is_valid = 1;
  fm_set_status_message("Copied");
}

void fm_clipboard_cut(void) {
  if (fm_state.highlighted_item < 0 ||
      fm_state.highlighted_item >= fm_state.file_count)
    return;
  fm_file_t *file = &fm_state.files[fm_state.highlighted_item];
  strncpy(fm_state.clipboard.path, file->full_path, FM_PATH_MAX - 1);
  fm_state.clipboard.is_cut = 1;
  fm_state.clipboard.is_valid = 1;
  fm_set_status_message("Cut");
}

void fm_clipboard_paste(void) {
  if (!fm_state.clipboard.is_valid)
    return;

  char src[FM_PATH_MAX];

  strncpy(src, fm_state.clipboard.path, sizeof(src) - 1);
  src[sizeof(src) - 1] = '\0';

  char *filename = strrchr(src, '/');
  filename = filename ? filename + 1 : src;

  char dst[FM_PATH_MAX];
  snprintf(dst, FM_PATH_MAX, "%s/%s", fm_state.current_path, filename);

  if (fm_state.clipboard.is_cut) {
    fm_move_file(src, dst);
    fm_state.clipboard.is_valid = 0;
  } else {
    fm_copy_file(src, dst);
  }

  fm_set_status_message("Pasted");
  fm_refresh_directory();
}

/* ===== Viewer/child process tracking ===== */

static void fm_prune_viewer_pids(void) {
  int w = 0;
  for (int i = 0; i < fm_state.viewer_pid_count; i++) {
    int pid = fm_state.viewer_pids[i];
    if (wait(pid) == -1)
      fm_state.viewer_pids[w++] = pid;
  }
  fm_state.viewer_pid_count = w;
}

static void fm_register_viewer_pid(int pid) {
  fm_prune_viewer_pids();
  if (fm_state.viewer_pid_count >= FM_VIEWER_PID_MAX) {
    kill_process(fm_state.viewer_pids[0]);
    wait(fm_state.viewer_pids[0]);
    for (int i = 1; i < fm_state.viewer_pid_count; i++)
      fm_state.viewer_pids[i - 1] = fm_state.viewer_pids[i];
    fm_state.viewer_pid_count--;
  }
  fm_state.viewer_pids[fm_state.viewer_pid_count++] = pid;
}

/*
 * Every program this app runs — the resolved nxassoc.h target, a direct
 * executable, or kilo as a fallback — is launched via nxexec.h's
 * nxexec_spawn_hosted_argv(), i.e. "/sys/bin/nxexec <prog> [path]", exactly
 * like nxlauncher's tiles (nxexec_spawn_hosted()). nxfilem never spawns an
 * app directly (nxexec_spawn_search/nxexec_spawn_detached): nxexec is the
 * one place that decides whether the child needs a hosting terminal or gets
 * to run standalone, and every launcher in this system goes through it.
 */

int fm_open_with_kilo(fm_file_t *file) {
  if (!file || file->is_dir)
    return -1;

  char argv0[] = "kilo";
  char argv1[FM_PATH_MAX];
  char *argv[2];

  strncpy(argv1, file->full_path, sizeof(argv1) - 1);
  argv1[sizeof(argv1) - 1] = '\0';
  argv[0] = argv0;
  argv[1] = argv1;

  int pid = nxexec_spawn_hosted_argv(2, argv);
  if (pid <= 0) {
    fm_set_status_message("Failed to open with kilo");
    return pid;
  }
  fm_register_viewer_pid(pid);
  fm_set_status_message("Opened with kilo");
  return pid;
}

/*
 * fm_open_file - resolve the program for 'file' via the shared nxassoc.h
 * table and launch it through nxexec (nxexec_spawn_hosted_argv), instead of
 * a local if/else chain and instead of spawning it directly.
 * Directories navigate instead of "opening".
 */
int fm_open_file(fm_file_t *file) {
  if (!file)
    return -1;
  if (file->is_dir) {
    fm_navigate_to(file->full_path);
    return 0;
  }

  char prog[NXASSOC_PROG_MAX];
  int kind = nxassoc_resolve(file->full_path, prog, sizeof(prog));

  char argv1[FM_PATH_MAX];
  char *argv[2];
  int argc;

  if (kind == NXASSOC_KIND_EXEC) {
    argv[0] = prog;
    argc = 1;
  } else {
    strncpy(argv1, file->full_path, sizeof(argv1) - 1);
    argv1[sizeof(argv1) - 1] = '\0';
    argv[0] = prog;
    argv[1] = argv1;
    argc = 2;
  }

  int pid = nxexec_spawn_hosted_argv(argc, argv);
  if (pid <= 0) {
    fm_set_status_message("Failed to launch");
    return pid;
  }
  fm_register_viewer_pid(pid);
  fm_set_status_message("Opened");
  return pid;
}
