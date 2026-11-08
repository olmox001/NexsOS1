/*
 * NX-SDK image decoder extension.
 * The existing bounded OS1 file reader and ARGB conversion implementation
 * are moved here unchanged from the runtime TU. Third-party stb_image is an
 * external dependency supplied from user/lib/stb.
 */
#include "../../internal.h"
#include <image.h>
#include <os1.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-braces"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 4096
#include <stb_image.h>
/*
 * graphics_load_image - load an encoded image into sanitized ARGB32 pixels.
 *
 * Encoded input is treated as hostile data: it is copied into a bounded scratch
 * buffer, probed before decode, rejected on dimension/pixel/file caps, decoded
 * to a temporary RGBA plane, then copied into an OS1-owned ARGB buffer.  The
 * caller never sees encoded bytes or decoder-owned storage, which keeps image
 * rendering an inert pixel operation suitable for the stdimage base API.
 */

/* nx_image_read_file - read `path` whole into a malloc'd, size-capped, 16
 * bytes-past-the-end-zeroed buffer (the padding stb_image's memory reader
 * wants). R4: split out of graphics_load_image() so the read/decode/convert
 * stages each fit the ~60-line guideline on their own instead of one ~90
 * line function doing all three. Returns NULL and closes/frees everything
 * it opened on any failure; *out_size is only valid on success. */
static unsigned char *nx_image_read_file(const char *path, int *out_size) {
  long handle =
      OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ, OBJ_TYPE_FILE);
  if (handle < 0)
    return NULL;

  long stat_size = OS1_object_ctl((int)handle, OBJ_CTL_STAT, 0);
  if (stat_size <= 0 || (uint64_t)stat_size > OS1_IMAGE_MAX_FILE_BYTES) {
    (void)OS1low_handle_close((int)handle); /* R7: nothing more to do with
                                                a close failure on an
                                                already-rejected file */
    return NULL;
  }

  int size = (int)stat_size;
  unsigned char *data = malloc((size_t)size + 16u);
  if (!data) {
    (void)OS1low_handle_close((int)handle); /* R7: see rationale above */
    return NULL;
  }

  int total = 0;
  /* R2: bounded by `size` (OS1_IMAGE_MAX_FILE_BYTES-capped above); each
   * successful iteration strictly advances `total`, and a non-positive
   * `got` breaks out immediately. */
  while (total < size) {
    long got = OS1_object_read((int)handle, data + total,
                               (unsigned long)(size - total));
    if (got <= 0) {
      (void)OS1low_handle_close((int)handle); /* R7: read already failed;
                                                   the close result changes
                                                   nothing left to report */
      free(data);
      return NULL;
    }
    nx_assert(total + got <= size); /* R5 */
    total += (int)got;
  }
  (void)OS1low_handle_close((int)handle); /* R7: file is fully read; a close
                                              failure here cannot undo that */

  if (total != size) {
    free(data);
    return NULL;
  }
  for (int i = 0; i < 16; i++) /* R2: fixed 16-byte pad, see function doc */
    data[size + i] = 0;
  *out_size = size;
  return data;
}

uint32_t *graphics_load_image(const char *path, int *w, int *h) {
  if (!path || !w || !h)
    return NULL;

  *w = 0;
  *h = 0;

  int size = 0;
  unsigned char *data = nx_image_read_file(path, &size);
  if (!data)
    return NULL;

  int iw = 0;
  int ih = 0;
  int channels = 0;
  if (!stbi_info_from_memory(data, size, &iw, &ih, &channels)) {
    free(data);
    return NULL;
  }
  if (iw <= 0 || ih <= 0 || iw > OS1_IMAGE_MAX_DIMENSION ||
      ih > OS1_IMAGE_MAX_DIMENSION ||
      (uint64_t)iw * (uint64_t)ih > OS1_IMAGE_MAX_PIXELS) {
    free(data);
    return NULL;
  }

  int n = 0;
  unsigned char *rgba = stbi_load_from_memory(data, size, &iw, &ih, &n, 4);
  free(data);
  if (!rgba)
    return NULL;

  uint64_t pixels = (uint64_t)iw * (uint64_t)ih;
  if (pixels > OS1_IMAGE_MAX_PIXELS || pixels > ((uint64_t)SIZE_MAX / 4u)) {
    stbi_image_free(rgba);
    return NULL;
  }

  uint32_t *argb = (uint32_t *)malloc((size_t)pixels * 4u);
  if (!argb) {
    stbi_image_free(rgba);
    return NULL;
  }

  /* R2: bounded by `pixels`, itself capped above by OS1_IMAGE_MAX_PIXELS. */
  for (uint64_t i = 0; i < pixels; i++) {
    uint8_t r = rgba[i * 4u + 0u];
    uint8_t g = rgba[i * 4u + 1u];
    uint8_t b = rgba[i * 4u + 2u];
    uint8_t a = rgba[i * 4u + 3u];
    argb[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
              (uint32_t)b;
  }

  stbi_image_free(rgba);
  *w = iw;
  *h = ih;
  return argb;
}


#pragma GCC diagnostic pop
