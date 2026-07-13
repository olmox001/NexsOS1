/* SDL2 event pump translated from the NexsOS portability event stream. */
#include "../../sdl/src/SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_NEXSOS

#include "../../sdl/src/events/SDL_keyboard_c.h"
#include "../../sdl/src/events/SDL_mouse_c.h"
#include "../../sdl/src/events/SDL_events_c.h"
#include "SDL_nexsosvideo.h"
#include "../os1_video_platform.h"

/* Linux evdev keycode -> SDL scancode (the VirtIO input path delivers evdev
 * codes).  Table covers the full main block; unmapped codes stay UNKNOWN and
 * printable input still arrives through the UTF-8 text channel. */
static const SDL_Scancode nexsos_scancode_table[] = {
    /* 0 */ SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE,
    /* 2 */ SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    /* 6 */ SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
    /* 10 */ SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS,
    /* 13 */ SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB,
    /* 16 */ SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
    /* 20 */ SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I,
    /* 24 */ SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET,
    /* 27 */ SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_RETURN,
    /* 29 */ SDL_SCANCODE_LCTRL,
    /* 30 */ SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F,
    /* 34 */ SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K,
    /* 38 */ SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE,
    /* 41 */ SDL_SCANCODE_GRAVE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSLASH,
    /* 44 */ SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V,
    /* 48 */ SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M,
    /* 51 */ SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH,
    /* 54 */ SDL_SCANCODE_RSHIFT, SDL_SCANCODE_KP_MULTIPLY,
    /* 56 */ SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE, SDL_SCANCODE_CAPSLOCK,
    /* 59 */ SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3,
    /* 62 */ SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
    /* 65 */ SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9,
    /* 68 */ SDL_SCANCODE_F10,
};

static SDL_Scancode nexsos_scancode(uint32_t code) {
  if (code < SDL_arraysize(nexsos_scancode_table))
    return nexsos_scancode_table[code];
  switch (code) {
  case 87: return SDL_SCANCODE_F11;
  case 88: return SDL_SCANCODE_F12;
  case 97: return SDL_SCANCODE_RCTRL;
  case 100: return SDL_SCANCODE_RALT;
  case 102: return SDL_SCANCODE_HOME;
  case 103: return SDL_SCANCODE_UP;
  case 104: return SDL_SCANCODE_PAGEUP;
  case 105: return SDL_SCANCODE_LEFT;
  case 106: return SDL_SCANCODE_RIGHT;
  case 107: return SDL_SCANCODE_END;
  case 108: return SDL_SCANCODE_DOWN;
  case 109: return SDL_SCANCODE_PAGEDOWN;
  case 110: return SDL_SCANCODE_INSERT;
  case 111: return SDL_SCANCODE_DELETE;
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
