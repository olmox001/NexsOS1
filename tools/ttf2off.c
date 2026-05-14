/*
 * tools/ttf2off.c
 * Host tool to convert TTF to OS1 Font Format (.off)
 * Self-contained to avoid header conflicts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define FONT_MAGIC 0x31534F // "OS1"

struct font_glyph_info {
  int16_t x0, y0;        /* Bitmap position offset */
  uint8_t width, height; /* Bitmap dimensions */
  int16_t advance;       /* Horizontal advance */
  uint32_t data_offset;  /* Offset into bitmap data */
};

struct font_header {
  uint32_t magic;
  uint16_t size;
  uint16_t first_char;
  uint16_t num_chars;
  uint16_t ascent;
  uint16_t descent;
  uint32_t bitmap_size;
};

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <font.ttf> <size> <output.off>\n", argv[0]);
        return 1;
    }

    const char *ttf_path = argv[1];
    int font_size = atoi(argv[2]);
    const char *out_path = argv[3];

    FILE *f = fopen(ttf_path, "rb");
    if (!f) { perror("fopen ttf"); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    uint8_t *ttf_data = malloc(size);
    if (!ttf_data) { fprintf(stderr, "malloc failed\n"); return 1; }
    fread(ttf_data, 1, size, f);
    fclose(f);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf_data, 0)) {
        fprintf(stderr, "stbtt_InitFont failed\n");
        return 1;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, (float)font_size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    int start_char = 32;
    int num_chars = 95;

    struct font_header h;
    memset(&h, 0, sizeof(h));
    h.magic = FONT_MAGIC;
    h.first_char = start_char;
    h.num_chars = num_chars;
    h.ascent = (int)(ascent * scale);
    h.descent = (int)(-descent * scale);
    
    struct font_glyph_info *glyphs = malloc(num_chars * sizeof(struct font_glyph_info));
    uint8_t **bitmaps = malloc(num_chars * sizeof(uint8_t *));
    int *widths = malloc(num_chars * sizeof(int));
    int *heights = malloc(num_chars * sizeof(int));
    uint32_t total_bitmap_size = 0;

    for (int i = 0; i < num_chars; i++) {
        int cp = start_char + i;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
        glyphs[i].advance = (int)(adv * scale);
        int x0, y0;
        bitmaps[i] = stbtt_GetCodepointBitmap(&font, scale, scale, cp, &widths[i], &heights[i], &x0, &y0);
        glyphs[i].x0 = (int16_t)x0;
        glyphs[i].y0 = (int16_t)y0;
        glyphs[i].width = (uint8_t)widths[i];
        glyphs[i].height = (uint8_t)heights[i];
        glyphs[i].data_offset = total_bitmap_size;
        total_bitmap_size += widths[i] * heights[i];
    }
    h.bitmap_size = total_bitmap_size;

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("fopen out"); return 1; }

    fwrite(&h, 1, sizeof(h), out);
    fwrite(glyphs, 1, num_chars * sizeof(struct font_glyph_info), out);
    for (int i = 0; i < num_chars; i++) {
        if (bitmaps[i]) {
            fwrite(bitmaps[i], 1, widths[i] * heights[i], out);
            stbtt_FreeBitmap(bitmaps[i], NULL);
        }
    }

    fclose(out);
    free(ttf_data);
    free(glyphs);
    free(bitmaps);
    free(widths);
    free(heights);
    printf("Successfully converted %s to %s (%u bytes bitmap)\n", ttf_path, out_path, total_bitmap_size);
    return 0;
}
