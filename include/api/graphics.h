#ifndef _GRAPHICS_H
#define _GRAPHICS_H

#include <stdint.h>
#include <stddef.h>

/**
 * Standard colors (ARGB)
 */
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_BLACK   0xFF000000
#define COLOR_RED     0xFFFF0000
#define COLOR_GREEN   0xFF00FF00
#define COLOR_BLUE    0xFF0000FF
#define COLOR_YELLOW  0xFFFFCC00

/**
 * Draw a solid rectangle in a window.
 */
void graphics_draw_rect(int win_id, int x, int y, int w, int h, uint32_t color);

/**
 * Blit a pixel buffer to a window.
 */
void graphics_blit(int win_id, int x, int y, int w, int h, const uint32_t *buffer);

/**
 * Draw text in a window using stb_easy_font.
 * Returns the number of pixels wide the text was.
 */
int graphics_draw_text(int win_id, int x, int y, const char *text, uint32_t color);

/**
 * Load an image from file using stb_image.
 * Returns a pointer to the pixel data (ARGB), and sets width/height.
 * Must be freed with free().
 */
uint32_t *graphics_load_image(const char *path, int *w, int *h);

#endif
