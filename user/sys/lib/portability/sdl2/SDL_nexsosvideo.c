/* SDL2 NexsOS video driver overlay; built alongside SDL, never patched into it. */
#include "../../sdl/src/SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_NEXSOS

#include "SDL_video.h"
#include "SDL_hints.h"
#include "../../sdl/src/events/SDL_events_c.h"
#include "SDL_nexsosvideo.h"
#include "../os1_video_platform.h"

#define NEXSOSVID_DRIVER_NAME "nexsos"

/* Default-available: on NexsOS this is the only real driver, so it must be
 * picked with no hint set; an explicit hint for another driver still wins. */
static int NEXSOS_Available(void) {
  const char *requested = SDL_GetHint(SDL_HINT_VIDEODRIVER);
  return !requested || SDL_strcmp(requested, NEXSOSVID_DRIVER_NAME) == 0;
}

static void NEXSOS_DeleteDevice(SDL_VideoDevice *device) {
  SDL_free(device->driverdata);
  SDL_free(device);
}

static SDL_VideoDevice *NEXSOS_CreateDevice(void) {
  if (!NEXSOS_Available())
    return NULL;

  SDL_VideoDevice *device = SDL_calloc(1, sizeof(*device));
  NEXSOS_VideoData *data = SDL_calloc(1, sizeof(*data));
  if (!device || !data) {
    SDL_free(data);
    SDL_free(device);
    SDL_OutOfMemory();
    return NULL;
  }

  device->driverdata = data;
  device->VideoInit = NEXSOS_VideoInit;
  device->VideoQuit = NEXSOS_VideoQuit;
  device->PumpEvents = NEXSOS_PumpEvents;
  device->CreateSDLWindow = NEXSOS_CreateWindow;
  device->DestroyWindow = NEXSOS_DestroyWindow;
  device->SetWindowSize = NEXSOS_SetWindowSize;
  device->SetWindowTitle = NEXSOS_SetWindowTitle;
  device->CreateWindowFramebuffer = NEXSOS_CreateWindowFramebuffer;
  device->UpdateWindowFramebuffer = NEXSOS_UpdateWindowFramebuffer;
  device->DestroyWindowFramebuffer = NEXSOS_DestroyWindowFramebuffer;
  device->free = NEXSOS_DeleteDevice;
  return device;
}

VideoBootStrap NEXSOS_bootstrap = {
    NEXSOSVID_DRIVER_NAME, "NexsOS software video driver", NEXSOS_CreateDevice,
    NULL};

int NEXSOS_VideoInit(_THIS) {
  struct os1_video_info info;
  if (os1_video_query(&info) != 0)
    return SDL_SetError("NexsOS display unavailable");

  SDL_DisplayMode mode;
  SDL_zero(mode);
  mode.format = SDL_PIXELFORMAT_ARGB8888;
  mode.w = info.width;
  mode.h = info.height;
  if (SDL_AddBasicVideoDisplay(&mode) < 0)
    return -1;
  SDL_AddDisplayMode(&_this->displays[0], &mode);
  return 0;
}

void NEXSOS_VideoQuit(_THIS) { (void)_this; }

int NEXSOS_CreateWindow(_THIS, SDL_Window *window) {
  NEXSOS_WindowData *data = SDL_calloc(1, sizeof(*data));
  if (!data)
    return SDL_OutOfMemory();

  data->os1_window = os1_video_window_create(window->x, window->y, window->w,
                                              window->h, window->title);
  if (data->os1_window < 0) {
    SDL_free(data);
    return SDL_SetError("NexsOS window creation failed");
  }
  window->driverdata = data;
  window->flags &= ~SDL_WINDOW_HIDDEN;
  window->flags |= SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS;
  ((NEXSOS_VideoData *)_this->driverdata)->focused_window = window;
  SDL_SetMouseFocus(window);
  SDL_SetKeyboardFocus(window);
  return 0;
}

void NEXSOS_DestroyWindow(_THIS, SDL_Window *window) {
  NEXSOS_WindowData *data = (NEXSOS_WindowData *)window->driverdata;
  if (!data)
    return;
  NEXSOS_DestroyWindowFramebuffer(_this, window);
  os1_video_window_destroy(data->os1_window);
  if (((NEXSOS_VideoData *)_this->driverdata)->focused_window == window)
    ((NEXSOS_VideoData *)_this->driverdata)->focused_window = NULL;
  SDL_free(data);
  window->driverdata = NULL;
}

void NEXSOS_SetWindowSize(_THIS, SDL_Window *window) {
  (void)_this;
  NEXSOS_WindowData *data = (NEXSOS_WindowData *)window->driverdata;
  if (data && os1_video_window_resize(data->os1_window, window->w, window->h) < 0)
    SDL_SetError("NexsOS window resize failed");
}

void NEXSOS_SetWindowTitle(_THIS, SDL_Window *window) {
  (void)_this;
  (void)window;
  /* Title mutation awaits a capability-owned OS1 window-property operation. */
}

#endif /* SDL_VIDEO_DRIVER_NEXSOS */
