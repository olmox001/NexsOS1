/*
 * tools/mkfont.c
 * Host tool to generate a C header with rasterized font data from a TTF
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <font.ttf> <size> <output.h>\n", argv[0]);
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
    int num_chars = 95; // Basic ASCII

    FILE *out = fopen(out_path, "w");
    if (!out) { perror("fopen out"); return 1; }

    fprintf(out, "/* Auto-generated font data from %s */\n", ttf_path);
    fprintf(out, "#include <stdint.h>\n\n");
    
    fprintf(out, "#define FONT_SIZE %d\n", font_size);
    fprintf(out, "#define FONT_FIRST_CHAR %d\n", start_char);
    fprintf(out, "#define FONT_NUM_CHARS %d\n", num_chars);
    fprintf(out, "#define FONT_ASCENT %d\n", (int)(ascent * scale));
    fprintf(out, "#define FONT_DESCENT %d\n\n", (int)(-descent * scale));

    /* We rely on the struct font_glyph_info defined in include/api/font.h */

    uint8_t *bitmaps[95];
    int widths[95], heights[95], xoffs[95], yoffs[95];
    int advances[95];
    uint32_t total_bitmap_size = 0;

    for (int i = 0; i < num_chars; i++) {
        int cp = start_char + i;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
        advances[i] = (int)(adv * scale);

        bitmaps[i] = stbtt_GetCodepointBitmap(&font, scale, scale, cp, &widths[i], &heights[i], &xoffs[i], &yoffs[i]);
        total_bitmap_size += widths[i] * heights[i];
    }

    fprintf(out, "static const struct font_glyph_info font_glyphs[FONT_NUM_CHARS] = {\n");
    uint32_t current_offset = 0;
    for (int i = 0; i < num_chars; i++) {
        fprintf(out, "    {%d, %d, %d, %d, %d, %u}, /* '%c' */\n", 
                xoffs[i], yoffs[i], widths[i], heights[i], advances[i], current_offset, (start_char + i));
        current_offset += widths[i] * heights[i];
    }
    fprintf(out, "};\n\n");

    fprintf(out, "static const uint8_t font_bitmap[] = {\n");
    for (int i = 0; i < num_chars; i++) {
        if (bitmaps[i]) {
            for (int j = 0; j < widths[i] * heights[i]; j++) {
                fprintf(out, "0x%02x, ", bitmaps[i][j]);
                if ((j + 1) % 16 == 0) fprintf(out, "\n");
            }
            stbtt_FreeBitmap(bitmaps[i], NULL);
        }
    }
    fprintf(out, "\n};\n");

    fclose(out);
    free(ttf_data);
    return 0;
}
