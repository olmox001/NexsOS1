#ifndef OS1_VID_H
#define OS1_VID_H

/*
 * include/api/os1vid.h
 * OS1 video playback compatibility layer over pl_mpeg (MPEG1 + MP2, MIT).
 *
 * pl_mpeg.h ITSELF IS NOT MODIFIED. Download the unmodified upstream file
 * from:
 *
 *   https://raw.githubusercontent.com/phoboslab/pl_mpeg/master/pl_mpeg.h
 *
 * and place it, byte-for-byte as downloaded, at:
 *
 *   include/api/pl_mpeg.h
 *
 * This header is the ONLY thing that touches OS1: it feeds pl_mpeg an
 * in-memory buffer (loaded through the same capability-gated, size-capped
 * read path graphics_load_image() uses for images — see NOTE below), drives
 * plm_decode() with the real elapsed time from OS1_mono_ns(), converts the
 * decoded YCbCr frame into the engine's packed ARGB32 layout, and exposes a
 * tiny os1vid_* API that callers (nximage) use exactly like os1_image_t.
 *
 * SCOPE, STATED PLAINLY: pl_mpeg decodes MPEG1 video + MP2 audio out of an
 * MPEG-PS container — i.e. ".mpg"/".mpeg" files. It does NOT decode MP4, AVI,
 * MOV, H.264, HEVC, or any modern codec/container; those are not "the same
 * format with a different name," they are different specifications, and nothing
 * here or upstream reads them. To play an .mp4/.avi/.mov, transcode it first
 * (real ffmpeg invocation, from pl_mpeg's own README):
 *
 *   ffmpeg -i input.mp4 -c:v mpeg1video -c:a mp2 -format mpeg output.mpg
 *
 * AUDIO: this layer disables pl_mpeg's audio decoder (plm_set_audio_enabled)
 * because there is no real audio output device on this system yet — the SDL2
 * port's own audio backend is the `dummy` driver (see the Makefile's
 * SDL2_SRCS: only src/audio/dummy is built in). Decoding audio anyway would
 * cost cycles for a channel nothing can play. Video-only is the honest
 * scope, not a shortcut.
 *
 * NOTE (bounded in-memory load): there is no streaming file-read primitive
 * exposed to userland here (OS1_fs_read / handle_create+object_read always
 * read a full extent), so, like graphics_load_image(), this loads the whole
 * file into one capability-gated, size-capped buffer up front and hands
 * ownership of that buffer to pl_mpeg (plm_create_with_memory(...,
 * free_when_done=1)), which frees it internally via PLM_FREE when the plm_t
 * is destroyed. Callers never see or free that buffer themselves.
 */

#include <os1.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define OS1VID_MAX_FILE_BYTES (128u * 1024u * 1024u)
/* Must match kernel OBJ_MAX_IO_BYTES (object.c): object_read rejects n above
 * this, so whole-file loads must loop in chunks even when the file fits in
 * OS1VID_MAX_FILE_BYTES. */
#define OS1VID_READ_CHUNK_BYTES (16u * 1024u * 1024u)

typedef struct plm_t plm_t; /* opaque; full definition lives in pl_mpeg.h */

typedef struct os1vid_s {
  plm_t *plm;
  uint32_t *pixels; /* width*height ARGB32, alpha always 0xFF, engine-native
                     * layout — the same one graphics_load_image() produces,
                     * so callers hand this straight to window_blit(). */
  int width;
  int height;
  double framerate;   /* frames/sec reported by the stream; 0 if unknown */
  int has_frame;       /* a frame was (re)written into `pixels` since the last
                        * os1vid_update() the caller inspected */
  int ended;           /* stream reached EOF and is not looping */
  unsigned long long last_ns; /* os1_mono_ns() at the previous update() */
} os1vid_t;

/*
 * os1vid_open - load path, parse headers, size the ARGB32 frame buffer.
 * Returns NULL on any failure (missing/oversized file, undecodable stream,
 * no video track). errno is left set by the underlying OS1_fs_* call on a
 * file-level failure.
 */
os1vid_t *os1vid_open(const char *path);

/*
 * os1vid_update - advance playback by the REAL elapsed time since the last
 * call (via os1_mono_ns()), decoding as many frames as that time span calls
 * for. Returns 1 if `v->pixels` holds a newly decoded frame the caller should
 * blit, 0 if nothing changed (e.g. not enough time has passed yet, or the
 * stream has ended and looping is off).
 */
int os1vid_update(os1vid_t *v);

/* os1vid_set_loop - loop back to the start on EOF (default: off). */
void os1vid_set_loop(os1vid_t *v, int loop);

/* os1vid_has_ended - stream finished and is not looping. */
int os1vid_has_ended(const os1vid_t *v);

void os1vid_close(os1vid_t *v);

/* os1vid_path_has_known_ext - true for ".mpg"/".mpeg" (case-insensitive).
 * Mirrors os1_image_path_has_known_ext()'s shape in image.h, for callers
 * that dispatch on extension the way nximage does. */
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
/* pl_mpeg defaults PLM_MALLOC/PLM_FREE/PLM_REALLOC to malloc/free/realloc,
 * which this libc already provides (lib.c) — no override needed. PLM_NO_STDIO
 * is intentionally NOT defined: pl_mpeg's own fopen()-based path is unused
 * here (we always go through plm_create_with_memory), so it costs nothing to
 * leave in, and it keeps this file identical to a stock build of the library
 * for anyone diffing it against upstream. */
#include "pl_mpeg.h"

/*
 * os1vid_load_file - bounded, capability-gated whole-file read.
 * Same shape as graphics_load_image()'s file-loading half (lib.c): acquire a
 * READ-only FILE capability, reject anything over OS1VID_MAX_FILE_BYTES,
 * read to completion, and hand the raw buffer back for plm_create_with_memory
 * to take ownership of. Returns NULL and sets *out_size = 0 on any failure.
 */
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
    long got =
        OS1_object_read((int)handle, data + total, (unsigned long)want);
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

  /* free_when_done = 1: plm now owns `data` and will PLM_FREE() it in
   * plm_destroy(); we never touch it again from here. */
  plm_t *plm = plm_create_with_memory(data, size, 1);
  if (!plm) {
    free(data);
    return 0;
  }

  int w = plm_get_width(plm);
  int h = plm_get_height(plm);
  if (w <= 0 || h <= 0) {
    /* No decodable video track (e.g. audio-only PS, or not MPEG1 at all). */
    plm_destroy(plm);
    return 0;
  }

  os1vid_t *v = (os1vid_t *)malloc(sizeof(os1vid_t));
  if (!v) {
    plm_destroy(plm);
    return 0;
  }
  memset(v, 0, sizeof(*v));
  v->plm = plm;
  v->width = w;
  v->height = h;
  v->framerate = plm_get_framerate(plm);
  v->pixels = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
  if (!v->pixels) {
    plm_destroy(plm);
    free(v);
    return 0;
  }
  /* Pre-fill alpha to opaque: plm_frame_to_bgra() (see os1vid_video_cb) never
   * writes the 4th byte of each pixel, by design (see pl_mpeg.h's
   * PLM_DEFINE_FRAME_CONVERT_FUNCTION) — it assumes the caller wants to own
   * that byte, which here is always fully opaque video. */
  for (int i = 0; i < w * h; i++)
    v->pixels[i] = 0xFF000000u;

  plm_set_audio_enabled(plm, 0); /* no audio device to play it on; see header */
  plm_set_loop(plm, 0);

  return v;
}

/*
 * os1vid_video_cb - pl_mpeg's per-frame callback (plm_set_video_decode_callback).
 *
 * plm_frame_to_bgra with RI=2/GI=1/BI=0 (pl_mpeg.h's own `plm_frame_to_bgra`)
 * writes memory byte order B,G,R,(untouched) per pixel — which is exactly the
 * little-endian layout this engine's ARGB32 uint32_t already uses everywhere
 * (see graphics_load_image()'s `argb[i] = a<<24 | r<<16 | g<<8 | b`): the
 * untouched 4th byte is the alpha byte we pre-filled to 0xFF above and never
 * needs to move again, so no separate alpha pass runs per frame.
 */
static void os1vid_video_cb(plm_t *plm, plm_frame_t *frame, void *user) {
  (void)plm;
  os1vid_t *v = (os1vid_t *)user;
  plm_frame_to_bgra(frame, (uint8_t *)v->pixels, v->width * 4);
  v->has_frame = 1;
}

int os1vid_update(os1vid_t *v) {
  if (!v || !v->plm)
    return 0;
  v->has_frame = 0;

  if (!v->plm) /* defensive: mirrors the NULL guard every other OS1_* verb uses */
    return 0;

  unsigned long long now = os1_mono_ns();
  double dt;
  if (v->last_ns == 0) {
    dt = 0.0; /* first call: decode exactly one frame, no time has "passed" */
  } else {
    dt = (double)(now - v->last_ns) / 1e9;
    if (dt > 0.25)
      dt = 0.25; /* clamp a debugger-pause/scheduling-gap stall: avoid a
                  * multi-second decode burst trying to "catch up" */
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

void os1vid_close(os1vid_t *v) {
  if (!v)
    return;
  if (v->plm)
    plm_destroy(v->plm); /* also frees the file buffer we handed it, PLM_FREE */
  free(v->pixels);
  free(v);
}

#endif /* OS1VID_IMPLEMENTATION */
#endif /* OS1_VID_H */
