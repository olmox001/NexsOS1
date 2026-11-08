#ifndef OS1_THEME_H
#define OS1_THEME_H

#include <stdint.h>

typedef struct os1_compositor_style {
  int titlebar, window_borders, rounded_corners, shadows, animations;
  int titlebar_height, border_radius, shadow_size, shadow_type;
  int button_shape, button_side;
} os1_compositor_style_t;
typedef struct os1_compositor_theme {
  uint32_t win_bg, title_active, title_inactive;
  uint32_t title_text_active, title_text_inactive;
  uint32_t close_btn, border, accent;
} os1_compositor_theme_t;
typedef struct os1_compositor_background {
  uint32_t bg_color, bg_top, bg_bottom;
} os1_compositor_background_t;

enum { OS1_STYLE_NEXS, OS1_STYLE_CLASSIC, OS1_STYLE_MATERIAL,
       OS1_STYLE_GLASS, OS1_STYLE_MINIMAL, OS1_STYLE_RETRO, OS1_STYLE_COUNT };
enum { OS1_THEME_LIGHT, OS1_THEME_DARK, OS1_THEME_COUNT };
enum { OS1_BG_BLACK, OS1_BG_RED, OS1_BG_GREEN, OS1_BG_YELLOW, OS1_BG_BLUE,
       OS1_BG_MAGENTA, OS1_BG_CYAN, OS1_BG_WHITE, OS1_BG_GRAY,
       OS1_BG_BRIGHT_RED, OS1_BG_BRIGHT_GREEN, OS1_BG_BRIGHT_YELLOW,
       OS1_BG_BRIGHT_BLUE, OS1_BG_BRIGHT_MAGENTA, OS1_BG_BRIGHT_CYAN,
       OS1_BG_BRIGHT_WHITE, OS1_BG_COUNT };

const os1_compositor_style_t *os1_theme_style_active(void);
const os1_compositor_theme_t *os1_theme_palette_active(void);
const os1_compositor_background_t *os1_theme_background_active(void);
int os1_theme_titlebar_height(void);
int os1_theme_set_style(int style_id);
int os1_theme_set_palette(int theme_id);
int os1_theme_set_background(int background_id);
uint32_t os1_theme_generation(void);

#endif
