/*
 * kernel/graphics/compositor_style.c
 * Compositor appearance presets (GFX-DYN-01 / DIR-07).
 *
 * Policy / Style / Theme are kept separate (see compositor_style.h).  This file
 * holds the built-in presets and the active selection.  The compositor reads
 * compositor_style_active()/compositor_theme_active() each frame; switching a
 * preset marks the whole desktop dirty so the change shows immediately.
 *
 * v1 wires titlebar presence + height (Style) and the full colour palette
 * (Theme) into the renderer.  border/shadow/rounded drawing are modelled here
 * but not yet rasterised — documented render hooks for the desktop/mobile
 * transform (DIR-07).
 */
#include <kernel/compositor_style.h>

/* Built-in styles, indexed by STYLE_* ids. */
static const compositor_style_t styles[STYLE_COUNT] = {
    /* STYLE_CLASSIC — X11/Motif: square, no shadow (the NEXS default look). */
    [STYLE_CLASSIC] = {.titlebar = 1,
                       .window_borders = 0,
                       .rounded_corners = 0,
                       .shadows = 0,
                       .animations = 0,
                       .titlebar_height = 20,
                       .border_radius = 0,
                       .shadow_size = 0},
    /* STYLE_MATERIAL — Android/Material 3: rounded, soft shadow, animations. */
    [STYLE_MATERIAL] = {.titlebar = 1,
                        .window_borders = 0,
                        .rounded_corners = 1,
                        .shadows = 1,
                        .animations = 1,
                        .titlebar_height = 24,
                        .border_radius = 8,
                        .shadow_size = 6},
    /* STYLE_GLASS — macOS/Win11: rounded, thin border, blur (future). */
    [STYLE_GLASS] = {.titlebar = 1,
                     .window_borders = 1,
                     .rounded_corners = 1,
                     .shadows = 1,
                     .animations = 1,
                     .titlebar_height = 22,
                     .border_radius = 10,
                     .shadow_size = 4},
    /* STYLE_MINIMAL — tiling WM: no chrome at all. */
    [STYLE_MINIMAL] = {.titlebar = 0,
                       .window_borders = 0,
                       .rounded_corners = 0,
                       .shadows = 0,
                       .animations = 0,
                       .titlebar_height = 0,
                       .border_radius = 0,
                       .shadow_size = 0},
    /* STYLE_RETRO — CDE/Win95: bold square borders. */
    [STYLE_RETRO] = {.titlebar = 1,
                     .window_borders = 1,
                     .rounded_corners = 0,
                     .shadows = 0,
                     .animations = 0,
                     .titlebar_height = 20,
                     .border_radius = 0,
                     .shadow_size = 0},
};

/* Built-in themes, indexed by THEME_* ids. */
static const compositor_theme_t themes[THEME_COUNT] = {
    /* THEME_LIGHT — the existing macOS-style palette (default; preserves the
     * current appearance exactly). */
    [THEME_LIGHT] = {.bg_top = 0xFF142850,
                     .bg_bottom = 0xFF1450A0,
                     .win_bg = 0xFFFCFCFD,
                     .title_active = 0xFFEFEFF4,
                     .title_inactive = 0xFFE5E5EA,
                     .title_text_active = 0xFF000000,
                     .title_text_inactive = 0xFF8E8E93,
                     .close_btn = 0xFFFF5F57,
                     .border = 0xFFD1D1D6,
                     .accent = 0xFF007AFF},
    /* THEME_DARK — dark palette. */
    [THEME_DARK] = {.bg_top = 0xFF101015,
                    .bg_bottom = 0xFF1C1C2A,
                    .win_bg = 0xFF1C1C1E,
                    .title_active = 0xFF2C2C2E,
                    .title_inactive = 0xFF1F1F22,
                    .title_text_active = 0xFFF2F2F7,
                    .title_text_inactive = 0xFF8E8E93,
                    .close_btn = 0xFFFF5F57,
                    .border = 0xFF3A3A3C,
                    .accent = 0xFF0A84FF},
};

static int active_style = STYLE_CLASSIC;
static int active_theme = THEME_LIGHT;

const compositor_style_t *compositor_style_active(void) {
  return &styles[active_style];
}

const compositor_theme_t *compositor_theme_active(void) {
  return &themes[active_theme];
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
