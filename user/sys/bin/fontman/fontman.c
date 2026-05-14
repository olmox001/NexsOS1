#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <font.h>
#include <font.h>

/* stb_truetype needs some math. Since we are freestanding, we provide basics. */
#define STBTT_ifloor(x)   ((int)(x))
#define STBTT_iceil(x)    ((int)((x) + 0.999999f))
#define STBTT_sqrt(x)     sqrt(x)
#define STBTT_pow(x,y)    pow(x,y)
#define STBTT_fmod(x,y)   fmod(x,y)
#define STBTT_cos(x)      cos(x)
#define STBTT_acos(x)     acos(x)
#define STBTT_fabs(x)     ((x) < 0 ? -(x) : (x))

/* Simple math implementations for stb_truetype */
static double sqrt(double n) {
    if (n < 0) return 0;
    double x = n;
    double y = 1;
    double e = 0.000001;
    while (x - y > e) {
        x = (x + y) / 2;
        y = n / x;
    }
    return x;
}

static double pow(double base, double exp) {
    if (exp == 0) return 1;
    if (exp == 1) return base;
    double res = 1;
    for (int i = 0; i < (int)exp; i++) res *= base;
    return res;
}

static double fmod(double x, double y) {
    return x - (int)(x/y) * y;
}

/* Taylor series for cos */
static double cos(double x) {
    double res = 1;
    double term = 1;
    for (int i = 1; i < 10; i++) {
        term *= -x * x / (2 * i * (2 * i - 1));
        res += term;
    }
    return res;
}

/* acos approximation */
static double acos(double x) {
    if (x < -1) x = -1;
    if (x > 1) x = 1;
    /* Very rough approximation for font rasterization */
    return (1.0 - x) * (3.14159265 / 2.0);
}

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../../../tools/stb_truetype.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <font.ttf> <size>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int size = atoi(argv[2]);

    printf("FontMan: Loading %s at size %d...\n", path, size);

    /* Read TTF file */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("Error: Could not open font file %s\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long ttf_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    unsigned char *ttf_buffer = malloc(ttf_size);
    fread(ttf_buffer, 1, ttf_size, fp);
    fclose(fp);

    /* Init stb_truetype */
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf_buffer, stbtt_GetFontOffsetForIndex(ttf_buffer, 0))) {
        printf("Error: stbtt_InitFont failed\n");
        return 1;
    }

    int first_char = 32;
    int num_chars = 224; // 32 to 255
    
    struct font_glyph_info *glyphs = malloc(sizeof(struct font_glyph_info) * num_chars);
    uint32_t bitmap_capacity = 64 * 1024;
    uint8_t *bitmap = malloc(bitmap_capacity);
    uint32_t bitmap_offset = 0;

    float scale = stbtt_ScaleForPixelHeight(&font, (float)size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    for (int i = 0; i < num_chars; i++) {
        int codepoint = first_char + i;
        int w, h, xoff, yoff;
        unsigned char *pixels = stbtt_GetCodepointBitmap(&font, 0, scale, codepoint, &w, &h, &xoff, &yoff);

        if (bitmap_offset + (w * h) > bitmap_capacity) {
            bitmap_capacity *= 2;
            bitmap = realloc(bitmap, bitmap_capacity);
        }

        int advance;
        stbtt_GetCodepointHMetrics(&font, codepoint, &advance, NULL);

        glyphs[i].x0 = (int16_t)xoff;
        glyphs[i].y0 = (int16_t)yoff;
        glyphs[i].width = (uint8_t)w;
        glyphs[i].height = (uint8_t)h;
        glyphs[i].advance = (int16_t)(advance * scale);
        glyphs[i].data_offset = bitmap_offset;

        if (pixels) {
            memcpy(bitmap + bitmap_offset, pixels, w * h);
            stbtt_FreeBitmap(pixels, NULL);
            bitmap_offset += (w * h);
        }
    }

    /* Prepare Header */
    struct font_header header;
    header.magic = FONT_MAGIC;
    header.size = (uint16_t)size;
    header.first_char = (uint16_t)first_char;
    header.num_chars = (uint16_t)num_chars;
    header.ascent = (uint16_t)(ascent * scale);
    header.descent = (uint16_t)(-descent * scale);
    header.bitmap_size = bitmap_offset;

    /* Pack everything into a single buffer */
    size_t total_size = sizeof(header) + (sizeof(struct font_glyph_info) * num_chars) + bitmap_offset;
    void *final_data = malloc(total_size);
    
    uint8_t *ptr = (uint8_t *)final_data;
    memcpy(ptr, &header, sizeof(header)); ptr += sizeof(header);
    memcpy(ptr, glyphs, sizeof(struct font_glyph_info) * num_chars); ptr += sizeof(struct font_glyph_info) * num_chars;
    memcpy(ptr, bitmap, bitmap_offset);

    printf("FontMan: Packed font size %zu bytes. Calling set_font...\n", total_size);
    int res = set_font(final_data, total_size);
    printf("FontMan: Syscall returned %d\n", res);

    if (res == 0) {
        printf("FontMan: SUCCESS! System font updated.\n");
    } else {
        printf("FontMan: FAILED with error %d\n", res);
    }

    /* Keep it in memory if the kernel just points to it (for now, as per sys_set_font hack) */
    while(1) yield();

    return 0;
}
