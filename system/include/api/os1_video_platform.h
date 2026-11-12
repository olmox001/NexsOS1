/*
 * OS1 video portability adapter.
 *
 * This is the only NexsOS-specific include used by the SDL, OpenGL and D3D9
 * ports. It exposes the current software-surface contract without pretending
 * that the kernel already supplies a hardware 3D API.
 */
#ifndef OS1_VIDEO_PLATFORM_H
#define OS1_VIDEO_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

enum os1_video_backend {
  OS1_VIDEO_BACKEND_SOFTWARE = 1,
  OS1_VIDEO_BACKEND_GPU_CONTEXT = 2, /* reserved: ASTRA GPU object */
};

enum os1_video_capability {
  OS1_VIDEO_CAP_ARGB8888 = 1u << 0,
  OS1_VIDEO_CAP_WINDOW_RESIZE = 1u << 1,
  OS1_VIDEO_CAP_DAMAGE_PRESENT = 1u << 2,
  OS1_VIDEO_CAP_SOFTWARE_3D = 1u << 3,
};

struct os1_video_info {
  uint32_t abi_version;
  uint32_t backend;
  uint32_t capabilities;
  int width;
  int height;
};

enum os1_video_event_type {
  OS1_VIDEO_EVENT_KEY = 1,
  OS1_VIDEO_EVENT_MOUSE_BUTTON = 2,
  OS1_VIDEO_EVENT_RESIZE = 3,
};

/* Stable event shape for SDL and D3D9 adapters. It deliberately does not
 * expose the kernel IPC packet or OS1's public input_event_t ABI. */
struct os1_video_event {
  uint32_t type;
  union {
    struct {
      uint32_t scancode;
      uint8_t key;
      uint8_t state;
      char utf8[8];
    } key;
    struct {
      int button;
      int state;
      int x;
      int y;
    } mouse;
    struct {
      int width;
      int height;
    } resize;
  } data;
};

int os1_video_query(struct os1_video_info *out);
int os1_video_window_create(int x, int y, int width, int height,
                            const char *title);
void os1_video_window_destroy(int window);
int os1_video_window_resize(int window, int width, int height);
int os1_video_present_argb8888(int window, int x, int y, int width,
                               int height, const uint32_t *pixels,
                               size_t pixel_count);
/* Present rows whose source pitch is expressed in pixels. This is used by
 * foreign software surfaces (notably SDL) whose pitch is not necessarily
 * width * 4. The kernel ABI still receives only a tightly packed region. */
int os1_video_present_argb8888_strided(int window, int x, int y, int width,
                                       int height, const uint32_t *pixels,
                                       size_t source_stride,
                                       size_t source_pixel_count);
/* Returns 1 with a translated event, 0 when none is queued, or a negative
 * OS1 error. Input delivery remains capability/focus mediated by the kernel. */
int os1_video_poll_event(struct os1_video_event *out);
void os1_video_render(void);

#endif /* OS1_VIDEO_PLATFORM_H */
