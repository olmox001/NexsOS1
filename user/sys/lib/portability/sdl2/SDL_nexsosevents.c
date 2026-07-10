/* SDL2 event pump translated from the NexsOS portability event stream. */
#include "../../sdl/src/SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_NEXSOS

#include "../../sdl/src/events/SDL_keyboard_c.h"
#include "../../sdl/src/events/SDL_mouse_c.h"
#include "../../sdl/src/events/SDL_events_c.h"
#include "SDL_nexsosvideo.h"
#include "../os1_video_platform.h"

static SDL_Scancode nexsos_scancode(uint32_t code) {
  switch (code) {
  case 1: return SDL_SCANCODE_ESCAPE;
  case 14: return SDL_SCANCODE_BACKSPACE;
  case 15: return SDL_SCANCODE_TAB;
  case 28: return SDL_SCANCODE_RETURN;
  case 57: return SDL_SCANCODE_SPACE;
  case 103: return SDL_SCANCODE_UP;
  case 105: return SDL_SCANCODE_LEFT;
  case 106: return SDL_SCANCODE_RIGHT;
  case 108: return SDL_SCANCODE_DOWN;
  default: return SDL_SCANCODE_UNKNOWN;
  }
}

static Uint8 nexsos_mouse_button(int button) {
  if (button == 0x110) return SDL_BUTTON_LEFT;
  if (button == 0x111) return SDL_BUTTON_RIGHT;
  if (button == 0x112) return SDL_BUTTON_MIDDLE;
  return 0;
}

void NEXSOS_PumpEvents(_THIS) {
  NEXSOS_VideoData *data = (NEXSOS_VideoData *)_this->driverdata;
  struct os1_video_event event;
  while (os1_video_poll_event(&event) > 0) {
    SDL_Window *window = data->focused_window;
    if (!window)
      continue;
    if (event.type == OS1_VIDEO_EVENT_KEY) {
      SDL_Scancode code = nexsos_scancode(event.data.key.scancode);
      SDL_SendKeyboardKey(event.data.key.state ? SDL_PRESSED : SDL_RELEASED,
                          code);
      if (event.data.key.state && event.data.key.utf8[0]) {
        char text[9];
        SDL_memcpy(text, event.data.key.utf8, 8);
        text[8] = '\0';
        SDL_SendKeyboardText(text);
      }
    } else if (event.type == OS1_VIDEO_EVENT_MOUSE_BUTTON) {
      Uint8 button = nexsos_mouse_button(event.data.mouse.button);
      SDL_SendMouseMotion(window, 0, 0, event.data.mouse.x, event.data.mouse.y);
      if (button)
        SDL_SendMouseButton(window, 0,
                            event.data.mouse.state ? SDL_PRESSED : SDL_RELEASED,
                            button);
    } else if (event.type == OS1_VIDEO_EVENT_RESIZE) {
      SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED,
                          event.data.resize.width, event.data.resize.height);
    }
  }
}

#endif /* SDL_VIDEO_DRIVER_NEXSOS */
