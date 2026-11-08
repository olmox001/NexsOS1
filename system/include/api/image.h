#ifndef OS1_IMAGE_H
#define OS1_IMAGE_H

#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <graphics.h>
#include <os1.h>
#include <string.h>

typedef struct {
  int w;
  int h;
  uint32_t *pixels;
} os1_image_t;

#define OS1_IMAGE_OK 0
#define OS1_IMAGE_ERR_ARG      (-1)
#define OS1_IMAGE_ERR_IO       (-2)
#define OS1_IMAGE_ERR_FORMAT   (-3)
#define OS1_IMAGE_ERR_NOMEM    (-4)
#define OS1_IMAGE_ERR_LIMIT    (-5)

#define OS1_IMAGE_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define OS1_IMAGE_MAX_DIMENSION  4096
#define OS1_IMAGE_MAX_PIXELS     (4096u * 4096u)

/*
 * OS1 stdimage baseline.
 *
 * Decoders return sanitized ARGB32 pixels only. Encoded bytes are read into a
 * bounded scratch buffer, metadata/script chunks are not exposed to callers, and
 * all display paths consume this inert pixel buffer instead of the source file.
 */
static inline os1_image_t *os1_image_load(const char *path) {
  int w = 0;
  int h = 0;
  uint32_t *raw_pixels = graphics_load_image(path, &w, &h);
  if (!raw_pixels) {
    return NULL;
  }

  os1_image_t *img = (os1_image_t *)malloc(sizeof(os1_image_t));
  if (!img) {
    free(raw_pixels);
    return NULL;
  }

  img->w = w;
  img->h = h;
  img->pixels = raw_pixels;
  return img;
}

static inline int os1_image_ext_eq(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return 0;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static inline int os1_image_path_has_known_ext(const char *path) {
  static const char *const exts[] = {
      ".png",  ".apng", ".jpg",  ".jpeg", ".jpe",  ".jfif", ".pjpeg",
      ".pjp",  ".bmp",  ".dib",  ".gif",  ".tga",  ".icb",  ".vda",
      ".vst",  ".psd",  ".pic",  ".ppm",  ".pgm",  ".pbm",  ".pnm",
      ".webp", ".tif",  ".tiff", ".ico",  ".cur",  ".qoi",  ".avif",
      ".heic", ".heif", ".jp2",  ".j2k",  ".jxl",  ".exr",  NULL};
  if (!path)
    return 0;
  const char *ext = strrchr(path, '.');
  if (!ext || ext == path)
    return 0;
  for (int i = 0; exts[i]; i++) {
    if (os1_image_ext_eq(ext, exts[i]))
      return 1;
  }
  return 0;
}

static inline int os1_image_path_has_decodable_ext(const char *path) {
  static const char *const exts[] = {
      ".png", ".apng", ".jpg", ".jpeg", ".jpe", ".jfif", ".pjpeg", ".pjp",
      ".bmp", ".dib",  ".gif", ".tga",  ".icb", ".vda",  ".vst",   ".psd",
      ".pic", ".ppm",  ".pgm", ".pbm",  ".pnm", NULL};
  if (!path)
    return 0;
  const char *ext = strrchr(path, '.');
  if (!ext || ext == path)
    return 0;
  for (int i = 0; exts[i]; i++) {
    if (os1_image_ext_eq(ext, exts[i]))
      return 1;
  }
  return 0;
}

static inline void os1_image_draw(int win_id, int x, int y, os1_image_t *img) {
  if (!img || !img->pixels || win_id < 0)
    return;
  graphics_blit(win_id, x, y, img->w, img->h, img->pixels);
}

static inline void os1_image_free(os1_image_t *img) {
  if (!img)
    return;
  free(img->pixels);
  free(img);
}

static inline int os1_image_fit_size(int src_w, int src_h, int max_w, int max_h,
                                     int allow_upscale, int *out_w,
                                     int *out_h) {
  if (src_w <= 0 || src_h <= 0 || max_w <= 0 || max_h <= 0 || !out_w || !out_h)
    return OS1_IMAGE_ERR_ARG;

  uint64_t w_by_h = (uint64_t)max_h * (uint64_t)src_w;
  uint64_t h_by_w = (uint64_t)max_w * (uint64_t)src_h;
  int w;
  int h;

  if (w_by_h <= h_by_w) {
    h = max_h;
    w = (int)(w_by_h / (uint64_t)src_h);
  } else {
    w = max_w;
    h = (int)(h_by_w / (uint64_t)src_w);
  }

  if (!allow_upscale && w >= src_w && h >= src_h) {
    w = src_w;
    h = src_h;
  }
  if (w < 1)
    w = 1;
  if (h < 1)
    h = 1;
  *out_w = w;
  *out_h = h;
  return OS1_IMAGE_OK;
}

static inline os1_image_t *os1_image_resample(const os1_image_t *src, int dst_w,
                                              int dst_h) {
  if (!src || !src->pixels || src->w <= 0 || src->h <= 0 || dst_w <= 0 ||
      dst_h <= 0)
    return NULL;
  if ((uint64_t)dst_w * (uint64_t)dst_h > OS1_IMAGE_MAX_PIXELS)
    return NULL;

  os1_image_t *dst = (os1_image_t *)malloc(sizeof(os1_image_t));
  if (!dst)
    return NULL;
  dst->pixels = (uint32_t *)malloc((size_t)dst_w * (size_t)dst_h * 4u);
  if (!dst->pixels) {
    free(dst);
    return NULL;
  }
  dst->w = dst_w;
  dst->h = dst_h;

  for (int y = 0; y < dst_h; y++) {
    int sy = (int)(((uint64_t)y * (uint64_t)src->h) / (uint64_t)dst_h);
    if (sy >= src->h)
      sy = src->h - 1;
    for (int x = 0; x < dst_w; x++) {
      int sx = (int)(((uint64_t)x * (uint64_t)src->w) / (uint64_t)dst_w);
      if (sx >= src->w)
        sx = src->w - 1;
      dst->pixels[y * dst_w + x] = src->pixels[sy * src->w + sx];
    }
  }
  return dst;
}

static inline os1_image_t *os1_image_resample_fit(const os1_image_t *src,
                                                  int max_w, int max_h,
                                                  int allow_upscale) {
  int w = 0;
  int h = 0;
  if (!src ||
      os1_image_fit_size(src->w, src->h, max_w, max_h, allow_upscale, &w, &h) !=
          OS1_IMAGE_OK)
    return NULL;
  return os1_image_resample(src, w, h);
}

#endif // OS1_IMAGE_H
