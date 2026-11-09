/* Userland ARGB8888 raster operations used by compositord and graphics APIs. */
#include <os1_gfx.h>

static uint32_t blend_over(uint32_t src, uint32_t dst) {
  uint32_t alpha = src >> 24;
  if (alpha == 255)
    return src;
  if (alpha == 0)
    return dst;
  uint32_t inverse = 255 - alpha;
  uint32_t red =
      (((src >> 16) & 255) * alpha + ((dst >> 16) & 255) * inverse) / 255;
  uint32_t green =
      (((src >> 8) & 255) * alpha + ((dst >> 8) & 255) * inverse) / 255;
  uint32_t blue =
      ((src & 255) * alpha + (dst & 255) * inverse) / 255;
  return 0xff000000u | (red << 16) | (green << 8) | blue;
}

int os1_gfx_surface_valid(const struct os1_gfx_surface *surface) {
  if (!surface || !surface->pixels || !surface->width || !surface->height ||
      surface->stride < surface->width || surface->width > 0x7fffffffU ||
      surface->height > 0x7fffffffU)
    return 0;
  if (surface->height - 1 > ((size_t)-1 - surface->width) / surface->stride)
    return 0;
  return (surface->height - 1) * surface->stride + surface->width <= surface->capacity;
}

void os1_gfx_clear(struct os1_gfx_surface *surface, uint32_t argb) {
  if (!os1_gfx_surface_valid(surface))
    return;
  for (size_t y = 0; y < surface->height; ++y) {
    uint32_t *row = surface->pixels + y * surface->stride;
    for (size_t x = 0; x < surface->width; ++x)
      row[x] = argb;
  }
}

void os1_gfx_fill_rect(struct os1_gfx_surface *surface,
                       const struct os1_gfx_rect *rect, uint32_t argb) {
  if (!os1_gfx_surface_valid(surface) || !rect || rect->width <= 0 ||
      rect->height <= 0)
    return;

  int64_t left = rect->x;
  int64_t top = rect->y;
  int64_t right = left + (int64_t)rect->width;
  int64_t bottom = top + (int64_t)rect->height;
  if (left < 0)
    left = 0;
  if (top < 0)
    top = 0;
  if (right > (int64_t)surface->width)
    right = (int64_t)surface->width;
  if (bottom > (int64_t)surface->height)
    bottom = (int64_t)surface->height;
  if (left >= right || top >= bottom)
    return;

  for (int64_t y = top; y < bottom; ++y) {
    uint32_t *pixel = surface->pixels + (size_t)y * surface->stride + left;
    for (int64_t x = left; x < right; ++x)
      *pixel++ = argb;
  }
}

void os1_gfx_composite_over(struct os1_gfx_surface *destination,
                            const struct os1_gfx_surface *source, int x,
                            int y) {
  if (!os1_gfx_surface_valid(destination) ||
      !os1_gfx_surface_valid(source))
    return;

  int64_t left = x < 0 ? -(int64_t)x : 0;
  int64_t top = y < 0 ? -(int64_t)y : 0;
  int64_t right = (int64_t)source->width;
  int64_t bottom = (int64_t)source->height;
  if ((int64_t)x + right > (int64_t)destination->width)
    right = (int64_t)destination->width - x;
  if ((int64_t)y + bottom > (int64_t)destination->height)
    bottom = (int64_t)destination->height - y;
  if (left >= right || top >= bottom)
    return;

  for (int64_t sy = top; sy < bottom; ++sy) {
    const uint32_t *src = source->pixels + (size_t)sy * source->stride + left;
    uint32_t *dst = destination->pixels +
                    (size_t)(sy + y) * destination->stride + (left + x);
    for (int64_t sx = left; sx < right; ++sx) {
      uint32_t source_pixel = *src++;
      uint32_t destination_pixel = *dst;
      *dst++ = blend_over(source_pixel, destination_pixel);
    }
  }
}

void os1_gfx_composite_scaled(struct os1_gfx_surface *destination,
                              const struct os1_gfx_surface *source, int x,
                              int y, int width, int height) {
  if (!os1_gfx_surface_valid(destination) ||
      !os1_gfx_surface_valid(source) || width <= 0 || height <= 0)
    return;
  if ((size_t)width == source->width && (size_t)height == source->height) {
    os1_gfx_composite_over(destination, source, x, y);
    return;
  }
  int64_t x0 = x < 0 ? 0 : x;
  int64_t y0 = y < 0 ? 0 : y;
  int64_t x1 = (int64_t)x + width;
  int64_t y1 = (int64_t)y + height;
  if (x1 > (int64_t)destination->width)
    x1 = (int64_t)destination->width;
  if (y1 > (int64_t)destination->height)
    y1 = (int64_t)destination->height;
  for (int64_t dy = y0; dy < y1; ++dy) {
    size_t sy = (size_t)(dy - y) * source->height / (size_t)height;
    for (int64_t dx = x0; dx < x1; ++dx) {
      size_t sx = (size_t)(dx - x) * source->width / (size_t)width;
      uint32_t *dst = destination->pixels + (size_t)dy * destination->stride +
                      (size_t)dx;
      uint32_t src = source->pixels[sy * source->stride + sx];
      *dst = blend_over(src, *dst);
    }
  }
}
