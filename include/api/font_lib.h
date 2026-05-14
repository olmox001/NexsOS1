/*
 * include/api/font_lib.h
 * Standardized font library for user-space applications
 */
#ifndef _OS1_FONT_LIB_H
#define _OS1_FONT_LIB_H

#include <stdint.h>
#include <stddef.h>
#include <graphics.h>
#include <font.h>

struct font_ctx {
    struct font_header header;
    struct font_glyph_info *glyphs;
    uint8_t *bitmap;
    void *raw_data;
};

/* Load a .off font file */
struct font_ctx *font_load(const char *path);

/* Free font context */
void font_free(struct font_ctx *ctx);

/* Draw text using a specific font context */
void font_draw_string(int win_id, struct font_ctx *ctx, int x, int y, const char *str, uint32_t color);

/* Get string width with a specific font */
int font_string_width(struct font_ctx *ctx, const char *str);

#endif
