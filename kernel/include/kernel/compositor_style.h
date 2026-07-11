/*
 * kernel/include/kernel/compositor_style.h
 * Compositor appearance model (GFX-DYN-01 / DIR-07).
 *
 * Four orthogonal concerns, deliberately separated (the maintainer's model):
 *
 *   Policy     = behaviour   (floating / tiled / mobile window management)
 *   Style      = form        (titlebar? borders? rounded? shadows? sizes)
 *   Theme      = colours     (window palette: titlebar, text, accent)
 *   Background = desktop     (base colour + gradient reference colours)
 *
 * Style and Theme never touch the scene graph, surfaces, input or buffers —
 * they only change HOW the existing windows are rendered.  The compositor reads
 * the active style/theme/background each frame; switching them re-renders with
 * no other state change.
 */
#ifndef _KERNEL_COMPOSITOR_STYLE_H
#define _KERNEL_COMPOSITOR_STYLE_H

#include <stdint.h>

/* Window-management policy (behaviour).  Tiling/mobile are hooks for the
 * desktop-vs-mobile transform; floating is the only one wired today. */
typedef enum {
  WINDOW_POLICY_FLOATING = 0,
  WINDOW_POLICY_TILED,
  WINDOW_POLICY_MOBILE,
} window_policy_t;

/* Style = form of the window chrome (no colours here). */
typedef struct compositor_style {
  int titlebar;        /* draw a title bar + close button                  */
  int window_borders;  /* draw a 1px border around windows                 */
  int rounded_corners; /* round window corners (border_radius)             */
  int shadows;         /* draw drop shadows                                */
  int animations;      /* enable open/close/move animations               */
  int titlebar_height; /* px; 0 ⇒ no chrome regardless of `titlebar`       */
  int border_radius;   /* px corner radius when rounded_corners            */
  int shadow_size;     /* px shadow spread when shadows                    */
  int shadow_type;     /* 0=solid win_bg (Retro), 1=fast SDF, 2=premium grad.  */
  int button_shape;    /* 0 = circle (macOS), 1 = square (Retro/Win95) */
  int button_side;     /* 0 = left, 1 = right */
} compositor_style_t;

/* Theme = colour palette (ARGB8888), windows only. */
typedef struct compositor_theme {
  uint32_t win_bg;              /* default window background                */
  uint32_t title_active;        /* focused title bar                        */
  uint32_t title_inactive;      /* unfocused title bar                      */
  uint32_t title_text_active;   /* focused title text                       */
  uint32_t title_text_inactive; /* unfocused title text                     */
  uint32_t close_btn;           /* close button                             */
  uint32_t border;              /* window border                            */
  uint32_t accent;              /* accent (caret/selection)                 */
} compositor_theme_t;

/* Background = base colour and gradient references (separate from theme). */
typedef struct compositor_background {
  uint32_t bg_color;            /* base / solid tint                        */
  uint32_t bg_top;              /* reference colour 1 (gradient top)        */
  uint32_t bg_bottom;           /* reference colour 2 (gradient bottom)     */
} compositor_background_t;

/* Preset identifiers (stable small ints — also the SYS_SET_STYLE args). */
enum {
  STYLE_NEXS = 0, /* NEXS default                     */
  STYLE_CLASSIC,  /* X11/Motif: square, no shadow                       */
  STYLE_MATERIAL, /* Android/Material 3: rounded, soft shadow           */
  STYLE_GLASS,    /* macOS/Win11: rounded, thin border                  */
  STYLE_MINIMAL,  /* tiling WM: no chrome                               */
  STYLE_RETRO,    /* CDE/Win95: bold square borders                     */
  STYLE_COUNT
};
enum { THEME_LIGHT = 0, THEME_DARK, THEME_COUNT };
/* xterm 16-colour-derived palette. BG_BLACK carries the old "dark" gradient
 * and BG_BLUE carries the old "nexs" gradient (the default background) — the
 * two used to be separate presets (BG_NEXS/BG_DARK) but duplicated colours
 * already present in this palette, so they were folded in here instead. */
enum {
  BG_BLACK = 0,
  BG_RED,
  BG_GREEN,
  BG_YELLOW,
  BG_BLUE, /* default background: the former "nexs" gradient */
  BG_MAGENTA,
  BG_CYAN,
  BG_WHITE,
  BG_GRAY,
  BG_BRIGHT_RED,
  BG_BRIGHT_GREEN,
  BG_BRIGHT_YELLOW,
  BG_BRIGHT_BLUE,
  BG_BRIGHT_MAGENTA,
  BG_BRIGHT_CYAN,
  BG_BRIGHT_WHITE,
  BG_COUNT
};

/* Active accessors (read each frame by the compositor). */
const compositor_style_t *compositor_style_active(void);
const compositor_theme_t *compositor_theme_active(void);
const compositor_background_t *compositor_background_active(void);

/* Select a preset by id; returns 0 on success, -1 if out of range.  Marks the
 * compositor dirty so the change is visible immediately. */
int compositor_set_style(int style_id);
int compositor_set_theme(int theme_id);
int compositor_set_background(int bg_id);

/* Effective title-bar height for a window: the active style's height, or 0 when
 * the style has no titlebar.  Single source of truth for chrome geometry. */
int compositor_titlebar_height(void);

/* Provided by the compositor: mark the whole desktop dirty so a style/theme
 * switch repaints immediately. */
void compositor_full_damage(void);

#endif /* _KERNEL_COMPOSITOR_STYLE_H */
