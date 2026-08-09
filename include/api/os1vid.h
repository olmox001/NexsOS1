#ifndef OS1_VID_H
#define OS1_VID_H

#include <os1.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OS1VID_MAX_FILE_BYTES (128u * 1024u * 1024u)
#define OS1VID_READ_CHUNK_BYTES (16u * 1024u * 1024u)

typedef struct plm_t plm_t;

typedef struct os1vid_s {
  plm_t *plm;
  uint32_t *pixels; /* Padded luma_width x luma_height for pl_mpeg decode */
  uint32_t
      *packed_pixels; /* Packed width x height for safe drawing/resampling */
  int width;
  int height;
  int luma_width;
  int luma_height;
  double framerate;
  int has_frame;
  int ended;
  unsigned long long last_ns;
  int paused;
} os1vid_t;

os1vid_t *os1vid_open(const char *path);
int os1vid_update(os1vid_t *v);
void os1vid_set_loop(os1vid_t *v, int loop);
int os1vid_has_ended(const os1vid_t *v);
void os1vid_pause(os1vid_t *v);
void os1vid_resume(os1vid_t *v);
int os1vid_is_paused(const os1vid_t *v);
int os1vid_seek(os1vid_t *v, double seconds);
double os1vid_get_time(const os1vid_t *v);
double os1vid_get_duration(const os1vid_t *v);
void os1vid_close(os1vid_t *v);

static inline int os1vid_path_has_known_ext(const char *path) {
  if (!path)
    return 0;
  const char *ext = 0;
  for (const char *p = path; *p; p++)
    if (*p == '.')
      ext = p;
  if (!ext)
    return 0;
  char a[8];
  int n = 0;
  for (const char *p = ext; *p && n < 7; p++, n++)
    a[n] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
  a[n] = '\0';
  return strcmp(a, ".mpg") == 0 || strcmp(a, ".mpeg") == 0;
}

/* ===================================================================== */
#ifdef OS1VID_IMPLEMENTATION

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

static uint8_t *os1vid_load_file(const char *path, size_t *out_size) {
  *out_size = 0;
  long handle =
      OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ, OBJ_TYPE_FILE);
  if (handle < 0)
    return 0;

  long stat_size = OS1_object_ctl((int)handle, OBJ_CTL_STAT, 0);
  if (stat_size <= 0 || (uint64_t)stat_size > OS1VID_MAX_FILE_BYTES) {
    OS1low_handle_close((int)handle);
    return 0;
  }

  size_t size = (size_t)stat_size;
  uint8_t *data = (uint8_t *)malloc(size);
  if (!data) {
    OS1low_handle_close((int)handle);
    return 0;
  }

  size_t total = 0;
  while (total < size) {
    size_t want = size - total;
    if (want > OS1VID_READ_CHUNK_BYTES)
      want = OS1VID_READ_CHUNK_BYTES;
    long got = OS1_object_read((int)handle, data + total, (unsigned long)want);
    if (got <= 0) {
      OS1low_handle_close((int)handle);
      free(data);
      return 0;
    }
    total += (size_t)got;
  }
  OS1low_handle_close((int)handle);

  if (total != size) {
    free(data);
    return 0;
  }

  *out_size = size;
  return data;
}

os1vid_t *os1vid_open(const char *path) {
  if (!path)
    return 0;

  size_t size = 0;
  uint8_t *data = os1vid_load_file(path, &size);
  if (!data)
    return 0;

  plm_t *plm = plm_create_with_memory(data, size, 1);
  if (!plm) {
    free(data);
    return 0;
  }

  int w = plm_get_width(plm);
  int h = plm_get_height(plm);
  if (w <= 0 || h <= 0) {
    plm_destroy(plm);
    return 0;
  }

  /* Calculate padded luma dimensions like pl_mpeg does internally (multiples of
   * 16) */
  int mb_w = (w + 15) >> 4;
  int mb_h = (h + 15) >> 4;
  int luma_w = mb_w << 4;
  int luma_h = mb_h << 4;

  os1vid_t *v = (os1vid_t *)malloc(sizeof(os1vid_t));
  if (!v) {
    plm_destroy(plm);
    return 0;
  }
  memset(v, 0, sizeof(*v));
  v->plm = plm;
  v->width = w;
  v->height = h;
  v->luma_width = luma_w;
  v->luma_height = luma_h;
  v->framerate = plm_get_framerate(plm);

  /* Allocate both padded (for decoder) and packed (for drawing) buffers */
  v->pixels = (uint32_t *)malloc((size_t)luma_w * (size_t)luma_h * 4u);
  v->packed_pixels = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
  if (!v->pixels || !v->packed_pixels) {
    plm_destroy(plm);
    free(v->pixels);
    free(v->packed_pixels);
    free(v);
    return 0;
  }

  for (int i = 0; i < w * h; i++)
    v->packed_pixels[i] = 0xFF000000u;
  for (int i = 0; i < luma_w * luma_h; i++)
    v->pixels[i] = 0xFF000000u;

  plm_set_audio_enabled(plm, 0);
  plm_set_loop(plm, 0);

  return v;
}

static void os1vid_video_cb(plm_t *plm, plm_frame_t *frame, void *user) {
  (void)plm;
  os1vid_t *v = (os1vid_t *)user;

  /* Decode into padded buffer with correct stride */
  plm_frame_to_bgra(frame, (uint8_t *)v->pixels, v->luma_width * 4);

  /* Pack it into packed_pixels (discard macroblock padding) */
  for (int y = 0; y < v->height; y++) {
    memcpy(&v->packed_pixels[y * v->width], &v->pixels[y * v->luma_width],
           v->width * 4);
  }
  v->has_frame = 1;
}

int os1vid_update(os1vid_t *v) {
  if (!v || !v->plm)
    return 0;
  v->has_frame = 0;

  unsigned long long now = os1_mono_ns();

  if (v->paused) {
    v->last_ns = now;
    return 0;
  }

  double dt;
  if (v->last_ns == 0) {
    dt = 0.0;
  } else {
    dt = (double)(now - v->last_ns) / 1e9;
    if (dt > 0.25)
      dt = 0.25;
  }
  v->last_ns = now;

  plm_set_video_decode_callback(v->plm, os1vid_video_cb, v);
  plm_decode(v->plm, dt);

  if (plm_has_ended(v->plm))
    v->ended = 1;

  return v->has_frame;
}

void os1vid_set_loop(os1vid_t *v, int loop) {
  if (v && v->plm)
    plm_set_loop(v->plm, loop);
}

int os1vid_has_ended(const os1vid_t *v) { return v ? v->ended : 1; }

void os1vid_pause(os1vid_t *v) {
  if (v)
    v->paused = 1;
}

void os1vid_resume(os1vid_t *v) {
  if (!v)
    return;
  v->paused = 0;
  v->last_ns = os1_mono_ns();
}

int os1vid_is_paused(const os1vid_t *v) { return v ? v->paused : 0; }

int os1vid_seek(os1vid_t *v, double seconds) {
  if (!v || !v->plm)
    return 0;

  double dur = plm_get_duration(v->plm);
  if (seconds < 0.0)
    seconds = 0.0;
  if (dur > 0.0 && seconds > dur)
    seconds = dur;

  plm_set_video_decode_callback(v->plm, os1vid_video_cb, v);
  int ok = plm_seek(v->plm, seconds, 1);
  if (!ok)
    return 0;

  v->last_ns = os1_mono_ns();
  if (dur <= 0.0 || seconds < dur)
    v->ended = 0;

  return 1;
}

double os1vid_get_time(const os1vid_t *v) {
  if (!v || !v->plm)
    return 0.0;
  return plm_get_time(v->plm);
}

double os1vid_get_duration(const os1vid_t *v) {
  if (!v || !v->plm)
    return 0.0;
  return plm_get_duration(v->plm);
}

void os1vid_close(os1vid_t *v) {
  if (!v)
    return;
  if (v->plm)
    plm_destroy(v->plm);
  free(v->pixels);
  free(v->packed_pixels);
  free(v);
}

#endif /* OS1VID_IMPLEMENTATION */
#endif /* OS1_VID_H */