/*
 * user/sys/bin/nxfilem/nxfilem.h
 * NEXS file manager — core data structures & API.
 *
 * Rewritten to match the rendering/window architecture the rest of the
 * system-app suite already settled on (nxsettings.c/nxui.c/nxbar.c): an
 * owned ARGB framebuffer blitted once per frame, glyphs drawn with
 * font_lib.h directly onto that buffer, fb_rrect() rounded-rect primitives,
 * a per-frame button hit-table that IS the draw call (so draw and hit-test
 * can never drift apart), a single state-signature hash that skips the
 * blit entirely when nothing visible changed, and theme reload on
 * INPUT_TYPE_LOOK_CHANGED via nxres.h. File-type -> program association
 * goes through the shared nxassoc.h table instead of a local if/else chain.
 */
#ifndef _NEXS_FM_H
#define _NEXS_FM_H

#include <dirent.h>
#include <font_lib.h>
#include <image.h>
#include <input.h>
#include <os1.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../nxassoc.h"
#include "../nxexec.h"
#include "../nxicon.h"
#include "../nxres.h"

/* ===== LIMITS ===== */
#define FM_MAX_FILES 500
#define FM_NAME_MAX 256
#define FM_PATH_MAX 512
#define FM_HISTORY_MAX 100
#define FM_VIEWER_PID_MAX 16
#define FM_MAX_BTNS 640 /* one row of file items can register several hit
                         * rects (icon-implicit + name + whole row), plus
                         * menu/toolbar/sidebar/context-menu entries */

/* ===== LAYOUT ===== */
#define FM_MIN_WIN_W 520
#define FM_MIN_WIN_H 360
#define FM_MENUBAR_H 26
#define FM_TOOLBAR_H 40
#define FM_STATUSBAR_H 24
#define FM_SIDEBAR_W 160
#define FM_ROW_H 26
#define FM_PATHBAR_H 28
#define FM_ICON_SIZE 18
#define FM_PAD 8

/* ===== KEY / MOUSE CODES (input.h scancodes, evdev BTN_* button codes) === */
#ifndef KEY_UP
#define KEY_UP INPUT_KEY_UP
#endif
#ifndef KEY_DOWN
#define KEY_DOWN INPUT_KEY_DOWN
#endif
#ifndef MOUSE_BTN_LEFT
#define MOUSE_BTN_LEFT 0x110
#endif
#ifndef MOUSE_BTN_RIGHT
#define MOUSE_BTN_RIGHT 0x111
#endif

/* ===== FILE ENTRY ===== */
typedef struct {
  char name[FM_NAME_MAX];
  char full_path[FM_PATH_MAX];
  int is_dir;
  int is_hidden;
  int is_selected;
  long size;
  long mtime;
  int icon_id; /* NXICON_* from nxicon_classify_file() */
} fm_file_t;

typedef enum {
  FM_SORT_NAME = 0,
  FM_SORT_SIZE = 1,
  FM_SORT_DATE = 2,
  FM_SORT_TYPE = 3,
} fm_sort_mode_t;

typedef struct {
  char path[FM_PATH_MAX];
  int is_cut;
  int is_valid;
} fm_clipboard_t;

/* Rename is an inline, keyboard-driven edit of the highlighted row's name
 * (no text-input widget exists anywhere else in this codebase to reuse, so
 * this is the smallest self-contained one: capture ASCII keys into a
 * buffer, Enter commits via fm_rename_file(), Esc cancels). */
typedef struct {
  int active;
  int index; /* fm_state.files[] index being renamed */
  char buf[FM_NAME_MAX];
  int len;
} fm_rename_state_t;

/* ===== APPLICATION STATE ===== */
typedef struct {
  int window_id;
  int running;

  char current_path[FM_PATH_MAX];
  char home_path[FM_PATH_MAX];

  fm_file_t files[FM_MAX_FILES];
  int file_count;
  int selected_count;
  long long total_size;

  int scroll_offset;
  int highlighted_item;
  int last_click_item;
  long long last_click_time_ms; /* CLOCK_MONOTONIC, ms */

  fm_sort_mode_t sort_mode;
  int sort_reverse;

  fm_clipboard_t clipboard;

  char history[FM_HISTORY_MAX][FM_PATH_MAX];
  int history_pos;
  int history_count;

  int show_hidden;

  int viewer_pids[FM_VIEWER_PID_MAX];
  int viewer_pid_count;

  int context_menu_active;
  int context_menu_x, context_menu_y;

  int menu_open; /* -1 closed, else top-menu index */

  fm_rename_state_t rename;

  char status_message[128];
} fm_state_t;

/* state.c */
extern fm_state_t fm_state;
void fm_state_init(void);
void fm_state_add_to_history(const char *path);
int fm_state_can_undo(void);
int fm_state_can_redo(void);
void fm_state_sort_files(fm_sort_mode_t mode);
void fm_state_select_all(void);
void fm_state_deselect_all(void);
void fm_state_toggle_select(int index);
void fm_set_status_message(const char *msg);

/* sort.c */
int fm_sort_by_name(const fm_file_t *a, const fm_file_t *b);
int fm_sort_by_size(const fm_file_t *a, const fm_file_t *b);
int fm_sort_by_date(const fm_file_t *a, const fm_file_t *b);
int fm_sort_by_type(const fm_file_t *a, const fm_file_t *b);
void fm_qsort(fm_file_t *arr, int n,
             int (*cmp)(const fm_file_t *, const fm_file_t *));

/* fileops.c */
void fm_refresh_directory(void);
void fm_navigate_to(const char *path);
void fm_navigate_back(void);
void fm_navigate_forward(void);
void fm_navigate_home(void);
void fm_navigate_up(void);
void fm_copy_file(const char *src, const char *dst);
void fm_move_file(const char *src, const char *dst);
void fm_delete_file(const char *path);
void fm_create_folder(const char *name);
void fm_rename_file(const char *old_path, const char *new_name);
void fm_clipboard_copy(void);
void fm_clipboard_cut(void);
void fm_clipboard_paste(void);
int fm_open_file(fm_file_t *file);
int fm_open_with_kilo(fm_file_t *file);

/* draw.c: owns the framebuffer/font/window handle, low-level pixel & text
 * primitives. Non-static globals so ui.c can lay out on top of them. */
extern uint32_t *fm_fb;
extern int fm_fb_w, fm_fb_h;
extern int fm_win_id;
extern struct font_ctx *fm_font;

void fm_gfx_init(int win_id, int ww, int wh);
void fm_gfx_realloc(int ww, int wh);
void fm_fill_rect(int x, int y, int w, int h, uint32_t color);
void fm_rrect(int x, int y, int w, int h, int r, uint32_t color);
void fm_draw_text(int x, int y, const char *s, uint32_t color);
int fm_text_width(const char *s);
void fm_draw_icon(int x, int y, int size, int icon_id, int light,
                  uint32_t fallback_color);
void fm_blit(void);

/* ui.c: rendering entry point + palette + hit-testing */
int fm_ui_init(void);
void fm_ui_redraw(int force);
void fm_ui_reinit_window(int ww, int wh);
void fm_ui_handle_click(int x, int y, int button);
void fm_ui_load_theme(int is_light);
void fm_ui_begin_rename(int index);
void fm_ui_commit_rename(void);
void fm_ui_cancel_rename(void);
int fm_ui_visible_rows(void);

/* events.c */
void fm_handle_keyboard(input_event_t *event);

/* main.c */
int main(void);

#endif /* _NEXS_FM_H */
