/*
 * user/lib/font_lib.c
 * Implementation of the standardized font library
 */
#include <font_lib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct font_ctx *font_load(const char *path) {
    int size = file_read(path, NULL, 0, 0);
    if (size <= 0) return NULL;

    void *data = malloc(size);
    if (!data) return NULL;

    if (file_read(path, data, size, 0) != size) {
        free(data);
        return NULL;
    }

    struct font_header *h = (struct font_header *)data;
    if (h->magic != FONT_MAGIC) {
        free(data);
        return NULL;
    }

    struct font_ctx *ctx = malloc(sizeof(struct font_ctx));
    if (!ctx) {
        free(data);
        return NULL;
    }

    ctx->header = *h;
    ctx->glyphs = (struct font_glyph_info *)((uint8_t *)data + sizeof(struct font_header));
    ctx->bitmap = (uint8_t *)ctx->glyphs + h->num_chars * sizeof(struct font_glyph_info);
    ctx->raw_data = data;

    return ctx;
}

void font_free(struct font_ctx *ctx) {
    if (ctx) {
        if (ctx->raw_data) free(ctx->raw_data);
        free(ctx);
    }
}

static void draw_glyph(int win_id, struct font_ctx *ctx, int x, int y, uint32_t codepoint, uint32_t color) {
    int idx = (int)codepoint - ctx->header.first_char;
    if (idx < 0 || idx >= ctx->header.num_chars) return;

    struct font_glyph_info *gi = &ctx->glyphs[idx];
    uint8_t *bitmap = ctx->bitmap + gi->data_offset;

    int start_x = x + gi->x0;
    int start_y = y + ctx->header.ascent + gi->y0;

    /* 
     * Note: This is slow because it calls window_draw for each pixel.
     * Real apps should use window_blit with a local buffer.
     */
    for (int gy = 0; gy < gi->height; gy++) {
        for (int gx = 0; gx < gi->width; gx++) {
            uint8_t alpha = bitmap[gy * gi->width + gx];
            if (alpha > 128) { /* Simple threshold for now */
                window_draw(win_id, start_x + gx, start_y + gy, 1, 1, color);
            }
        }
    }
}

void font_draw_string(int win_id, struct font_ctx *ctx, int x, int y, const char *str, uint32_t color) {
    if (!ctx || !str) return;

    int cursor_x = x;
    uint32_t codepoint;
    int consumed;

    while (*str) {
        consumed = utf8_decode(str, &codepoint);
        if (consumed <= 0) {
            str++;
            continue;
        }
        draw_glyph(win_id, ctx, cursor_x, y, codepoint, color);
        int idx = (int)codepoint - ctx->header.first_char;
        if (idx >= 0 && idx < ctx->header.num_chars) {
            cursor_x += ctx->glyphs[idx].advance;
        }
        str += consumed;
    }
}

int font_string_width(struct font_ctx *ctx, const char *str) {
    if (!ctx || !str) return 0;

    int width = 0;
    uint32_t codepoint;
    int consumed;

    while (*str) {
        consumed = utf8_decode(str, &codepoint);
        if (consumed <= 0) {
            str++;
            continue;
        }
        int idx = (int)codepoint - ctx->header.first_char;
        if (idx >= 0 && idx < ctx->header.num_chars) {
            width += ctx->glyphs[idx].advance;
        }
        str += consumed;
    }
    return width;
}
