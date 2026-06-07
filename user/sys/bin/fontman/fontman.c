/*
 * user/sys/bin/fontman/fontman.c
 * Font Manager — TTF rasterizer and kernel font uploader
 *
 * Reads a TrueType font file, rasterizes the printable ASCII range
 * (codepoints 32–255) using stb_truetype, packs the glyph bitmap and
 * metrics into a flat buffer with a struct font_header, and transfers it
 * to the kernel via SYS_SET_FONT (#253).
 *
 * After the upload, fontman spins forever (while(1) yield()) because the
 * kernel's sys_set_font implementation stores a raw pointer into userland
 * heap memory rather than copying the data.  If fontman exited, the kernel
 * would subsequently dereference freed memory.  See USR-FONTMAN-01.
 *
 * Math stubs:
 *   stb_truetype requires sqrt, pow, fmod, cos, acos.  Since there is no
 *   freestanding libm, this file provides handwritten implementations.
 *   The acos() approximation is severely inaccurate outside [0,1]; see
 *   USR-FONTMAN-03 for the impact on curve subdivision.
 *
 * Known issues:
 *   USR-FONTMAN-01 (W3 MISSING) The kernel holds a raw pointer into this
 *                  process's heap; fontman cannot exit.  Fix: kernel must
 *                  copy the font buffer at sys_set_font time.
 *   USR-FONTMAN-02 (W2 BAD-IMPL) <font.h> is #included twice (lines 5–6);
 *                  harmless due to include guards but indicates copy-paste.
 *   USR-FONTMAN-03 (W2 BAD-IMPL) acos(x) is approximated as
 *                  (1-x)*(π/2), which is only correct at x=1 (acos(1)=0).
 *                  For all other inputs the error grows; stb_truetype uses
 *                  acos in Bezier arc subdivision — malformed glyphs result.
 *   USR-BLOAT-01/02 (W2 BAD-IMPL·PERF) fontman.elf is ~773KB (verified),
 *                  dominated by stb_truetype + stb_image in lib.o and DWARF.
 */
#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <font.h>
#include <font.h> /* NOTE(USR-FONTMAN-02): duplicate include; harmless with guards */

/* stb_truetype needs some math. Since we are freestanding, we provide basics.
 * Each STBTT_ macro redirects stb_truetype's math calls to our implementations
 * below.  STBTT_fabs uses an expression macro (no function call overhead). */
#define STBTT_ifloor(x)   ((int)(x))
#define STBTT_iceil(x)    ((int)((x) + 0.999999f))
#define STBTT_sqrt(x)     sqrt(x)
#define STBTT_pow(x,y)    pow(x,y)
#define STBTT_fmod(x,y)   fmod(x,y)
#define STBTT_cos(x)      cos(x)
#define STBTT_acos(x)     acos(x)
#define STBTT_fabs(x)     ((x) < 0 ? -(x) : (x))

/*
 * sqrt - Babylonian (Heron's) method square root.
 *
 * Converges when |x - y| <= epsilon (0.000001).  Returns 0 for negative
 * input to avoid undefined behaviour.  Precision is adequate for stb_truetype's
 * glyph scaling; not suitable for general-purpose use.
 */
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

/*
 * pow - integer-exponent power via repeated multiplication.
 *
 * Only handles non-negative integer exponents (exp is cast to int).
 * Fractional or negative exponents silently produce wrong results.
 * stb_truetype uses pow() for gamma correction; integer exponents suffice
 * for the font-rasterization path used here.
 */
static double pow(double base, double exp) {
    if (exp == 0) return 1;
    if (exp == 1) return base;
    double res = 1;
    for (int i = 0; i < (int)exp; i++) res *= base;
    return res;
}

/*
 * fmod - floating-point remainder via truncating division.
 *
 * Equivalent to x - trunc(x/y)*y.  Sufficient for stb_truetype's angle
 * wrapping operations.
 */
static double fmod(double x, double y) {
    return x - (int)(x/y) * y;
}

/*
 * cos - Taylor series cosine (9 terms).
 *
 * Converges well for small |x|.  For large angles stb_truetype is expected
 * to normalise its arguments before calling cos, so accuracy is adequate.
 */
static double cos(double x) {
    double res = 1;
    double term = 1;
    for (int i = 1; i < 10; i++) {
        term *= -x * x / (2 * i * (2 * i - 1));
        res += term;
    }
    return res;
}

/*
 * acos - ROUGH approximation: (1 - x) * (π/2).
 *
 * NOTE(USR-FONTMAN-03): This formula is only correct at x=1 (gives 0) and
 * x=0 (gives π/2).  For x in (-1, 1) the error grows; for x < 0 the result
 * is larger than π/2 but the true value should be in (π/2, π].  stb_truetype
 * uses acos() in arc-to-Bezier subdivision; severe approximation error will
 * produce malformed glyph outlines for characters with curved strokes.
 * Correct fix: implement Newton-Raphson or use a polynomial minimax
 * approximation.
 *
 * Input is clamped to [-1, 1] to avoid undefined domain.
 */
static double acos(double x) {
    if (x < -1) x = -1;
    if (x > 1) x = 1;
    /* Very rough approximation for font rasterization */
    return (1.0 - x) * (3.14159265 / 2.0);
}

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../../../tools/stb_truetype.h"

/*
 * main - fontman entry point.
 *
 * argv[1]: path to a TrueType (.ttf) font file.
 * argv[2]: desired pixel height (passed to stbtt_ScaleForPixelHeight).
 *
 * Steps:
 *   1. Open the TTF file via fopen/fseek/ftell/fread (lib.c FILE emulation).
 *   2. Initialise stb_truetype (stbtt_InitFont).
 *   3. Rasterise codepoints 32–255 (num_chars=224) into a dynamically grown
 *      bitmap buffer (initial 64KB, doubled via realloc on overflow).
 *   4. Collect per-glyph metrics (x0, y0, width, height, advance).
 *   5. Pack header + glyph_info array + raw bitmap into one contiguous buffer
 *      (final_data) and call set_font() -> SYS_SET_FONT (#253).
 *   6. Spin forever with yield() to keep final_data alive in the heap.
 *
 * NOTE(USR-FONTMAN-01): The kernel's sys_set_font stores a raw pointer to
 *   final_data in userland heap memory.  fontman must not exit; if it did
 *   the kernel would dereference freed memory on every subsequent text render.
 *   Correct fix: kernel should copy the font buffer at SYS_SET_FONT time.
 *
 * Returns 1 on argument or file error; never returns on success.
 */
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <font.ttf> <size>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int size = atoi(argv[2]);

    printf("FontMan: Loading %s at size %d...\n", path, size);

    /* Read TTF file into heap buffer via the lib.c FILE emulation layer. */
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

    /* Initialise stb_truetype font parser.
     * stbtt_GetFontOffsetForIndex handles TTC collections (index 0 = first font). */
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf_buffer, stbtt_GetFontOffsetForIndex(ttf_buffer, 0))) {
        printf("Error: stbtt_InitFont failed\n");
        return 1;
    }

    int first_char = 32;
    int num_chars = 224; /* Covers codepoints 32 (space) through 255 */

    struct font_glyph_info *glyphs = malloc(sizeof(struct font_glyph_info) * num_chars);
    /* Start with 64KB bitmap capacity; doubled each time a glyph would overflow. */
    uint32_t bitmap_capacity = 64 * 1024;
    uint8_t *bitmap = malloc(bitmap_capacity);
    uint32_t bitmap_offset = 0;  /* Current write position in the bitmap buffer */

    /* Compute scale factor so the font renders at the requested pixel height. */
    float scale = stbtt_ScaleForPixelHeight(&font, (float)size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    for (int i = 0; i < num_chars; i++) {
        int codepoint = first_char + i;
        int w, h, xoff, yoff;
        /* Rasterise one glyph; pixels is a heap-allocated alpha bitmap. */
        unsigned char *pixels = stbtt_GetCodepointBitmap(&font, 0, scale, codepoint, &w, &h, &xoff, &yoff);

        /* Grow bitmap buffer if this glyph would overflow it.
         * NOTE: realloc here can trigger the userland malloc coalesce path.
         * No overflow guard on bitmap_capacity*2 (no check for wrap). */
        if (bitmap_offset + (w * h) > bitmap_capacity) {
            bitmap_capacity *= 2;
            bitmap = realloc(bitmap, bitmap_capacity);
        }

        int advance;
        stbtt_GetCodepointHMetrics(&font, codepoint, &advance, NULL);

        /* Fill glyph metrics; advance is scaled from font units to pixels. */
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

    /* Prepare font_header; descent is stored as a positive value. */
    struct font_header header;
    header.magic = FONT_MAGIC;
    header.size = (uint16_t)size;
    header.first_char = (uint16_t)first_char;
    header.num_chars = (uint16_t)num_chars;
    header.ascent = (uint16_t)(ascent * scale);
    header.descent = (uint16_t)(-descent * scale);  /* descent is negative in font units */
    header.bitmap_size = bitmap_offset;

    /* Pack everything into a single contiguous buffer:
     *   [ font_header ][ font_glyph_info * num_chars ][ raw alpha bitmap ]
     * final_data is passed to the kernel and MUST remain allocated (see
     * USR-FONTMAN-01). */
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

    /* Keep it in memory if the kernel just points to it (for now, as per sys_set_font hack).
     * NOTE(USR-FONTMAN-01): This spin loop is mandatory under the current kernel
     * implementation.  The kernel's sys_set_font stores a raw pointer into
     * final_data; exiting fontman would free it and leave the kernel with a
     * dangling pointer, causing a use-after-free on every subsequent text render.
     * Fix: kernel should copy the buffer at SYS_SET_FONT time. */
    while(1) yield();

    return 0;
}
