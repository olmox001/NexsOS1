/*
 * kernel/graphics/compositor_style.c
 * Compositor appearance presets (GFX-DYN-01 / DIR-07).
 *
 * Policy / Style / Theme / Background are kept separate (see
 * compositor_style.h). This file holds the built-in presets and the active
 * selection.  The compositor reads
 * compositor_style_active()/compositor_theme_active()/
 * compositor_background_active() each frame; switching a preset marks the whole
 * desktop dirty so the change shows immediately.
 */
#include <kernel/compositor_style.h>

/* Built-in styles, indexed by STYLE_* ids. */
static const compositor_style_t styles[STYLE_COUNT] = {

    /* STYLE_NEXS — */
    [STYLE_NEXS] = {.titlebar = 1,
                    .window_borders = 0,
                    .rounded_corners = 1,
                    .shadows = 1,
                    .animations = 0,
                    .titlebar_height = 24,
                    .border_radius = 10,
                    .button_shape = 2,
                    .button_side = 1,
                    .shadow_size = 6,
                    .shadow_type = 1},
    /* STYLE_CLASSIC — X11/Motif: square, no shadow . */
    [STYLE_CLASSIC] = {.titlebar = 1,
                       .window_borders = 1,
                       .rounded_corners = 1,
                       .shadows = 1,
                       .animations = 0,
                       .titlebar_height = 20,
                       .border_radius = 10,
                       .button_shape = 0,
                       .button_side = 1,
                       .shadow_size = 4,
                       .shadow_type = 1},
    /* STYLE_MATERIAL — Android/Material 3: rounded, soft shadow, animations. */
    [STYLE_MATERIAL] = {.titlebar = 1,
                        .window_borders = 1,
                        .rounded_corners = 1,
                        .shadows = 1,
                        .animations = 1,
                        .titlebar_height = 24,
                        .border_radius = 12,
                        .button_shape = 2,
                        .button_side = 1,
                        .shadow_size = 6,
                        .shadow_type = 2},
    /* STYLE_GLASS — macOS/Win11: rounded, thin border, blur (future). */
    [STYLE_GLASS] = {.titlebar = 1,
                     .window_borders = 0,
                     .rounded_corners = 1,
                     .shadows = 1,
                     .animations = 1,
                     .titlebar_height = 22,
                     .border_radius = 10,
                     .button_shape = 0,
                     .button_side = 0,
                     .shadow_size = 6,
                     .shadow_type = 2},
    /* STYLE_MINIMAL — */
    [STYLE_MINIMAL] = {.titlebar = 1,
                       .window_borders = 0,
                       .rounded_corners = 1,
                       .shadows = 0,
                       .animations = 0,
                       .titlebar_height = 20,
                       .border_radius = 10,
                       .button_shape = 2,
                       .button_side = 1,
                       .shadow_size = 0,
                       .shadow_type = 0},
    /* STYLE_RETRO — CDE/Win95: bold square borders. */
    [STYLE_RETRO] = {.titlebar = 1,
                     .window_borders = 1,
                     .rounded_corners = 0,
                     .shadows = 1,
                     .animations = 0,
                     .titlebar_height = 20,
                     .border_radius = 10,
                     .button_shape = 1,
                     .button_side = 1,
                     .shadow_size = 4,
                     .shadow_type = 0},

};

/* Built-in themes, indexed by THEME_* ids. */
static const compositor_theme_t themes[THEME_COUNT] = {
    /* THEME_LIGHT — the existing macOS-style palette (default; preserves the
     * current appearance exactly). */
    [THEME_LIGHT] = {.win_bg = 0xFFFCFCFD,
                     .title_active = 0xFFEFEFF4,
                     .title_inactive = 0xFFE5E5EA,
                     .title_text_active = 0xFF000000,
                     .title_text_inactive = 0xFF8E8E93,
                     .close_btn = 0xFFFF5F57,
                     .border = 0xFFD1D1D6,
                     .accent = 0xFF007AFF},
    /* THEME_DARK — dark palette. */
    [THEME_DARK] = {.win_bg = 0xFFFCFCFD,
                    .title_active = 0xFF2C2C2E,
                    .title_inactive = 0xFF1F1F22,
                    .title_text_active = 0xFFF2F2F7,
                    .title_text_inactive = 0xFF8E8E93,
                    .close_btn = 0xFFFF5F57,
                    .border = 0xFF3A3A3C,
                    .accent = 0xFF0A84FF},
};

/* Built-in backgrounds, indexed by BG_* ids.  Same logic the desktop
 * gradient used back when it lived on compositor_theme_t (bg_top/bg_bottom
 * as plain literal colours, no derivation formula) — just one triplet per
 * background instead of one shared pair for both themes.  Every entry is a
 * real two-stop gradient (darker/muted top, richer/lighter bottom). */
static const compositor_background_t backgrounds[BG_COUNT] = {
    [BG_BLACK] = {.bg_color = 0xFF121212,
                  .bg_top = 0xFF121212,
                  .bg_bottom = 0xFF181818}, /* near-black anchor */
    [BG_RED] = {.bg_color = 0xFFB33A3A,
                .bg_top = 0xFF933030,
                .bg_bottom = 0xFFFF5454}, /* terracotta */
    [BG_GREEN] = {.bg_color = 0xFF3E8E5B,
                  .bg_top = 0xFF33744B,
                  .bg_bottom = 0xFF5ACE84}, /* forest emerald */
    [BG_YELLOW] = {.bg_color = 0xFFC9A227,
                   .bg_top = 0xFFA58520,
                   .bg_bottom = 0xFFFFEB39}, /* amber gold */
    /* the original desktop gradient (was on compositor_theme_t); default
       background. */
    [BG_BLUE] = {.bg_color = 0xFF143060,
                 .bg_top = 0xFF142850,
                 .bg_bottom = 0xFF1450A0}, /* nexs */
    [BG_MAGENTA] = {.bg_color = 0xFF8E44AD,
                    .bg_top = 0xFF74388E,
                    .bg_bottom = 0xFFCE63FB}, /* amethyst */
    [BG_CYAN] = {.bg_color = 0xFF2A9D8F,
                 .bg_top = 0xFF228175,
                 .bg_bottom = 0xFF3DE4CF}, /* teal */
    [BG_WHITE] = {.bg_color = 0xFFD8D8DC,
                  .bg_top = 0xFFB1B1B4,
                  .bg_bottom = 0xFFFFFFFF}, /* soft off-white */
    [BG_GRAY] = {.bg_color = 0xFF4A4A52,
                 .bg_top = 0xFF3D3D43,
                 .bg_bottom = 0xFF6B6B77}, /* cool slate */
    [BG_BRIGHT_RED] = {.bg_color = 0xFFE8604F,
                       .bg_top = 0xFFBE4F41,
                       .bg_bottom = 0xFFFF8B73}, /* coral */
    [BG_BRIGHT_GREEN] = {.bg_color = 0xFF52B788,
                         .bg_top = 0xFF439670,
                         .bg_bottom = 0xFF77FFC5}, /* mint */
    [BG_BRIGHT_YELLOW] = {.bg_color = 0xFFF2C94C,
                          .bg_top = 0xFFC6A53E,
                          .bg_bottom = 0xFFFFFF6E}, /* soft gold */
    [BG_BRIGHT_BLUE] = {.bg_color = 0xFF4A78C0,
                        .bg_top = 0xFF3D629D,
                        .bg_bottom =
                            0xFF6BAEFF}, /* sky, distinct from nexs blue */
    [BG_BRIGHT_MAGENTA] = {.bg_color = 0xFFB07CD1,
                           .bg_top = 0xFF9066AB,
                           .bg_bottom = 0xFFFFB4FF}, /* orchid */
    [BG_BRIGHT_CYAN] = {.bg_color = 0xFF56C9C9,
                        .bg_top = 0xFF47A5A5,
                        .bg_bottom = 0xFF7DFFFF}, /* aqua */
    [BG_BRIGHT_WHITE] = {.bg_color = 0xFFF5F5F7,
                         .bg_top = 0xFFC9C9CB,
                         .bg_bottom = 0xFFFFFFFF}, /* near white */
};

static int active_style = STYLE_MINIMAL;
static int active_theme = THEME_DARK;
static int active_background = BG_BLUE; /* default: the nexs gradient */

const compositor_style_t *compositor_style_active(void) {
  return &styles[active_style];
}

const compositor_theme_t *compositor_theme_active(void) {
  return &themes[active_theme];
}

const compositor_background_t *compositor_background_active(void) {
  return &backgrounds[active_background];
}

int compositor_titlebar_height(void) {
  const compositor_style_t *s = &styles[active_style];
  return s->titlebar ? s->titlebar_height : 0;
}

int compositor_set_style(int style_id) {
  if (style_id < 0 || style_id >= STYLE_COUNT)
    return -1;
  active_style = style_id;
  compositor_full_damage(); /* re-render with the new chrome */
  return 0;
}

int compositor_set_theme(int theme_id) {
  if (theme_id < 0 || theme_id >= THEME_COUNT)
    return -1;
  active_theme = theme_id;
  compositor_full_damage();
  return 0;
}

int compositor_set_background(int bg_id) {
  if (bg_id < 0 || bg_id >= BG_COUNT)
    return -1;
  active_background = bg_id;
  compositor_full_damage();
  return 0;
}
