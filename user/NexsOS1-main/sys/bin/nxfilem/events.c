/*
 * user/sys/bin/nxfilem/events.c
 * Keyboard handling (mouse clicks go through fm_ui_handle_click, ui.c).
 */
#include "nxfilem.h"

/* fm_handle_keyboard_rename - text-entry mode while renaming the highlighted
 * row: printable ASCII appends, Backspace deletes, Enter commits, Esc
 * cancels. No other key does anything while this mode is active. */
static void fm_handle_keyboard_rename(unsigned char key, uint16_t sc) {
  if (sc == INPUT_KEY_ESC || key == 27) {
    fm_ui_cancel_rename();
    return;
  }
  if (sc == INPUT_KEY_ENTER || key == '\r' || key == '\n') {
    fm_ui_commit_rename();
    return;
  }
  if (sc == INPUT_KEY_BACKSPACE || key == '\b' || key == 127) {
    if (fm_state.rename.len > 0)
      fm_state.rename.buf[--fm_state.rename.len] = '\0';
    return;
  }
  if (key >= 32 && key < 127 &&
      fm_state.rename.len < (int)sizeof(fm_state.rename.buf) - 1) {
    fm_state.rename.buf[fm_state.rename.len++] = (char)key;
    fm_state.rename.buf[fm_state.rename.len] = '\0';
  }
}

void fm_handle_keyboard(input_event_t *event) {
  if (event->type != INPUT_TYPE_KEYBOARD || event->keyboard.state != KEY_PRESSED)
    return;

  unsigned char key = event->keyboard.key;
  uint16_t sc = event->keyboard.scancode;

  if (fm_state.rename.active) {
    fm_handle_keyboard_rename(key, sc);
    return;
  }

  if (sc == INPUT_KEY_UP || key == 'w') {
    if (fm_state.highlighted_item > 0) {
      fm_state.highlighted_item--;
      if (fm_state.highlighted_item < fm_state.scroll_offset)
        fm_state.scroll_offset = fm_state.highlighted_item;
    }
    return;
  }
  if (sc == INPUT_KEY_DOWN || key == 's') {
    if (fm_state.highlighted_item < fm_state.file_count - 1) {
      fm_state.highlighted_item++;
      int visible = fm_ui_visible_rows();
      if (visible < 1)
        visible = 1;
      if (fm_state.highlighted_item >= fm_state.scroll_offset + visible)
        fm_state.scroll_offset = fm_state.highlighted_item - visible + 1;
    }
    return;
  }

  if (key == ' ') {
    fm_state_toggle_select(fm_state.highlighted_item);
    return;
  }

  if (key == 'u' || sc == INPUT_KEY_BACKSPACE)
    fm_navigate_up();
  else if (key == 'h')
    fm_navigate_home();
  else if (key == 'r' || key == 'R')
    fm_refresh_directory();
  else if (key == 'c' || key == 'C')
    fm_clipboard_copy();
  else if (key == 'x' || key == 'X')
    fm_clipboard_cut();
  else if (key == 'v' || key == 'V')
    fm_clipboard_paste();
  else if (key == 'd' || key == 'D') {
    if (fm_state.highlighted_item >= 0 &&
        fm_state.highlighted_item < fm_state.file_count) {
      fm_delete_file(fm_state.files[fm_state.highlighted_item].full_path);
      fm_refresh_directory();
    }
  } else if (key == 'n' || key == 'N') {
    fm_create_folder(NULL);
  } else if (key == 'e' || key == 'E') {
    fm_ui_begin_rename(fm_state.highlighted_item);
  } else if (key == 'a') {
    fm_state_select_all();
  } else if (key == 'A') {
    fm_state_deselect_all();
  } else if (key == '\r' || key == '\n' || key == 'o' || key == 'O' ||
             sc == INPUT_KEY_ENTER) {
    if (fm_state.highlighted_item >= 0 &&
        fm_state.highlighted_item < fm_state.file_count)
      fm_open_file(&fm_state.files[fm_state.highlighted_item]);
  } else if (key == '1') {
    fm_state_sort_files(FM_SORT_NAME);
  } else if (key == '2') {
    fm_state_sort_files(FM_SORT_SIZE);
  } else if (key == '3') {
    fm_state_sort_files(FM_SORT_DATE);
  } else if (key == '4') {
    fm_state_sort_files(FM_SORT_TYPE);
  } else if (sc == INPUT_KEY_ESC || key == 27) {
    fm_state.context_menu_active = 0;
    fm_state.menu_open = -1;
  } else if (key == 'q' || key == 'Q') {
    fm_state.running = 0;
  }
}
