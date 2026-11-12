#ifndef OS1_MATERIAL_COLORS_H
#define OS1_MATERIAL_COLORS_H
/* Shared Material-inspired neutral surface tokens. All ARGB8888.
 * Distinct planes: window surface/chrome, TTY text-field, text and caret.
 * Theme and desktop background are orthogonal. No kernel-specific ABI. */
#include <stdint.h>

#define OS1_MATERIAL_WINDOW_LIGHT 0xFFF5F3EFU
#define OS1_MATERIAL_WINDOW_DARK 0xFF25272CU
#define OS1_MATERIAL_TTY_LIGHT 0xFFF1EFEAU
#define OS1_MATERIAL_TTY_DARK 0xFF212329U
#define OS1_MATERIAL_TEXT_LIGHT 0xFF303238U
#define OS1_MATERIAL_TEXT_DARK 0xFFE8E6E0U
#define OS1_MATERIAL_PROMPT_LIGHT 0xFF426AA9U
#define OS1_MATERIAL_PROMPT_DARK 0xFF8CD3ACU
#define OS1_MATERIAL_CARET_LIGHT 0xFF456DB2U
#define OS1_MATERIAL_CARET_DARK 0xFF97BDE5U

/* Neutral desktop backgrounds: warm off-whites and a lighter tonal gray.
 * The other fourteen coloured accents retain their established identities. */
#define OS1_MATERIAL_BG_WHITE 0xFFECEAE5U
#define OS1_MATERIAL_BG_GRAY 0xFFBEC1BEU
#define OS1_MATERIAL_BG_BRIGHT_WHITE 0xFFF7F5F0U

struct os1_material_tty_palette {
  uint32_t window_surface;
  uint32_t text_background;
  uint32_t text_foreground;
  uint32_t prompt;
  uint32_t caret;
};

static inline struct os1_material_tty_palette
os1_material_tty_palette(int light) {
  struct os1_material_tty_palette result;
  result.window_surface = light ? OS1_MATERIAL_WINDOW_LIGHT
                                : OS1_MATERIAL_WINDOW_DARK;
  result.text_background = light ? OS1_MATERIAL_TTY_LIGHT
                                 : OS1_MATERIAL_TTY_DARK;
  result.text_foreground = light ? OS1_MATERIAL_TEXT_LIGHT
                                 : OS1_MATERIAL_TEXT_DARK;
  result.prompt = light ? OS1_MATERIAL_PROMPT_LIGHT
                        : OS1_MATERIAL_PROMPT_DARK;
  result.caret = light ? OS1_MATERIAL_CARET_LIGHT
                       : OS1_MATERIAL_CARET_DARK;
  return result;
}
#endif
