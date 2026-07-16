/*
 * user/sys/bin/nxfilem/state.c
 * Navigation history, selection, sort mode, status message.
 */
#include "nxfilem.h"

fm_state_t fm_state = {0};

void fm_state_init(void) {
  memset(&fm_state, 0, sizeof(fm_state));
  fm_state.window_id = -1;
  fm_state.running = 1;
  fm_state.sort_mode = FM_SORT_NAME;
  fm_state.menu_open = -1;
  fm_state.highlighted_item = -1;
  fm_state.last_click_item = -1;

  /* Prefer the process CWD. If it is "/", start in /home when available.
   * If the CWD cannot be determined, also try /home. Final fallback is "/". */
  if (getcwd(fm_state.home_path, FM_PATH_MAX) == 0 &&
      fm_state.home_path[0] != '\0') {

    if (strcmp(fm_state.home_path, "/") == 0 && chdir("/home") == 0) {
      strncpy(fm_state.home_path, "/home", FM_PATH_MAX - 1);
      fm_state.home_path[FM_PATH_MAX - 1] = '\0';
    }

  } else if (chdir("/home") == 0) {

    strncpy(fm_state.home_path, "/home", FM_PATH_MAX - 1);
    fm_state.home_path[FM_PATH_MAX - 1] = '\0';

  } else {

    chdir("/");
    strncpy(fm_state.home_path, "/", FM_PATH_MAX - 1);
    fm_state.home_path[FM_PATH_MAX - 1] = '\0';
  }

  strncpy(fm_state.current_path, fm_state.home_path, FM_PATH_MAX - 1);
  fm_state.current_path[FM_PATH_MAX - 1] = '\0';

  strncpy(fm_state.history[0], fm_state.home_path, FM_PATH_MAX - 1);
  fm_state.history[0][FM_PATH_MAX - 1] = '\0';
}

void fm_state_add_to_history(const char *path) {
  if (fm_state.history_pos < fm_state.history_count - 1)
    fm_state.history_count = fm_state.history_pos + 1;

  if (fm_state.history_count > 0 &&
      strcmp(fm_state.history[fm_state.history_pos], path) == 0)
    return;

  if (fm_state.history_count >= FM_HISTORY_MAX) {
    for (int i = 0; i < fm_state.history_count - 1; i++)
      strncpy(fm_state.history[i], fm_state.history[i + 1], FM_PATH_MAX - 1);
    fm_state.history_count--;
    if (fm_state.history_pos > 0)
      fm_state.history_pos--;
  }

  strncpy(fm_state.history[fm_state.history_count], path, FM_PATH_MAX - 1);
  fm_state.history_count++;
  fm_state.history_pos = fm_state.history_count - 1;
}

int fm_state_can_undo(void) { return fm_state.history_pos > 0; }

int fm_state_can_redo(void) {
  return fm_state.history_pos < fm_state.history_count - 1;
}

void fm_state_sort_files(fm_sort_mode_t mode) {
  if (fm_state.sort_mode == mode)
    fm_state.sort_reverse = !fm_state.sort_reverse;
  else {
    fm_state.sort_mode = mode;
    fm_state.sort_reverse = 0;
  }
  fm_refresh_directory();
}

void fm_state_select_all(void) {
  fm_state.selected_count = 0;
  for (int i = 0; i < fm_state.file_count; i++) {
    fm_state.files[i].is_selected = 1;
    fm_state.selected_count++;
  }
}

void fm_state_deselect_all(void) {
  for (int i = 0; i < fm_state.file_count; i++)
    fm_state.files[i].is_selected = 0;
  fm_state.selected_count = 0;
}

void fm_state_toggle_select(int index) {
  if (index < 0 || index >= fm_state.file_count)
    return;
  fm_file_t *f = &fm_state.files[index];
  f->is_selected = !f->is_selected;
  fm_state.selected_count += f->is_selected ? 1 : -1;
}

void fm_set_status_message(const char *msg) {
  if (!msg)
    fm_state.status_message[0] = '\0';
  else {
    strncpy(fm_state.status_message, msg, sizeof(fm_state.status_message) - 1);
    fm_state.status_message[sizeof(fm_state.status_message) - 1] = '\0';
  }
}
