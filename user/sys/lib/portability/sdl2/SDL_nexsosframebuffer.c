/* SDL2 software framebuffer presentation over the NexsOS portability ABI. */
#include "../../sdl/src/SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_NEXSOS

#include "SDL_nexsosvideo.h"
#include "../os1_video_platform.h"

int NEXSOS_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format,
                                   void **pixels, int *pitch) {
  (void)_this;
  NEXSOS_WindowData *data = (NEXSOS_WindowData *)window->driverdata;
  NEXSOS_DestroyWindowFramebuffer(_this, window);
  /* Adopt the (possibly compositor-driven) size as the OS1 LOGICAL surface
   * before allocating the matching SDL framebuffer — the nxlauncher resize
   * pattern (GFX-DYN-01): the kernel no-ops when the size is unchanged, so
   * the first acquisition and grip-resize echoes cannot loop. */
  (void)os1_video_window_resize(data->os1_window, window->w, window->h);
  data->framebuffer = SDL_CreateRGBSurfaceWithFormat(
      0, window->w, window->h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!data->framebuffer)
    return -1;
  *format = SDL_PIXELFORMAT_ARGB8888;
  *pixels = data->framebuffer->pixels;
  *pitch = data->framebuffer->pitch;
  return 0;
}

int NEXSOS_UpdateWindowFramebuffer(_THIS, SDL_Window *window,
                                   const SDL_Rect *rects, int numrects) {
  (void)_this;
  NEXSOS_WindowData *data = (NEXSOS_WindowData *)window->driverdata;
  if (!data || !data->framebuffer)
    return SDL_SetError("NexsOS framebuffer unavailable");

  SDL_Surface *surface = data->framebuffer;
  int first = (numrects > 0) ? 0 : -1;
  int last = (numrects > 0) ? numrects : 1;
  SDL_Rect full = {0, 0, surface->w, surface->h};
  for (int i = first; i < last; i++) {
    SDL_Rect area = (i < 0) ? full : rects[i];
    if (!SDL_IntersectRect(&area, &full, &area))
      continue;
    const uint32_t *pixels = (const uint32_t *)surface->pixels +
                             (size_t)area.y * (surface->pitch / 4) + area.x;
    size_t stride = (size_t)(surface->pitch / 4);
    size_t source_count = (size_t)surface->h * stride;
    if (os1_video_present_argb8888_strided(data->os1_window, area.x, area.y,
                                           area.w, area.h, pixels, stride,
                                           source_count - (size_t)area.y * stride - area.x) < 0)
      return SDL_SetError("NexsOS framebuffer presentation failed");
  }
  os1_video_render();
  return 0;
}

void NEXSOS_DestroyWindowFramebuffer(_THIS, SDL_Window *window) {
  (void)_this;
  NEXSOS_WindowData *data = (NEXSOS_WindowData *)window->driverdata;
  if (data) {
    SDL_FreeSurface(data->framebuffer);
    data->framebuffer = NULL;
  }
}

#endif /* SDL_VIDEO_DRIVER_NEXSOS */
