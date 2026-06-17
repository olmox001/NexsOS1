// sdl12_os1_shim.c
// Minimal SDL 1.2 shim -> OS1 (video + keyboard + mouse + timing). No audio.
// Drop into project and compile alongside your game source files.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* OS1 Native API Headers */
#include <graphics.h>
#include <input.h>
#include <os1.h>

/* --- Minimal SDL 1.2 API Declarations & Structures --- */

typedef uint32_t Uint32;
typedef uint16_t Uint16;
typedef uint8_t Uint8;
typedef int Sint16;
typedef int SDL_bool;

#define SDL_TRUE 1
#define SDL_FALSE 0

typedef struct {
  Sint16 x, y;
  Uint16 w, h;
} SDL_Rect;

typedef struct {
  Uint8 bits_per_pixel;
  Uint8 bytes_per_pixel;
  Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;

typedef struct {
  int flags;
  SDL_PixelFormat *format;
  int w, h;
  int pitch;    // Bytes per row
  void *pixels; // ARGB32 (0xAARRGGBB) matching OS1 graphics_blit
  SDL_Rect clip_rect;
} SDL_Surface;

/* SDL 1.2 Standard Keyboard Keysym Layout */
typedef struct {
  Uint8 scancode;
  int sym; // Virtual key symbol (ASCII or custom mapping)
  int mod;
  Uint16 unicode;
} SDL_keysym;

typedef struct {
  Uint8 type;
  Uint8 state;
  SDL_keysym keysym;
} SDL_KeyboardEvent;

typedef struct {
  Uint8 type;
  Uint8 state;
  Uint8 button;
  Uint16 x, y;
} SDL_MouseButtonEvent;

typedef struct {
  Uint8 type;
  Uint8 state;
  Uint16 x, y;
  Sint16 xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct {
  Uint8 type;
} SDL_QuitEvent;

typedef union {
  Uint8 type;
  SDL_KeyboardEvent key;
  SDL_MouseMotionEvent motion;
  SDL_MouseButtonEvent button;
  SDL_QuitEvent quit;
} SDL_Event;

/* Event Types definitions matching SDL 1.2 spec */
#define SDL_NOEVENT 0
#define SDL_ACTIVEEVENT 1
#define SDL_KEYDOWN 2
#define SDL_KEYUP 3
#define SDL_MOUSEMOTION 4
#define SDL_MOUSEBUTTONDOWN 5
#define SDL_MOUSEBUTTONUP 6
#define SDL_QUIT 12

/* Key States */
#define SDL_PRESSED 1
#define SDL_RELEASED 0

/* Mouse Buttons mapping */
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3

/* Init flags */
#define SDL_INIT_VIDEO 0x00000020

/* Global states for the compositor window and system loop */
static int shim_win = -1;
static SDL_Surface *shim_screen = NULL;
static SDL_PixelFormat shim_global_format;
static int last_mouse_x = 0;
static int last_mouse_y = 0;

/* Mappings for special keys from OS1 to standard SDL virtual keys */
static int os1_to_sdl_keysym(int os1_key, uint16_t scancode) {
  if (os1_key != 0)
    return os1_key; // Return standard ASCII if present

  switch (scancode) {
  case INPUT_KEY_ESC:
    return 27; // ESC character
  case INPUT_KEY_BACKSPACE:
    return 8; // BS
  case INPUT_KEY_TAB:
    return 9; // TAB
  case INPUT_KEY_ENTER:
    return 13; // CR
  case INPUT_KEY_UP:
    return 273; // SDL1.2 SDLK_UP
  case INPUT_KEY_DOWN:
    return 274; // SDL1.2 SDLK_DOWN
  case INPUT_KEY_RIGHT:
    return 275; // SDL1.2 SDLK_RIGHT
  case INPUT_KEY_LEFT:
    return 276; // SDL1.2 SDLK_LEFT
  default:
    return 0;
  }
}

/* Helper: Allocate and clean surface structure */
static SDL_Surface *shim_create_surface(int w, int h) {
  SDL_Surface *s = (SDL_Surface *)malloc(sizeof(SDL_Surface));
  if (!s)
    return NULL;

  s->w = w;
  s->h = h;
  s->pitch = w * 4;
  s->flags = 0;

  s->pixels = malloc(w * h * 4);
  if (!s->pixels) {
    free(s);
    return NULL;
  }
  memset(s->pixels, 0, w * h * 4);

  // Set format fields (ARGB 32-bit internal native structure)
  shim_global_format.bits_per_pixel = 32;
  shim_global_format.bytes_per_pixel = 4;
  shim_global_format.Amask = 0xFF000000;
  shim_global_format.Rmask = 0x00FF0000;
  shim_global_format.Gmask = 0x0000FF00;
  shim_global_format.Bmask = 0x000000FF;
  s->format = &shim_global_format;

  // Default boundaries for graphics safe-writes
  s->clip_rect.x = 0;
  s->clip_rect.y = 0;
  s->clip_rect.w = w;
  s->clip_rect.h = h;

  return s;
}

static void shim_free_surface(SDL_Surface *s) {
  if (!s)
    return;
  if (s->pixels)
    free(s->pixels);
  free(s);
}

/* --- Exposed SDL 1.2 Core Functions Implementation --- */

int SDL_Init(Uint32 flags) {
  (void)flags;
  // Window state initialized clean
  shim_win = -1;
  shim_screen = NULL;
  return 0;
}

void SDL_Quit(void) {
  if (shim_screen) {
    shim_free_surface(shim_screen);
    shim_screen = NULL;
  }
  if (shim_win >= 0) {
    destroy_window(shim_win);
    shim_win = -1;
  }
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags) {
  (void)flags;
  (void)bpp;

  if (shim_win >= 0) {
    destroy_window(shim_win);
    shim_win = -1;
  }

  // Request system window to compositor
  shim_win = create_window(100, 100, width, height, "OS1 SDL-Shim App");
  if (shim_win < 0)
    return NULL;

  if (shim_screen)
    shim_free_surface(shim_screen);
  shim_screen = shim_create_surface(width, height);

  return shim_screen;
}

int SDL_Flip(SDL_Surface *screen) {
  if (!screen)
    screen = shim_screen;
  if (!screen || shim_win < 0)
    return -1;

  // Push the internal surface pixels straight to the window buffer
  graphics_blit(shim_win, 0, 0, screen->w, screen->h,
                (const uint32_t *)screen->pixels);
  compositor_render();
  return 0;
}

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color) {
  if (!dst || !dst->pixels)
    return -1;

  int start_x = 0, start_y = 0, end_x = dst->w, end_y = dst->h;

  // If rect pointer is valid, isolate the bounding bounds
  if (rect) {
    start_x = rect->x;
    start_y = rect->y;
    end_x = rect->x + rect->w;
    end_y = rect->y + rect->h;

    // Boundaries Clipping enforcement
    if (start_x < 0)
      start_x = 0;
    if (start_y < 0)
      start_y = 0;
    if (end_x > dst->w)
      end_x = dst->w;
    if (end_y > dst->h)
      end_y = dst->h;
  }

  uint32_t *pixels = (uint32_t *)dst->pixels;
  for (int y = start_y; y < end_y; y++) {
    int row_offset = y * dst->w;
    for (int x = start_x; x < end_x; x++) {
      pixels[row_offset + x] = (uint32_t)color;
    }
  }
  return 0;
}

int SDL_PollEvent(SDL_Event *out) {
  if (!out)
    return 0;

  input_event_t ev;
  int has_event = input_poll_event(&ev);
  if (has_event <= 0)
    return 0; // 0 = no event pending, -1 = error

  memset(out, 0, sizeof(SDL_Event));

  // Convert OS1 Keyboard packet structures to target format
  if (ev.type == INPUT_TYPE_KEYBOARD) {
    out->type =
        (ev.keyboard.state == KEY_PRESSED || ev.keyboard.state == KEY_REPEAT)
            ? SDL_KEYDOWN
            : SDL_KEYUP;
    out->key.state =
        (ev.keyboard.state == KEY_PRESSED || ev.keyboard.state == KEY_REPEAT)
            ? SDL_PRESSED
            : SDL_RELEASED;
    out->key.keysym.scancode = (Uint8)ev.keyboard.scancode;
    out->key.keysym.sym =
        os1_to_sdl_keysym(ev.keyboard.key, ev.keyboard.scancode);
    return 1;
  }

  // Convert OS1 Mouse properties (input.h match state layout)
  else if (ev.type == INPUT_TYPE_MOUSE) {
    // Detect relative coordinates modifications to declare standard motions
    if (ev.mouse.x != last_mouse_x || ev.mouse.y != last_mouse_y) {
      out->type = SDL_MOUSEMOTION;
      out->motion.x = (Uint16)ev.mouse.x;
      out->motion.y = (Uint16)ev.mouse.y;
      out->motion.xrel = (Sint16)(ev.mouse.x - last_mouse_x);
      out->motion.yrel = (Sint16)(ev.mouse.y - last_mouse_y);

      last_mouse_x = ev.mouse.x;
      last_mouse_y = ev.mouse.y;
      return 1;
    }
    // Process fallback actions as mouse state triggers
    else {
      // OS1 properties mirror standard click layouts
      out->type =
          (ev.mouse.state == 1) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
      out->button.state = (ev.mouse.state == 1) ? SDL_PRESSED : SDL_RELEASED;
      out->button.x = (Uint16)ev.mouse.x;
      out->button.y = (Uint16)ev.mouse.y;
      out->button.button = (Uint8)ev.mouse.button;
      return 1;
    }
  }
  return 0;
}

/* Stable timing implementation built over high-res Tier 3 primitives */
Uint32 SDL_GetTicks(void) {
  // Converts Monotonic boot nanoseconds directly into milliseconds
  return (Uint32)(os1_mono_ns() / 1000000ULL);
}

void SDL_Delay(Uint32 ms) {
  if (ms == 0) {
    yield();
    return;
  }

  // High-resolution active sleeping strategy avoiding thread block degradation
  unsigned long long start_ns = os1_mono_ns();
  unsigned long long target_delay_ns = (unsigned long long)ms * 1000000ULL;

  // Check if the delta is large enough to allow low priority yielding
  if (ms >= 10) {
    // Safe internal call translation
    OS1_sleep(ms / 10);
  }

  // Spinlock high-res adjustment loop to prevent missing precision timings
  while ((os1_mono_ns() - start_ns) < target_delay_ns) {
    yield();
  }
}

/* --- Export Symbols & Stubs to safeguard common engine linkages --- */

#ifdef __GNUC__
__attribute__((visibility("default")))
#endif
int SDL_WM_ToggleFullScreen(SDL_Surface *s) {
  (void)s;
  return 0;
}

void SDL_WM_SetCaption(const char *title, const char *icon) {
  (void)icon;
  // Set window title dynamically if tracking context matches
  if (shim_win >= 0 && title) {
    // Flags update or recreate window sequence could go here if wanted
  }
}

int SDL_LockSurface(SDL_Surface *s) {
  (void)s;
  return 0;
}
void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }

/* End of shim code */