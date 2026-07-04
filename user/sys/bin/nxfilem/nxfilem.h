/*
 * NeXs File Manager - Professional File Manager for OS1
 * Header: Core Data Structures & API
 */
#ifndef _NEXS_FM_H
#define _NEXS_FM_H

#include <os1.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <input.h>
#include <graphics.h>
#include <image.h>
#include <dirent.h>
#include "../nxexec.h"
#include <sys/stat.h>
#include <time.h>

/* ===== WINDOW & UI CONSTANTS ===== */
#define FM_WIN_W 640
#define FM_WIN_H 480
#define FM_MENU_HEIGHT 28
#define FM_TOOLBAR_HEIGHT 56
#define FM_SIDEBAR_WIDTH 180
#define FM_STATUSBAR_HEIGHT 28
#define FM_CONTENT_PAD 6
#define FM_ITEM_HEIGHT 36
#define FM_ITEM_ICON_SIZE 28

/* Work area dimensions */
#define FM_CONTENT_Y (FM_MENU_HEIGHT + FM_TOOLBAR_HEIGHT)
#define FM_CONTENT_X FM_SIDEBAR_WIDTH
#define FM_CONTENT_W (FM_WIN_W - FM_SIDEBAR_WIDTH)
#define FM_CONTENT_H (FM_WIN_H - FM_CONTENT_Y - FM_STATUSBAR_HEIGHT)

/* ===== COLOR SCHEME (Catppuccin) ===== */
#define FM_COLOR_BG 0xFF1e1e2e
#define FM_COLOR_SURFACE 0xFF313244
#define FM_COLOR_SURFACE1 0xFF45475a
#define FM_COLOR_SURFACE2 0xFF585b70
#define FM_COLOR_OVERLAY 0xFF6c7086
#define FM_COLOR_FG 0xFFcdd6f4
#define FM_COLOR_SUBTEXT 0xFFa6adc8
#define FM_COLOR_BLUE 0xFF89b4fa
#define FM_COLOR_CYAN 0xFF94e2d5
#define FM_COLOR_GREEN 0xFFa6e3a1
#define FM_COLOR_YELLOW 0xFFf9e2af
#define FM_COLOR_ORANGE 0xFFfab387
#define FM_COLOR_RED 0xFFf38ba8
#define FM_COLOR_LAVENDER 0xFFb4befe

/* Menu colors */
#define FM_MENU_BG FM_COLOR_SURFACE
#define FM_MENU_TEXT FM_COLOR_FG
#define FM_MENU_HOVER FM_COLOR_SURFACE1

/* ===== KEY CODES =====
 * Le frecce arrivano dal driver virtio-input come scancode evdev:
 *   UP=103, LEFT=105, RIGHT=106, DOWN=108 (vedi <input.h>).
 * input.h li ridefinisce in INPUT_KEY_*: li riusiamo per le frecce. */
#ifndef KEY_UP
#define KEY_UP    INPUT_KEY_UP
#endif
#ifndef KEY_DOWN
#define KEY_DOWN  INPUT_KEY_DOWN
#endif
#ifndef KEY_LEFT
#define KEY_LEFT  INPUT_KEY_LEFT
#endif
#ifndef KEY_RIGHT
#define KEY_RIGHT INPUT_KEY_RIGHT
#endif

/* ===== MOUSE BUTTON CODES =====
 * Il compositor passa `mouse.button` = evdev BTN_* code (vedi
 * kernel/include/drivers/virtio_input.h). Sono KEY CODE, non bitmask,
 * quindi si confrontano con `==`, non con `&`. */
#ifndef MOUSE_BTN_LEFT
#define MOUSE_BTN_LEFT   0x110
#endif
#ifndef MOUSE_BTN_RIGHT
#define MOUSE_BTN_RIGHT  0x111
#endif
#ifndef MOUSE_BTN_MIDDLE
#define MOUSE_BTN_MIDDLE 0x112
#endif

/* ===== FILE ENTRY STRUCTURE ===== */
#define FM_MAX_FILES 500
#define FM_NAME_MAX 256
#define FM_PATH_MAX 512
#define FM_SORT_MAX 50
#define FM_HISTORY_MAX 100
#define FM_VIEWER_PID_MAX 16

typedef struct {
    char name[FM_NAME_MAX];
    char full_path[FM_PATH_MAX];
    int is_dir;
    int is_hidden;
    int is_selected;
    long size;
    long mtime;
    int icon_id;
} fm_file_t;

/* ===== SORT & FILTER ===== */
typedef enum {
    SORT_NAME = 0,
    SORT_SIZE = 1,
    SORT_DATE = 2,
    SORT_TYPE = 3
} fm_sort_mode_t;

/* ===== VIEW MODE ===== */
typedef enum {
    VIEW_LIST = 0,
    VIEW_GRID = 1,
    VIEW_DETAIL = 2
} fm_view_mode_t;

/* ===== CLIPBOARD ===== */
typedef struct {
    char path[FM_PATH_MAX];
    int is_cut;
    int is_valid;
} fm_clipboard_t;

/* ===== MENU ITEM ===== */
typedef struct {
    char label[64];
    int x, y, w, h;
    int has_submenu;
    int enabled;
    int shortcut_key;
    int id;
} fm_menu_item_t;

/* ===== TOOLBAR BUTTON ===== */
typedef struct {
    char label[32];
    int x, y, w, h;
    int icon_id;
    int enabled;
    int hovered;
    int id;
} fm_button_t;

/* ===== APPLICATION STATE ===== */
typedef struct {
    int window_id;
    int running;
    int needs_redraw;
    int dirty_all;
    int dirty_menu;
    int dirty_toolbar;
    int dirty_sidebar;
    int dirty_content;
    int dirty_statusbar;
    int dirty_context_menu;

    char current_path[FM_PATH_MAX];
    char home_path[FM_PATH_MAX];
    
    fm_file_t files[FM_MAX_FILES];
    int file_count;
    int selected_count;
    long long total_size;
    long long selected_size;
    
    int scroll_offset;
    int highlighted_item;
    int last_click_item;
    long long last_click_time; /* millisecondi monotonic (clock_gettime) */
    
    fm_sort_mode_t sort_mode;
    int sort_reverse;
    fm_view_mode_t view_mode;
    
    fm_clipboard_t clipboard;
    
    char history[FM_HISTORY_MAX][FM_PATH_MAX];
    int history_pos;
    int history_count;
    
    int show_hidden;
    int show_sidebar;
    int show_statusbar;

    int viewer_pids[FM_VIEWER_PID_MAX];
    int viewer_pid_count;

    /* Context menu (click destro sulla lista file) */
    int context_menu_active;
    int context_menu_x;
    int context_menu_y;

    fm_menu_item_t menus[50];
    int menu_count;
    fm_button_t buttons[20];
    int button_count;
    
    int menu_open;
    int menu_id;
    
    char search_query[256];
    int search_active;
    
    char status_message[128];
} fm_state_t;

/* ===== FUNCTION DECLARATIONS ===== */

/* main.c */
int main(void);
void fm_init(void);
void fm_cleanup(void);
void fm_main_loop(void);

/* ui.c */
void fm_draw_menu(void);
void fm_draw_toolbar(void);
void fm_draw_sidebar(void);
void fm_draw_content(void);
void fm_draw_statusbar(void);
void fm_render_dirty_ui(void);
void fm_draw_context_menu(int x, int y);
int fm_open_with_kilo(fm_file_t *file);

void fm_mark_dirty_all(void);
void fm_mark_dirty_menu(void);
void fm_mark_dirty_toolbar(void);
void fm_mark_dirty_sidebar(void);
void fm_mark_dirty_content(void);
void fm_mark_dirty_statusbar(void);
void fm_mark_dirty_context_menu(void);

/* draw.c */
void fm_draw_rect(int x, int y, int w, int h, uint32_t color);
void fm_draw_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
void fm_draw_text(int x, int y, const char *text, uint32_t color);
void fm_draw_centered_text(int x, int y, int w, int h, const char *text, uint32_t color);
void fm_draw_icon(int x, int y, int id, uint32_t color);
void fm_draw_file_icon(int x, int y, const fm_file_t *file);
void fm_draw_scrollbar(int x, int y, int h, int total, int visible, int offset);

/* events.c */
void fm_handle_keyboard(input_event_t *event);
void fm_handle_mouse(input_event_t *event);
void fm_handle_mouse_click(int x, int y);
void fm_handle_mouse_double_click(int x, int y);
void fm_handle_mouse_right_click(int x, int y);

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
void fm_create_folder(const char *path);
void fm_rename_file(const char *old, const char *new);
void fm_clipboard_copy(void);
void fm_clipboard_cut(void);
void fm_clipboard_paste(void);
int fm_get_file_size(const char *path);
long fm_get_file_mtime(const char *path);
int fm_classify_icon(const char *name);
int fm_open_file(fm_file_t *file);

/* state.c */
extern fm_state_t fm_state;
void fm_state_init(void);
void fm_state_add_to_history(const char *path);
int fm_state_can_undo(void);
int fm_state_can_redo(void);
void fm_state_sort_files(fm_sort_mode_t mode);
void fm_state_filter_files(void);
void fm_state_select_all(void);
void fm_state_deselect_all(void);
void fm_set_status_message(const char *msg);
void fm_state_toggle_select(int index);
void fm_state_update_sidebar(void);

/* sort.c */
int fm_sort_by_name(const fm_file_t *a, const fm_file_t *b);
int fm_sort_by_size(const fm_file_t *a, const fm_file_t *b);
int fm_sort_by_date(const fm_file_t *a, const fm_file_t *b);
int fm_sort_by_type(const fm_file_t *a, const fm_file_t *b);
void fm_qsort(fm_file_t *arr, int n, int (*cmp)(const fm_file_t *, const fm_file_t *));

#endif /* _NEXS_FM_H */
