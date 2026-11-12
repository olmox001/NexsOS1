#ifndef _API_FONT_H
#define _API_FONT_H

#include <stdint.h>

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

/* 
 * The binary format on disk/memory is:
 * [struct font_header]
 * [struct font_glyph_info * num_chars]
 * [uint8_t * bitmap_size]
 */

#endif
