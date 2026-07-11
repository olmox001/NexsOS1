/*
 * user/bin/sdltest.c
 * First SDL2 application on NexsOS: exercises the whole port end to end —
 * libSDL2.a (cross-built, unpatched submodule) -> 'nexsos' video driver
 * overlay -> os1_video_platform -> OS1 window/blit/input ABI.
 *
 * Draws an animated colour gradient with a moving bar through
 * SDL_GetWindowSurface/SDL_UpdateWindowSurface (the software framebuffer
 * path) and quits on Escape or SDL_QUIT.  Frame pacing uses OS1_sleep since
 * the SDL timer subsystem is intentionally disabled in this profile.
 */
#include <os1.h>

#include "SDL.h"

#define TEST_W 320
#define TEST_H 240

int main(void) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    printf("[SDLTest] SDL_Init failed: %s\n", SDL_GetError());
    exit(1);
  }
  printf("[SDLTest] SDL %d.%d.%d, video driver: %s\n", SDL_MAJOR_VERSION,
         SDL_MINOR_VERSION, SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver());

  SDL_Window *window =
      SDL_CreateWindow("SDL2 on NexsOS", 80, 80, TEST_W, TEST_H, 0);
  if (!window) {
    printf("[SDLTest] SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    exit(1);
  }

  SDL_Surface *surface = SDL_GetWindowSurface(window);
  if (!surface) {
    printf("[SDLTest] SDL_GetWindowSurface failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    exit(1);
  }

  int running = 1;
  int frame = 0;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        running = 0;
      if (event.type == SDL_KEYDOWN &&
          event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
        running = 0;
    }

    /* STATIC gradient (any apparent motion of it is a presentation bug) +
     * three pure-channel squares for colour verification; the white bar is
     * the only element meant to move. */
    Uint32 *pixels = (Uint32 *)surface->pixels;
    int pitch_px = surface->pitch / 4;
    for (int y = 0; y < TEST_H; y++) {
      Uint8 g = (Uint8)((y * 255) / TEST_H);
      for (int x = 0; x < TEST_W; x++) {
        Uint8 r = (Uint8)((x * 255) / TEST_W);
        pixels[y * pitch_px + x] = 0xFF000000u | ((Uint32)r << 16) |
                                   ((Uint32)g << 8) | (Uint32)(255 - r);
      }
    }
    /* Channel check: pure red, green, blue squares (left to right). */
    SDL_Rect red = {12, 12, 32, 32}, green = {52, 12, 32, 32},
             blue = {92, 12, 32, 32};
    SDL_FillRect(surface, &red, SDL_MapRGB(surface->format, 255, 0, 0));
    SDL_FillRect(surface, &green, SDL_MapRGB(surface->format, 0, 255, 0));
    SDL_FillRect(surface, &blue, SDL_MapRGB(surface->format, 0, 0, 255));

    int bar_x = (frame * 3) % TEST_W;
    SDL_Rect bar = {bar_x, TEST_H - 24, 8, 24};
    SDL_FillRect(surface, &bar, SDL_MapRGB(surface->format, 255, 255, 255));

    if (SDL_UpdateWindowSurface(window) != 0) {
      printf("[SDLTest] present failed: %s\n", SDL_GetError());
      running = 0;
    }

    frame++;
    if (frame == 1)
      printf("[SDLTest] first frame presented via driver '%s'\n",
             SDL_GetCurrentVideoDriver());

    OS1_sleep(16); /* ~60 FPS; SDL timers are disabled in this profile */
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  printf("[SDLTest] clean shutdown\n");
  exit(0);
  return 0;
}
