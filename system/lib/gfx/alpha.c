/* Scalar ARGB compositing foundation. No allocation, device access or ISA
 * extension. Each operation has a constant, format-derived work bound.
 * Existing SDK surfaces remain straight-alpha; premultiplied buffers use
 * the explicitly named operations, never an implicit format conversion. */
#include <os1_gfx.h>

static uint32_t straight_channel(uint32_t source, uint32_t destination,
                                  uint32_t source_weight,
                                  uint32_t destination_weight) {
  return (source * source_weight + destination * destination_weight) /
         (source_weight + destination_weight);
}

static uint32_t opaque_over(uint32_t source, uint32_t destination,
                            uint32_t alpha) {
  uint32_t inverse = 255 - alpha;
  uint32_t red = (((source >> 16) & 255) * alpha +
                  ((destination >> 16) & 255) * inverse) / 255;
  uint32_t green = (((source >> 8) & 255) * alpha +
                    ((destination >> 8) & 255) * inverse) / 255;
  uint32_t blue = ((source & 255) * alpha + (destination & 255) * inverse) / 255;
  return UINT32_C(0xff000000) | (red << 16) | (green << 8) | blue;
}

uint32_t os1_gfx_argb_over(uint32_t source, uint32_t destination) {
  uint32_t alpha = source >> 24;
  if (alpha == 0) return destination;
  if (alpha == 255 || (destination >> 24) == 0) return source;
  if ((destination >> 24) == 255) return opaque_over(source, destination, alpha);
  uint32_t source_weight = alpha * 255;
  uint32_t destination_weight = (destination >> 24) * (255 - alpha);
  uint32_t red = straight_channel((source >> 16) & 255,
      (destination >> 16) & 255, source_weight, destination_weight);
  uint32_t green = straight_channel((source >> 8) & 255,
      (destination >> 8) & 255, source_weight, destination_weight);
  uint32_t blue = straight_channel(source & 255, destination & 255,
      source_weight, destination_weight);
  return ((source_weight + destination_weight) / 255 << 24) |
         (red << 16) | (green << 8) | blue;
}

uint32_t os1_gfx_argb_premultiply(uint32_t source) {
  uint32_t alpha = source >> 24;
  uint32_t red = ((source >> 16) & 255) * alpha / 255;
  uint32_t green = ((source >> 8) & 255) * alpha / 255;
  uint32_t blue = (source & 255) * alpha / 255;
  return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

static uint32_t premult_channel(uint32_t source, uint32_t destination,
                                uint32_t inverse, uint32_t alpha) {
  uint32_t result = source + destination * inverse / 255;
  /* Malformed premultiplied input cannot carry a channel into its neighbour. */
  return result > alpha ? alpha : result;
}

uint32_t os1_gfx_premult_over(uint32_t source, uint32_t destination) {
  uint32_t inverse = 255 - (source >> 24);
  uint32_t alpha = (source >> 24) + (destination >> 24) * inverse / 255;
  uint32_t red = premult_channel((source >> 16) & 255,
      (destination >> 16) & 255, inverse, alpha);
  uint32_t green = premult_channel((source >> 8) & 255,
      (destination >> 8) & 255, inverse, alpha);
  uint32_t blue = premult_channel(source & 255, destination & 255, inverse, alpha);
  return (alpha << 24) | (red << 16) | (green << 8) | blue;
}
